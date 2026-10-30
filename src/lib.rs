use std::fs;
use std::io::{self, Read, Write};
use std::net::{TcpStream, ToSocketAddrs};
use std::path::Path;
use std::time::{Duration, Instant};

pub mod ui;
pub mod updater;

pub const SECTIONS: &[&str] = &[
    "PLU", "DPT", "CLS", "UNT", "BAR", "TMS", "TMR", "INF", "TMT", "REP", "SCP",
];
const REBUILT_TAGS: &[&str] = &[
    "PLU", "DPT", "CLS", "UNT", "BAR", "LAB", "SAL", "REP", "SCF", "SCP", "TMS", "TMT", "TMR",
    "INF", "TIM", "BMP",
];

#[derive(Clone, Debug)]
struct Row {
    tokens: Vec<String>,
    offset: usize,
    special_idx: Option<usize>,
}

#[derive(Clone, Debug)]
struct Section {
    rows: Vec<Row>,
}

#[derive(Clone, Debug)]
pub struct Editor {
    raw: String,
    sections: Vec<(String, Section)>,
}

#[derive(Debug)]
pub enum EditError {
    Io(io::Error),
    Usage(String),
    Operation(String),
}
impl std::fmt::Display for EditError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Io(e) => write!(f, "{e}"),
            Self::Usage(s) | Self::Operation(s) => f.write_str(s),
        }
    }
}
impl From<io::Error> for EditError {
    fn from(e: io::Error) -> Self {
        Self::Io(e)
    }
}

fn marker_index(text: &str, marker: &str, from: usize) -> Option<usize> {
    let mut pos = from;
    while let Some(rel) = text.get(pos..)?.find(marker) {
        let found = pos + rel;
        let after = found + marker.len();
        if after == text.len()
            || text
                .as_bytes()
                .get(after)
                .is_some_and(|b| matches!(*b, b'\t' | b'\r' | b'\n'))
        {
            return Some(found);
        }
        pos = after;
    }
    None
}
fn parse_rows(block: &str, tag: &str) -> Vec<Row> {
    let start = format!("DWL\t{tag}");
    let end = format!("END\t{tag}");
    let mut body = block.strip_prefix(&start).unwrap_or(block);
    body = body.strip_prefix('\t').unwrap_or(body);
    body = body.strip_prefix('\r').unwrap_or(body);
    body = body.strip_prefix('\n').unwrap_or(body);
    body = body.strip_suffix(&end).unwrap_or(body);
    body = body.strip_suffix('\t').unwrap_or(body);
    body = body.strip_suffix('\r').unwrap_or(body);
    body = body.strip_suffix('\n').unwrap_or(body);
    body.split('\n')
        .filter_map(|line| {
            let line = line.strip_suffix('\r').unwrap_or(line);
            if line.is_empty() {
                return None;
            }
            let mut tokens: Vec<String> = line.split('\t').map(str::to_owned).collect();
            while tokens.len() > 1 && tokens[0] == tag && tokens[1] == tag {
                tokens.remove(0);
            }
            let offset = usize::from(tokens.first().is_some_and(|t| t == tag));
            let special_idx = if tag == "PLU" {
                plu_name_idx(&tokens, offset)
            } else if tag == "BAR" {
                Some(barcode_idx(&tokens, offset))
            } else {
                None
            };
            Some(Row {
                tokens,
                offset,
                special_idx,
            })
        })
        .collect()
}
fn plu_name_idx(tokens: &[String], base: usize) -> Option<usize> {
    (base + 1..tokens.len()).find(|&i| tokens[i].chars().any(|c| c.is_ascii_alphabetic()))
}
fn barcode_idx(tokens: &[String], offset: usize) -> usize {
    let mut last_placeholder: Option<usize> = None;
    for (i, token) in tokens.iter().enumerate().skip(offset + 1) {
        if token.is_empty() {
            continue;
        }
        if token.parse::<i64>().is_ok() {
            last_placeholder = Some(i);
        } else {
            return i;
        }
    }
    last_placeholder.map_or(offset + 6, |i| i + 1)
}
fn default_cols(tag: &str) -> usize {
    match tag {
        "PLU" => 5,
        "DPT" | "CLS" | "BAR" | "TMS" | "INF" | "TMT" | "REP" => 2,
        "UNT" | "SCP" => 3,
        "TMR" => 1,
        _ => 1,
    }
}
pub fn headers(tag: &str) -> &'static [&'static str] {
    match tag {
        "PLU" => &[
            "PLU #",
            "Product code",
            "Product name",
            "Price (as stored)",
            "Rest",
        ],
        "DPT" => &["DPT code", "Value / description"],
        "CLS" => &["CLS code", "Name"],
        "UNT" => &["Unit code", "Description", "Factor"],
        "BAR" => &["Barcode #", "Barcode data"],
        "TMS" => &["TMS ID", "Value"],
        "TMR" => &["TMR data"],
        "INF" => &["Field", "Value"],
        "TMT" => &["Code", "Message"],
        "REP" => &["REP code", "Data"],
        "SCP" => &["SCP code", "Subcode", "Value"],
        _ => &["Column"],
    }
}
fn display_row(tag: &str, row: &Row) -> Vec<String> {
    let t = &row.tokens;
    let o = row.offset;
    if tag == "PLU" {
        let ni = row.special_idx;
        let name = ni
            .map(|i| t[i].clone())
            .unwrap_or_else(|| t.get(o + 1).cloned().unwrap_or_default());
        let code = if ni == Some(o + 1) {
            String::new()
        } else {
            t.get(o + 1).cloned().unwrap_or_default()
        };
        let price = if ni.is_some_and(|i| i > o + 4) {
            t.get(o + 4).cloned().unwrap_or_default()
        } else {
            String::new()
        };
        let rest = ni
            .map(|i| t.iter().skip(i + 1).cloned().collect::<Vec<_>>().join("\t"))
            .unwrap_or_default();
        vec![
            t.get(o).cloned().unwrap_or_default(),
            code,
            name,
            price,
            rest,
        ]
    } else if tag == "BAR" {
        let bi = row.special_idx.unwrap_or_else(|| barcode_idx(t, o));
        vec![
            t.get(o).cloned().unwrap_or_default(),
            t.get(bi).cloned().unwrap_or_default(),
        ]
    } else {
        (0..default_cols(tag))
            .map(|i| t.get(o + i).cloned().unwrap_or_default())
            .collect()
    }
}

