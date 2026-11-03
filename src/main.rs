use std::env;
use std::fs;
use std::path::PathBuf;
use tms_editor::{
    discover, fetch_section, probe_scale, split_csv, upload_section, EditError, Editor,
    ScaleTransport, SCALE_DISCOVER_MS, SECTIONS,
};

const VERSION: &str = env!("TMS_APP_VERSION");

fn usage() {
    println!("Usage:\n  dlx50-tms-editor FILE.TMS                     open the GUI\n  dlx50-tms-editor FILE.TMS --list TAG          print rows to stdout\n  dlx50-tms-editor FILE.TMS --add TAG [NUMBER] [CODE] [NAME]\n      add one row (clone of the last, auto-numbered). PLU: NUMBER = PLU #, CODE = product code, NAME = product name.\n      PLU may also take --price VALUE (stored price, e.g. 799,2)\n  dlx50-tms-editor FILE.TMS --add TAG --csv FILE\n      bulk add: each line 'CODE,NAME' (header line skipped)\n  dlx50-tms-editor FILE.TMS --delete TAG CODE\n  dlx50-tms-editor FILE.TMS --set TAG ROW COL VALUE\n  dlx50-tms-editor FILE.TMS --delete-row TAG ROW\n  add --save-as PATH to write the result to PATH instead of FILE\n  dlx50-tms-editor --probe-scale HOST PORT [TIMEOUT_MS]\n      read-only network probe: sends a read-only UPL request and reports\n      what the scale answers (no write/delete/reboot/transfer sent)\n  dlx50-tms-editor --scale-live OUT.TMS\n      retrieve the first scale heard over the LAN into OUT.TMS (read-only;\n      exits 3 if no scale beacon is heard or a section fails)\n  dlx50-tms-editor --scale-upload FILE.TMS --allow-write\n      push every section of FILE.TMS to the first scale heard, then read each\n      section back and compare. Writing needs --allow-write; the framing is\n      not vendor-confirmed, so check the read-back result before trusting it\n  dlx50-tms-editor --scale-clock\n      set the scale's clock to this computer's local time (the device RTC\n      drifts on its own, so this is maintenance, not part of a transfer)\n  --version | --no-update-check | --help");
}
#[derive(Default)]
struct Args {
    file: Option<PathBuf>,
    tag: String,
    list: bool,
    add: bool,
    delete: Option<String>,
    set: Option<(usize, usize, String)>,
    delete_row: Option<usize>,
    csv: Option<PathBuf>,
    price: String,
    save_as: Option<PathBuf>,
    probe: Option<(String, u16, u64)>,
    scale_live: Option<PathBuf>,
    scale_upload: Option<PathBuf>,
    allow_write: bool,
    set_clock: bool,
    add_values: Vec<String>,
}
fn parse_args(args: &[String]) -> Result<Args, String> {
    let mut out = Args::default();
    if args.iter().any(|a| a.eq_ignore_ascii_case("--help")) {
        return Ok(out);
    }
    if args.iter().any(|a| a.eq_ignore_ascii_case("--version")) {
        return Ok(out);
    }
    if let Some(i) = args
        .iter()
        .position(|a| a.eq_ignore_ascii_case("--probe-scale"))
    {
        let host = args
            .get(i + 1)
            .filter(|s| !s.starts_with('-'))
            .ok_or("--probe-scale HOST PORT [TIMEOUT_MS] required")?
            .clone();
        let port: u16 = args
            .get(i + 2)
            .ok_or("--probe-scale HOST PORT [TIMEOUT_MS] required")?
            .parse()
            .map_err(|_| "probe PORT must be an integer from 1 to 65535")?;
        if port == 0 {
            return Err("probe PORT must be an integer from 1 to 65535".into());
        }
        let timeout = args
            .get(i + 3)
            .filter(|s| !s.starts_with('-'))
            .map_or(Ok(5000), |s| {
                s.parse::<u64>()
                    .map_err(|_| "probe TIMEOUT_MS must be an integer")
            })?;
        out.probe = Some((host, port, timeout));
        return Ok(out);
    }
    if let Some(i) = args
        .iter()
        .position(|a| a.eq_ignore_ascii_case("--scale-live"))
    {
        let out_path = args
            .get(i + 1)
            .filter(|s| !s.starts_with('-'))
            .ok_or("--scale-live OUT.TMS requires an output path")?;
        out.scale_live = Some(PathBuf::from(out_path));
        return Ok(out);
    }
    if let Some(i) = args
        .iter()
        .position(|a| a.eq_ignore_ascii_case("--scale-upload"))
    {
        let path = args
            .get(i + 1)
            .filter(|s| !s.starts_with('-'))
            .ok_or("--scale-upload FILE.TMS requires a file")?;
        out.scale_upload = Some(PathBuf::from(path));
        out.allow_write = args
            .iter()
            .any(|a| a.eq_ignore_ascii_case("--allow-write"));
        return Ok(out);
    }
    if args
        .iter()
        .any(|a| a.eq_ignore_ascii_case("--scale-clock"))
    {
        out.set_clock = true;
        return Ok(out);
    }
    if let Some(s) = args.iter().find(|s| !s.starts_with('-') && !s.is_empty()) {
        out.file = Some(PathBuf::from(s));
    }
    let mut i = 0;
    while i < args.len() {
        let a = args[i].as_str();
        match a.to_ascii_lowercase().as_str() {
            "--list" => {
                out.list = true;
                if let Some(v) = args.get(i + 1).filter(|v| !v.starts_with('-')) {
                    out.tag = v.clone();
                    i += 1;
                }
            }
            "--add" => {
                out.add = true;
                if let Some(v) = args.get(i + 1).filter(|v| !v.starts_with('-')) {
                    out.tag = v.clone();
                    i += 1;
                }
                i += 1;
                while i < args.len() {
                    let v = args[i].as_str();
                    if v.eq_ignore_ascii_case("--csv") {
                        out.csv = Some(PathBuf::from(
                            args.get(i + 1).ok_or("--csv requires a path")?,
                        ));
                        i += 2;
                        continue;
                    }
                    if v.eq_ignore_ascii_case("--price") {
                        out.price = args.get(i + 1).ok_or("--price requires a value")?.clone();
                        i += 2;
                        continue;
                    }
                    if v.starts_with('-') || out.add_values.len() >= 3 {
                        break;
                    }
                    out.add_values.push(v.into());
                    i += 1;
                }
                continue;
            }
            "--delete" => {
                let tag = args
                    .get(i + 1)
                    .filter(|v| !v.starts_with('-'))
                    .ok_or("--delete TAG CODE required")?;
                out.tag = tag.clone();
                out.delete = Some(
                    args.get(i + 2)
                        .filter(|v| !v.starts_with('-'))
                        .ok_or("--delete TAG CODE required")?
                        .clone(),
                );
                i += 2;
            }
            "--set" => {
                let tag = args.get(i + 1).ok_or("--set TAG ROW COL VALUE required")?;
                out.tag = tag.clone();
                let row = args
                    .get(i + 2)
                    .ok_or("--set TAG ROW COL VALUE required")?
                    .parse::<usize>()
                    .map_err(|_| "--set ROW and COL must be integers")?;
                let col = args
                    .get(i + 3)
                    .ok_or("--set TAG ROW COL VALUE required")?
                    .parse::<usize>()
                    .map_err(|_| "--set ROW and COL must be integers")?;
                let value = args
                    .get(i + 4)
                    .ok_or("--set TAG ROW COL VALUE required")?
                    .clone();
                out.set = Some((row, col, value));
                i += 4;
            }
            "--delete-row" => {
                let tag = args.get(i + 1).ok_or("--delete-row TAG ROW required")?;
                out.tag = tag.clone();
                let row = args
                    .get(i + 2)
                    .ok_or("--delete-row TAG ROW required")?
                    .parse::<usize>()
                    .map_err(|_| "--delete-row ROW must be an integer")?;
                out.delete_row = Some(row);
                i += 2;
            }
            "--save-as" => {
                out.save_as = Some(PathBuf::from(
                    args.get(i + 1).ok_or("--save-as requires a path")?,
                ));
                i += 1;
            }
            _ => {}
        }
        i += 1;
    }
    Ok(out)
}
fn csv_rows(path: &PathBuf) -> Result<Vec<(String, String)>, EditError> {
    let text = fs::read_to_string(path).map_err(EditError::Io)?;
    let mut rows = Vec::new();
    for (n, line) in text.lines().enumerate() {
        let line = line.trim();
        if line.is_empty() {
            continue;
        }
        let cells = split_csv(line);
        if n == 0
            && cells.first().is_some_and(|c| c.parse::<i64>().is_err())
            && cells.get(1).is_some_and(|c| c.parse::<i64>().is_err())
        {
            continue;
        }
        rows.push((
            cells.first().cloned().unwrap_or_default(),
            cells.get(1).cloned().unwrap_or_default(),
        ));
    }
    Ok(rows)
}
fn do_probe(host: &str, port: u16, timeout: u64) -> i32 {
    println!("dlx50-tms-editor network probe (issue #8)\n  target : {host}:{port}\n  timeout: {timeout} ms\n  mode   : read-only — sends a read-only UPL request for the INF section, no write/delete/reboot/transfer sent\n");
    let r = probe_scale(host, port, timeout);
    println!(
        "--- connection ---\n  connected : {}\n  host      : {}\n  port      : {}\n",
        if r.connected { "yes" } else { "no" },
        r.host,
        r.port
    );
    println!("--- observations ---");
    for o in &r.observations {
        println!("  {o}");
    }
    println!("\n--- captured response ---");
    if r.raw.is_empty() {
        println!("  (no bytes received)");
    } else {
        println!(
            "  bytes    : {}\n  hex       : {}",
            r.raw.len(),
            r.raw.iter().map(|b| format!("{b:02x}")).collect::<String>()
        );
        match std::str::from_utf8(&r.raw) {
            Ok(s) => println!(
                "  utf8      : {s:?}\n  lines     :\n{}",
                s.lines()
                    .map(|l| format!("    | {l}"))
                    .collect::<Vec<_>>()
                    .join("\n")
            ),
            Err(_) => println!("  utf8      : (not valid UTF-8 — see hex above)"),
        }
    }
    println!("\n--- summary for issue #8 ---\n{}\n\n--- next steps (paste into issue #7/8) ---\n  - target   : {}:{}\n  - connected: {}\n  - bytes    : {}\n  - probe    : read-only, no write/delete/reboot/transfer sent",r.summary,r.host,r.port,if r.connected{"yes"}else{"no"},r.raw.len());
    if r.connected {
        0
    } else {
        3
    }
}
/// `--scale-live OUT.TMS`: discover the first scale heard, say where it is,
/// then retrieve its live configuration. Read-only, like the probe.
/// Local time in the shape the device stores: "yy mm dd hh mm ss", tab-separated.
fn host_timestamp() -> Result<String, String> {
    #[cfg(unix)]
    let out = std::process::Command::new("date")
        .args(["+%y %-m %-d %H %M %S"])
        .output();
    #[cfg(windows)]
    let out = std::process::Command::new("powershell")
        .args([
            "-NoProfile",
            "-Command",
            "Get-Date -Format 'yy MM d HH mm ss'",
        ])
        .output();
    let out = out.map_err(|e| format!("cannot read the system clock: {e}"))?;
    if !out.status.success() {
        return Err("cannot read the system clock".into());
    }
    let text = String::from_utf8_lossy(&out.stdout);
    let parts: Vec<&str> = text.split_whitespace().collect();
    if parts.len() != 6 {
        return Err(format!("unexpected clock format: {text:?}"));
    }
    Ok(parts.join("\t"))
}