impl Editor {
    pub fn from_bytes(bytes: &[u8]) -> Result<Self, EditError> {
        let raw = String::from_utf8_lossy(bytes).into_owned();
        let mut sections = Vec::new();
        for tag in REBUILT_TAGS {
            let start = format!("DWL\t{tag}");
            let end = format!("END\t{tag}");
            let parsed = marker_index(&raw, &start, 0)
                .and_then(|s| {
                    marker_index(&raw, &end, s + start.len())
                        .map(|e| parse_rows(&raw[s..e + end.len()], tag))
                })
                .unwrap_or_default();
            sections.push(((*tag).to_owned(), Section { rows: parsed }));
        }
        Ok(Self { raw, sections })
    }
    pub fn open(path: impl AsRef<Path>) -> Result<Self, EditError> {
        Self::from_bytes(&fs::read(path)?)
    }
    fn section(&self, tag: &str) -> Option<&Section> {
        self.sections.iter().find(|(t, _)| t == tag).map(|(_, s)| s)
    }
    fn section_mut(&mut self, tag: &str) -> Option<&mut Section> {
        self.sections
            .iter_mut()
            .find(|(t, _)| t == tag)
            .map(|(_, s)| s)
    }
    pub fn list(&self, tag: &str) -> Result<Vec<Vec<String>>, EditError> {
        let s = self
            .section(tag)
            .ok_or_else(|| EditError::Usage(format!("no such section: {tag}")))?;
        let mut out = vec![headers(tag).iter().map(|s| (*s).to_owned()).collect()];
        out.extend(s.rows.iter().map(|r| display_row(tag, r)));
        Ok(out)
    }
    pub fn add(
        &mut self,
        tag: &str,
        number: &str,
        code: &str,
        name: &str,
        price: &str,
    ) -> Result<(), EditError> {
        let s = self
            .section_mut(tag)
            .ok_or_else(|| EditError::Operation(format!("no such section: {tag}")))?;
        let (mut row, offset) = if let Some(last) = s.rows.last() {
            (last.tokens.clone(), last.offset)
        } else if tag == "PLU" {
            return Err(EditError::Operation("PLU has no rows to clone and cannot be bootstrapped — open a file that already has PLU entries".into()));
        } else {
            (vec![tag.into(), "0".into()], 1)
        };
        let num_idx = offset;
        if let Some(cur) = row.get(num_idx).and_then(|v| v.parse::<i64>().ok()) {
            let max = s
                .rows
                .iter()
                .filter_map(|r| r.tokens.get(num_idx)?.parse::<i64>().ok())
                .max()
                .unwrap_or(cur);
            if num_idx < row.len() {
                row[num_idx] = (max + 1).to_string();
            }
        }
        let mut si = if tag == "PLU" {
            plu_name_idx(&row, offset)
        } else if tag == "BAR" {
            Some(barcode_idx(&row, offset))
        } else {
            None
        };
        if tag == "PLU" {
            let ni = si.unwrap_or_else(|| {
                row.insert(offset + 1, "NEW PRODUCT".into());
                offset + 1
            });
            if ni < row.len() {
                row[ni] = "NEW PRODUCT".into();
            }
            if ni > offset + 1 && offset + 1 < row.len() {
                let max = s
                    .rows
                    .iter()
                    .filter_map(|r| r.tokens.get(offset + 1)?.parse::<i64>().ok())
                    .max();
                row[offset + 1] = max.map(|x| (x + 1).to_string()).unwrap_or_default();
            }
            si = Some(ni);
            if !code.is_empty() && (ni == offset + 1 || offset + 1 >= row.len()) {
                return Err(EditError::Operation(format!("this file stores no product code on a PLU row (the name follows the PLU number), so code '{code}' cannot be saved — add without a code, or match rows by PLU number: --delete PLU <number>")));
            }
            if !number.is_empty() {
                row[offset] = number.into();
            }
            if !code.is_empty() && ni != offset + 1 {
                row[offset + 1] = code.into();
            }
            if !name.is_empty() && ni < row.len() {
                row[ni] = name.into();
            }
            if !price.is_empty() && ni > offset + 4 && offset + 4 < row.len() {
                row[offset + 4] = price.into();
            }
        } else if tag == "BAR" {
            let barcode = si.unwrap_or(offset + 6);
            if barcode >= row.len() {
                row.resize(barcode + 1, String::new());
            }
            row[barcode] = String::new();
            if !number.is_empty() {
                row[offset] = number.into();
            }
            if !name.is_empty() {
                row[barcode] = name.into();
            }
        } else {
            if !code.is_empty() {
                row[offset] = code.into();
            } else if !number.is_empty() {
                row[offset] = number.into();
            }
            if !name.is_empty() {
                if row.len() <= offset + 1 {
                    row.resize(offset + 2, String::new());
                }
                row[offset + 1] = name.into();
            }
        }
        s.rows.push(Row {
            tokens: row,
            offset,
            special_idx: si,
        });
        Ok(())
    }
    pub fn set(&mut self, tag: &str, row: usize, col: usize, value: &str) -> Result<(), EditError> {
        let s = self
            .section_mut(tag)
            .ok_or_else(|| EditError::Operation(format!("no such section: {tag}")))?;
        let count = s.rows.len();
        let r = s.rows.get_mut(row).ok_or_else(|| {
            EditError::Operation(format!(
                "cell {row},{col} is outside the {tag} table ({count} rows, {} columns)",
                default_cols(tag)
            ))
        })?;
        let cols = default_cols(tag);
        if col >= cols {
            return Err(EditError::Operation(format!(
                "cell {row},{col} is outside the {tag} table ({count} rows, {cols} columns)"
            )));
        }
        let o = r.offset;
        if tag == "PLU" {
            let idx = match col {
                0 => Some(o),
                1 => (r.special_idx != Some(o + 1)).then_some(o + 1),
                2 => r.special_idx,
                3 => Some(o + 4).filter(|i| r.special_idx.is_some_and(|n| n > *i)),
                4 => r.special_idx.map(|i| i + 1),
                _ => None,
            };
            if let Some(i) = idx {
                if i < r.tokens.len() {
                    if col == 4 {
                        let mut toks: Vec<String> = r.tokens[..i].to_vec();
                        toks.extend(value.split('\t').map(str::to_owned));
                        r.tokens = toks;
                    } else {
                        r.tokens[i] = value.into();
                    }
                }
            }
        } else if tag == "BAR" {
            let idx = if col == 0 { Some(o) } else { r.special_idx };
            if let Some(i) = idx {
                if i >= r.tokens.len() {
                    r.tokens.resize(i + 1, String::new());
                }
                r.tokens[i] = value.into();
            }
        } else {
            let idx = o + col;
            if idx >= r.tokens.len() {
                r.tokens.resize(idx + 1, String::new());
            }
            r.tokens[idx] = value.into();
        }
        Ok(())
    }
    pub fn delete_row(&mut self, tag: &str, row: usize) -> Result<(), EditError> {
        let s = self
            .section_mut(tag)
            .ok_or_else(|| EditError::Operation(format!("no such section: {tag}")))?;
        if row >= s.rows.len() {
            return Err(EditError::Operation(format!(
                "row {row} is outside the {tag} table ({} rows)",
                s.rows.len()
            )));
        }
        s.rows.remove(row);
        Ok(())
    }
    pub fn delete_code(&mut self, tag: &str, code: &str) -> Result<usize, EditError> {
        if code.is_empty() {
            return Err(EditError::Operation("empty code — nothing to match".into()));
        }
        let s = self
            .section_mut(tag)
            .ok_or_else(|| EditError::Operation(format!("no such section: {tag}")))?;
        let before = s.rows.len();
        s.rows.retain(|r| {
            let d = display_row(tag, r);
            let matches = if tag == "PLU" {
                d.get(1).is_some_and(|v| v == code) || d.first().is_some_and(|v| v == code)
            } else {
                d.first().is_some_and(|v| v == code) || d.get(1).is_some_and(|v| v == code)
            };
            !matches
        });
        let n = before - s.rows.len();
        if n == 0 {
            Err(EditError::Operation(format!(
                "no {tag} row with code '{code}'"
            )))
        } else {
            Ok(n)
        }
    }
    pub fn serialize(&self) -> String {
        let mut content = self.raw.clone();
        for (tag, section) in &self.sections {
            let start = format!("DWL\t{tag}");
            let end = format!("END\t{tag}");
            let Some(start_idx0) = marker_index(&content, &start, 0) else {
                continue;
            };
            let Some(end_idx) = marker_index(&content, &end, start_idx0 + start.len()) else {
                continue;
            };
            let block_end = end_idx + end.len();
            let mut start_idx = start_idx0;
            while start_idx > 0 && matches!(content.as_bytes()[start_idx - 1], b'\t' | b' ') {
                start_idx -= 1;
            }
            let line = |s: &str| {
                if s.ends_with('\t') {
                    format!("{s}\r\n")
                } else {
                    format!("{s}\t\r\n")
                }
            };
            let mut block = String::new();
            if start_idx > 0 && !content[..start_idx].ends_with('\n') {
                block.push_str("\r\n");
            }
            block.push_str(&line(&start));
            for row in &section.rows {
                block.push_str(&line(&row.tokens.join("\t")));
            }
            block.push_str(&line(&end));
            let marker_tail = content[block_end..].trim_start_matches(['\t', ' ']);
            if marker_tail.starts_with("\r\n\r\n") || marker_tail.starts_with("\n\n") {
                block.push_str("\r\n");
            }
            let mut after = block_end;
            while after < content.len()
                && matches!(content.as_bytes()[after], b'\t' | b' ' | b'\r' | b'\n')
            {
                after += 1;
            }
            content.replace_range(start_idx..after, &block);
        }
        if let Some(i) = content.rfind("END\tECS") {
            if content[i..].trim_matches(|c| c == ' ' || c == '\t' || c == '\r' || c == '\n')
                == "END\tECS"
            {
                content.replace_range(i.., "END\tECS\t\r\n");
            }
        }
        while content.contains("\n\n\n") {
            content = content.replace("\n\n\n", "\n\n");
        }
        if !content.ends_with('\n') {
            content.push('\n');
        }
        content
    }
    pub fn save(&self, path: impl AsRef<Path>) -> Result<(), EditError> {
        let path = path.as_ref();
        backup(path)?;
        fs::write(path, self.serialize().as_bytes())?;
        Ok(())
    }
    pub fn save_as(&self, path: impl AsRef<Path>) -> Result<(), EditError> {
        self.save(path)
    }
}
fn backup(path: &Path) -> Result<(), EditError> {
    if path.exists() {
        let bak = std::path::PathBuf::from(format!("{}.BAK", path.display()));
        if bak.exists() {
            fs::remove_file(&bak)?;
        }
        fs::copy(path, bak)?;
    }
    Ok(())
}