/// `--scale-clock`: set the device clock to this machine's local time. The
/// scale's RTC runs slow on its own, so this is maintenance, not a transfer.
fn do_scale_clock() -> i32 {
    let ts = match host_timestamp() {
        Ok(ts) => ts,
        Err(e) => {
            eprintln!("error: {e}");
            return 3;
        }
    };
    let ep = match discover(SCALE_DISCOVER_MS) {
        Ok(ep) => ep,
        Err(e) => {
            eprintln!("error: {e}");
            return 3;
        }
    };
    println!("scale endpoint: {}:{}", ep.host, ep.port);
    println!("setting the device clock to {ts}");
    if let Err(e) = upload_section(&ep, "TIM", format!("TIM\t{ts}\t\r\n").as_bytes()) {
        eprintln!("error: {e}");
        return 3;
    }
    match fetch_section(&ep, "TIM") {
        Ok(block) => {
            println!("device now: {:?}", String::from_utf8_lossy(&block));
            0
        }
        Err(e) => {
            eprintln!("error: {e}");
            3
        }
    }
}

/// `--scale-upload FILE.TMS --allow-write`: send every section, then verify by
/// reading each one back. The write framing is unconfirmed, so the read-back is
/// the result that counts.
fn do_scale_upload(local_path: &PathBuf) -> i32 {
    let mut transport = ScaleTransport::new();
    if let Err(e) = transport.list_files() {
        eprintln!("error: {e}");
        return 3;
    };
    if let Some(ep) = transport.endpoint() {
        println!("scale endpoint: {}:{}", ep.host, ep.port);
    }
    println!("writing {} to the scale…", local_path.display());
    match transport.upload(local_path, "LIVE") {
        Ok(()) => {
            println!("done: every section read back matches what was sent");
            0
        }
        Err(e) => {
            eprintln!("error: {e}");
            3
        }
    }
}
fn do_scale_live(out_path: &PathBuf) -> i32 {
    let mut transport = ScaleTransport::new();
    let files = match transport.list_files() {
        Ok(f) => f,
        Err(e) => {
            eprintln!("error: {e}");
            return 3;
        }
    };
    if let Some(ep) = transport.endpoint() {
        println!("scale endpoint: {}:{}", ep.host, ep.port);
    }
    if let Some(f) = files.first() {
        println!(
            "retrieving {}{} to {}",
            f.remote_path,
            if f.size == 0 {
                String::new()
            } else {
                format!(" ({} bytes)", f.size)
            },
            out_path.display()
        );
    }
    match transport.retrieve("LIVE", out_path) {
        Ok(()) => {
            println!(
                "retrieved the scale's live configuration to {}",
                out_path.display()
            );
            0
        }
        Err(e) => {
            eprintln!("error: {e}");
            3
        }
    }
}
fn run(args: Vec<String>) -> i32 {
    if args.iter().any(|a| a.eq_ignore_ascii_case("--version")) {
        println!("dlx50-tms-editor {VERSION}");
        return 0;
    }
    if args.iter().any(|a| a.eq_ignore_ascii_case("--help")) {
        usage();
        return 0;
    }
    let parsed = match parse_args(&args) {
        Ok(v) => v,
        Err(e) => {
            eprintln!("error: {e}");
            usage();
            return 2;
        }
    };
    if let Some((h, p, t)) = parsed.probe {
        return do_probe(&h, p, t);
    }
    if let Some(out_path) = parsed.scale_live {
        return do_scale_live(&out_path);
    }
    if parsed.set_clock {
        return do_scale_clock();
    }
    if let Some(local_path) = parsed.scale_upload {
        if !parsed.allow_write {
            eprintln!(
                "error: --scale-upload writes to the scale; re-run with --allow-write to confirm"
            );
            return 2;
        }
        return do_scale_upload(&local_path);
    }
    let Some(file) = parsed.file else {
        if parsed.list
            || parsed.add
            || parsed.delete.is_some()
            || parsed.set.is_some()
            || parsed.delete_row.is_some()
        {
            eprintln!("error: no .TMS file given");
            usage();
            return 3;
        }
        return launch_gui(None);
    };
    let cli = parsed.list
        || parsed.add
        || parsed.delete.is_some()
        || parsed.set.is_some()
        || parsed.delete_row.is_some();
    if !cli {
        return launch_gui(Some(file));
    }
    if parsed.tag.is_empty() {
        eprintln!(
            "error: missing section tag after the operation flag ({})",
            SECTIONS.join(", ")
        );
        usage();
        return 2;
    }
    let mut editor = match Editor::open(&file) {
        Ok(e) => e,
        Err(e) => {
            eprintln!("error: could not load {}: {e}", file.display());
            return 3;
        }
    };
    if parsed.list
        && !parsed.add
        && parsed.delete.is_none()
        && parsed.set.is_none()
        && parsed.delete_row.is_none()
        && parsed.csv.is_none()
    {
        match editor.list(&parsed.tag) {
            Ok(rows) => {
                for row in rows {
                    println!("{}", row.join("\t"));
                }
                return 0;
            }
            Err(e) => {
                eprintln!("error: {e}");
                return 2;
            }
        }
    }
    let mut failures = 0;
    let mut success = 0;
    if parsed.add {
        if let Some(csv) = &parsed.csv {
            match csv_rows(csv) {
                Ok(rows) => {
                    for (code, name) in rows {
                        if parsed.tag == "PLU" && !code.is_empty() { /* code supported only on layout A; core reports layout errors. */
                        }
                        match editor.add(
                            &parsed.tag,
                            "",
                            &code,
                            &name,
                            if parsed.tag == "PLU" {
                                &parsed.price
                            } else {
                                ""
                            },
                        ) {
                            Ok(()) => success += 1,
                            Err(e) => {
                                failures += 1;
                                eprintln!("row: {e}");
                            }
                        }
                    }
                }
                Err(e) => {
                    eprintln!("error: cannot open csv {}: {e}", csv.display());
                    return 3;
                }
            }
        } else {
            let number = parsed.add_values.first().map(String::as_str).unwrap_or("");
            let code = parsed.add_values.get(1).map(String::as_str).unwrap_or("");
            let name = parsed.add_values.get(2).map(String::as_str).unwrap_or("");
            let (code, name) = if parsed.tag == "PLU" {
                (code, name)
            } else {
                (if number.is_empty() { code } else { number }, name)
            };
            match editor.add(&parsed.tag, number, code, name, &parsed.price) {
                Ok(()) => success += 1,
                Err(e) => {
                    failures += 1;
                    eprintln!("error: {e}");
                }
            }
        }
    }
    if let Some(code) = parsed.delete.as_ref() {
        match editor.delete_code(&parsed.tag, code) {
            Ok(n) => success += n,
            Err(e) => {
                failures += 1;
                eprintln!("error: {e}");
            }
        }
    }
    if let Some((r, c, v)) = parsed.set.as_ref() {
        match editor.set(&parsed.tag, *r, *c, v) {
            Ok(()) => success += 1,
            Err(e) => {
                failures += 1;
                eprintln!("error: {e}");
            }
        }
    }
    if let Some(row) = parsed.delete_row {
        match editor.delete_row(&parsed.tag, row) {
            Ok(()) => success += 1,
            Err(e) => {
                failures += 1;
                eprintln!("error: {e}");
            }
        }
    }
    if failures > 0 {
        return 4;
    }
    if success == 0 {
        eprintln!("error: nothing to do");
        usage();
        return 2;
    }
    let target = parsed.save_as.as_ref().unwrap_or(&file);
    if let Err(e) = if parsed.save_as.is_some() {
        editor.save_as(target)
    } else {
        editor.save(target)
    } {
        eprintln!("error: {e}");
        return 3;
    }
    if parsed.save_as.is_some() {
        println!(
            "{}: {success} change(s); saved {}",
            parsed.tag,
            target.display()
        );
    } else {
        let what = if parsed.delete.is_some() || parsed.delete_row.is_some() {
            "deleted"
        } else if parsed.set.is_some() {
            "edited"
        } else {
            "added"
        };
        println!(
            "{}: {what} {success} row(s); saved {} (backup: {}.BAK)",
            parsed.tag,
            file.display(),
            file.display()
        );
    }
    0
}
fn launch_gui(path: Option<PathBuf>) -> i32 {
    if let Err(e) = tms_editor::ui::launch(path) {
        eprintln!("error: could not start desktop interface: {e}");
        return 1;
    }
    0
}
fn main() {
    std::process::exit(run(env::args().skip(1).collect()));
}