pub fn split_csv(line: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut cur = String::new();
    let mut quoted = false;
    for c in line.chars() {
        if c == '"' {
            quoted = !quoted;
        } else if c == ',' && !quoted {
            out.push(cur);
            cur = String::new();
        } else {
            cur.push(c);
        }
    }
    out.push(cur);
    out
}
pub fn compare_versions(a: &str, b: &str) -> i32 {
    fn strip(v: &str) -> &str {
        v.trim().trim_start_matches(['v', 'V'])
    }
    let aa: Vec<_> = strip(a).split(['.', '~']).collect();
    let bb: Vec<_> = strip(b).split(['.', '~']).collect();
    for i in 0..aa.len().max(bb.len()) {
        let x = aa.get(i).copied().unwrap_or("0");
        let y = bb.get(i).copied().unwrap_or("0");
        let ord = match (x.parse::<i64>(), y.parse::<i64>()) {
            (Ok(x), Ok(y)) => x.cmp(&y),
            _ => x.cmp(y),
        };
        if ord != std::cmp::Ordering::Equal {
            return if ord == std::cmp::Ordering::Less {
                -1
            } else {
                1
            };
        }
    }
    0
}
pub fn parse_sha256sums(s: &[u8], asset: &str) -> Option<String> {
    for line in String::from_utf8_lossy(s).lines() {
        let line = line.trim();
        let Some((hash, name)) = line.split_once(' ') else {
            continue;
        };
        let name = name.trim().trim_start_matches('*');
        if hash.len() == 64
            && hash.chars().all(|c| c.is_ascii_hexdigit())
            && name.eq_ignore_ascii_case(asset)
        {
            return Some(hash.to_ascii_lowercase());
        }
    }
    None
}
pub fn sha256_hex(bytes: &[u8]) -> String {
    use sha2::{Digest, Sha256};
    format!("{:x}", Sha256::digest(bytes))
}

#[derive(Debug, Default)]
pub struct ProbeResult {
    pub connected: bool,
    pub host: String,
    pub port: u16,
    pub timeout_ms: u64,
    pub raw: Vec<u8>,
    pub observations: Vec<String>,
    pub summary: String,
}
pub fn probe_scale(host: &str, port: u16, timeout_ms: u64) -> ProbeResult {
    let mut r = ProbeResult {
        host: host.into(),
        port,
        timeout_ms,
        ..Default::default()
    };
    let addr = (host, port)
        .to_socket_addrs()
        .ok()
        .and_then(|mut i| i.next());
    let Some(addr) = addr else {
        r.summary = format!("connect to {host}:{port} failed: could not resolve host");
        r.observations.push(r.summary.clone());
        return r;
    };
    let timeout = Duration::from_millis(timeout_ms);
    let start = Instant::now();
    let mut s = match TcpStream::connect_timeout(&addr, timeout) {
        Ok(s) => s,
        Err(e) => {
            r.summary = if e.kind() == io::ErrorKind::TimedOut {
                format!("connect to {host}:{port} timed out after {timeout_ms} ms")
            } else {
                format!("connect to {host}:{port} failed: {e}")
            };
            r.observations.push(r.summary.clone());
            return r;
        }
    };
    r.connected = true;
    r.observations.push(format!("connected to {host}:{port}"));
    if let Err(e) = s.write_all(b"SCAL\x01\0\0\0") {
        r.observations.push(format!("probe send failed: {e}"));
    } else {
        r.observations.push("sent 8-byte probe payload".into());
    }
    let _ = s.set_read_timeout(Some(Duration::from_millis(150)));
    let mut buf = [0u8; 4096];
    let mut idle = false;
    loop {
        if start.elapsed() > timeout {
            break;
        }
        match s.read(&mut buf) {
            Ok(0) => {
                idle = true;
                break;
            }
            Ok(n) => {
                r.raw.extend_from_slice(&buf[..n]);
            }
            Err(e)
                if matches!(
                    e.kind(),
                    io::ErrorKind::WouldBlock
                        | io::ErrorKind::TimedOut
                        | io::ErrorKind::Interrupted
                ) =>
            {
                if !r.raw.is_empty() {
                    idle = true;
                    break;
                }
                if start.elapsed() >= timeout {
                    break;
                }
            }
            Err(e) => {
                r.observations.push(format!("read failed: {e}"));
                break;
            }
        }
    }
    if r.raw.is_empty() {
        r.summary = if idle {
            format!("connected to {host}:{port} but the scale closed the connection without sending any data")
        } else {
            format!("connected to {host}:{port} but the scale sent no data within the {timeout_ms} ms timeout")
        };
        r.observations.push(r.summary.clone());
    } else {
        r.observations
            .push(format!("received {} byte(s)", r.raw.len()));
        let text = String::from_utf8_lossy(&r.raw);
        let mut notes = vec![
            format!(
                "scale agent at {host}:{port} responded with {} byte(s)",
                r.raw.len()
            ),
            format!(
                "raw payload (hex): {}",
                r.raw.iter().map(|b| format!("{b:02x}")).collect::<String>()
            ),
        ];
        if text.chars().any(|c| !c.is_control() || c == '\n') {
            notes.push(format!("decoded as UTF-8 ({}):", text.len()));
            notes.extend(text.lines().map(|l| format!("  | {}", l.trim())));
        }
        if text.to_ascii_uppercase().contains("ERROR") || text.to_ascii_uppercase().contains("ERR")
        {
            notes.push("scale returned an error-style payload — likely an unsupported command or missing authentication.".into());
        } else if text.to_ascii_uppercase().contains("TMS")
            || text.to_ascii_uppercase().contains("DLX")
        {
            notes.push("payload mentions TMS/DLX — likely relevant to the transfer protocol we are trying to discover.".into());
        }
        r.summary = notes.join("\n");
        r.observations = notes;
    }
    r
}

#[cfg(test)]
mod tests {
    use super::*;
    fn fixture_a() -> Vec<u8> {
        b"HEADER\tDLX50\r\nDWL\tPLU\t\r\nPLU\t1\t1001\t0\t0\t799,2\tBeef Tendon\t0\t0\t\r\nEND\tPLU\t\r\n\r\nDWL\tDPT\t\r\nDPT\t1\tFresh Fruit\t\r\nEND\tDPT\t\r\n\r\nDWL\tBAR\t\r\nBAR\t1\t\t0\t0\t0\t0\tB201E500L500A000A000A001\t\r\nEND\tBAR\t\r\n\r\nEND\tECS\t\r\n".to_vec()
    }
    #[test]
    fn byte_roundtrip() {
        let b = fixture_a();
        assert_eq!(Editor::from_bytes(&b).unwrap().serialize().as_bytes(), b);
    }
    #[test]
    fn plu_edit_and_delete_roundtrip() {
        let b = fixture_a();
        let mut e = Editor::from_bytes(&b).unwrap();
        e.add("PLU", "9", "", "Codeless Item", "").unwrap();
        assert_eq!(e.list("PLU").unwrap()[2][1], "1002");
        e.delete_code("PLU", "1002").unwrap();
        assert_eq!(e.serialize().as_bytes(), b);
    }
    #[test]
    fn layout_b_barcode_and_plu() {
        let b=b"DWL\tPLU\t\r\nPLU\t1\tWidget Alpha\t1\t0\t0\t0\t0\t0\t\r\nEND\tPLU\t\r\nDWL\tBAR\t\r\nBAR\t1\t0\t0\t0\t0\tB000L000A000A000A000\t\r\nEND\tBAR\t\r\nEND\tECS\t\r\n";
        let e = Editor::from_bytes(b).unwrap();
        assert_eq!(e.list("PLU").unwrap()[1][2], "Widget Alpha");
        assert_eq!(e.list("BAR").unwrap()[1][1], "B000L000A000A000A000");
        assert_eq!(e.list("DPT").unwrap().len(), 1);
        assert_eq!(e.serialize().as_bytes(), b);
    }
    #[test]
    fn bootstrap_empty_bar_without_panic() {
        let b = b"DWL\tBAR\t\r\nEND\tBAR\t\r\nEND\tECS\t\r\n";
        let mut e = Editor::from_bytes(b).unwrap();
        e.add("BAR", "2", "", "B001L001A000A000A000", "").unwrap();
        assert_eq!(e.list("BAR").unwrap()[1][1], "B001L001A000A000A000");
    }
    #[test]
    fn versions_and_checksum() {
        assert_eq!(compare_versions("v1.2.0", "1.1.9"), 1);
        assert_eq!(
            parse_sha256sums(
                format!("{}  app.zip\n", "a".repeat(64)).as_bytes(),
                "app.zip"
            )
            .unwrap(),
            "a".repeat(64)
        );
    }
}
