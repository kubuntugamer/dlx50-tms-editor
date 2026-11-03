use std::fs;
use std::io::{self, Read, Write};
use std::net::{TcpStream, ToSocketAddrs, UdpSocket};
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

// ---------------------------------------------------------------------------
// DL-series scale transport, read-only (issues #7 / #8).
//
// Ported from the Qt implementation parked on `qt-network-transport`; the
// protocol was recovered black-box from the scale's own beacons and from the
// .NET user-string heap of the OEM's management program. Everything on the
// wire is ASCII, TAB-separated and CRLF-terminated:
//
//   request : "UPL\t<SECTION>\t\r\n"      PC -> scale (read-only)
//   response: "\r\nDWL\t<SECTION>\t\r\n"
//             "<SECTION>\t<fields>\t\r\n"    zero or more data rows
//             "END\t<SECTION>\t\r\n"
//
// The scale keeps the connection open after answering, so the reader stops at
// the END marker instead of waiting for the peer to close, and an unknown or
// empty section is answered with an empty DWL/END pair — exactly what an empty
// section looks like in a real export.
//
// The write direction reuses that same block shape: the PC sends
// "\r\nDWL\t<SECTION>\t\r\n" + rows + "END\t<SECTION>\t\r\n" and the scale
// applies it without answering. Verified against a real DL-series (a full write
// followed by a retrieval is byte-identical apart from the clock row) but never
// captured from the OEM program, so `upload` re-reads every section instead of
// trusting the send. The device clock section (TIM) is never written.
// ---------------------------------------------------------------------------

/// Spec166 default TCP port for the scale's server.
pub const SCALE_DEFAULT_PORT: u16 = 33581;
/// Destination of the scale's ~10 s beacon ("PTR\t\r\n").
pub const SCALE_BEACON_PORT: u16 = 33594;
/// Destination of the ~60 s beacon ("UDP\t<port>\t<station>\t\r\n"), which
/// also carries the scale's TCP port.
pub const SCALE_INFO_PORT: u16 = 33584;
/// Discovery must outlast the 10 s beacon gap.
pub const SCALE_DISCOVER_MS: u64 = 12_000;
/// Per-section deadline; PLU takes ~6 s end to end, so this leaves headroom.
pub const SCALE_SECTION_MS: u64 = 30_000;
/// Poll slice while waiting on two sockets during discovery.
pub const SCALE_POLL_MS: u64 = 250;

/// The file's own header. The scale exposes no section for it and Save only
/// rewrites an existing "END\tECS" line rather than adding one, so a
/// retrieved file has to carry the header and footer itself. 254 is what this
/// device family's exports start with; the OEM's generic template ships 100.
pub const ECS_HEADER: &str = "ECS\tVER\t254\t\r\n";
/// The file's own footer.
pub const ECS_FOOTER: &str = "\r\nEND\tECS\t\r\n";

/// Section order copied from a real device export — the order the network
/// path requests them in. Deliberately distinct from `REBUILT_TAGS`, which is
/// the editor's own tab order.
pub const NETWORK_SECTIONS: &[&str] = &[
    "DPT", "CLS", "PLU", "UNT", "BAR", "LAB", "SAL", "REP", "SCF", "SCP", "TMS", "TMT", "TMR",
    "INF", "TIM", "BMP",
];

/// The exact bytes a section occupies in the file: the `DWL` header, the rows,
/// and the `END` closer. Used both to build a write and to predict what a
/// read-back should return.
pub fn section_block(tag: &str, rows: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(rows.len() + tag.len() * 2 + 16);
    out.extend_from_slice(format!("\r\nDWL\t{tag}\t\r\n").as_bytes());
    out.extend_from_slice(rows);
    out.extend_from_slice(format!("END\t{tag}\t\r\n").as_bytes());
    out
}

/// The row bytes of one section inside a whole `.TMS`, ready to be written
/// back. A missing section yields no rows — which is what an empty section is.
pub fn section_rows(file: &[u8], tag: &str) -> Vec<u8> {
    let start = format!("\r\nDWL\t{tag}\t\r\n");
    let end = format!("END\t{tag}\t\r\n");
    let Some(at) = file
        .windows(start.len())
        .position(|w| w == start.as_bytes())
    else {
        return Vec::new();
    };
    let from = at + start.len();
    let Some(len) = file[from..]
        .windows(end.len())
        .position(|w| w == end.as_bytes())
    else {
        return Vec::new();
    };
    file[from..from + len].to_vec()
}

/// Sections whose value belongs to the device, not to a saved file. `TIM` is
/// the scale's running clock (yy mm dd hh mm ss): writing a file's stale copy
/// back would reset it, so uploads skip it and the read-back ignores it.
pub const VOLATILE_SECTIONS: &[&str] = &["TIM"];

/// One read-only section request: "UPL\t<TAG>\t\r\n".
pub fn upl_request(tag: &str) -> Vec<u8> {
    let mut out = Vec::with_capacity(tag.len() + 6);
    out.extend_from_slice(b"UPL\t");
    out.extend_from_slice(tag.as_bytes());
    out.extend_from_slice(b"\t\r\n");
    out
}

/// The wire-perfect block for a section with no rows — also the answer the
/// scale gives for an unknown section.
pub fn empty_section_block(tag: &str) -> Vec<u8> {
    let mut out = Vec::with_capacity(tag.len() * 2 + 16);
    out.extend_from_slice(b"\r\nDWL\t");
    out.extend_from_slice(tag.as_bytes());
    out.extend_from_slice(b"\t\r\nEND\t");
    out.extend_from_slice(tag.as_bytes());
    out.extend_from_slice(b"\t\r\n");
    out
}

/// "UDP\t<port>\t<station>\t\r\n" -> the scale's TCP port. Anything else
/// (the 10 s "PTR\t\r\n" beacon, for instance) returns None and the caller
/// keeps the Spec166 default.
pub fn parse_beacon(datagram: &[u8]) -> Option<u16> {
    if !datagram.starts_with(b"UDP\t") {
        return None;
    }
    let text = std::str::from_utf8(datagram).ok()?;
    let mut fields = text.split('\t');
    if fields.next()? != "UDP" {
        return None;
    }
    let port: u16 = fields.next()?.trim().parse().ok()?;
    (port != 0).then_some(port)
}

/// Where a scale was heard: its address and (from the 60 s beacon, or the
/// Spec166 default) its TCP port.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Endpoint {
    pub host: String,
    pub port: u16,
    pub heard_beacon: bool,
}

/// Wait passively for a scale beacon — the way the vendor's utility does it.
/// Any beacon names the scale; only the 60 s one replaces the default port.
pub fn discover(timeout_ms: u64) -> Result<Endpoint, String> {
    let beacon = UdpSocket::bind(("0.0.0.0", SCALE_BEACON_PORT));
    let info = UdpSocket::bind(("0.0.0.0", SCALE_INFO_PORT));
    if beacon.is_err() && info.is_err() {
        return Err(format!(
            "cannot listen on UDP {SCALE_BEACON_PORT}/{} for the scale's beacon — both ports are already in use",
            SCALE_INFO_PORT
        ));
    }

    let deadline = Instant::now() + Duration::from_millis(timeout_ms);
    let mut buf = [0u8; 2048];
    'wait: loop {
        for sock in [&beacon, &info].into_iter().flatten() {
            let now = Instant::now();
            if now >= deadline {
                break 'wait;
            }
            let left = (deadline - now).as_millis() as u64;
            let slice = left.min(SCALE_POLL_MS).max(1);
            if sock.set_read_timeout(Some(Duration::from_millis(slice))).is_err() {
                continue;
            }
            match sock.recv_from(&mut buf) {
                Ok((n, from)) => {
                    let ip = from.ip();
                    if ip.is_unspecified() {
                        continue;
                    }
                    return Ok(Endpoint {
                        host: ip.to_string(),
                        port: parse_beacon(&buf[..n]).unwrap_or(SCALE_DEFAULT_PORT),
                        heard_beacon: true,
                    });
                }
                Err(e)
                    if matches!(
                        e.kind(),
                        io::ErrorKind::WouldBlock
                            | io::ErrorKind::TimedOut
                            | io::ErrorKind::Interrupted
                    ) =>
                {
                    continue;
                }
                Err(_) => continue,
            }
        }
    }
    Err(format!(
        "no scale beacon heard on UDP {SCALE_BEACON_PORT}/{} within {timeout_ms} ms — is the scale powered and on this network?",
        SCALE_INFO_PORT
    ))
}

/// One read-only UPL exchange. Fails unless the response both starts with
/// "\r\nDWL\t<TAG>\t\r\n" and ends with "END\t<TAG>\t\r\n", so a truncated
/// or garbled answer is never mistaken for a good one. There is deliberately
/// no idle timeout inside a section: the scale pauses between row batches.
pub fn fetch_section(ep: &Endpoint, tag: &str) -> Result<Vec<u8>, String> {
    fetch_section_with_deadline(ep, tag, SCALE_SECTION_MS)
}

/// The same exchange with an injectable per-section deadline, so a scale that
/// accepts and then goes silent can be tested without waiting 30 s.
pub fn fetch_section_with_deadline(
    ep: &Endpoint,
    tag: &str,
    deadline_ms: u64,
) -> Result<Vec<u8>, String> {
    let start = format!("\r\nDWL\t{tag}\t\r\n");
    let end = format!("END\t{tag}\t\r\n");

    let addr = (ep.host.as_str(), ep.port)
        .to_socket_addrs()
        .map_err(|e| format!("could not resolve scale at {}:{}: {e}", ep.host, ep.port))?
        .next()
        .ok_or_else(|| format!("could not resolve scale at {}:{}", ep.host, ep.port))?;

    let mut sock =
        TcpStream::connect_timeout(&addr, Duration::from_millis(deadline_ms)).map_err(|e| {
            format!("could not connect to scale at {}:{}: {e}", ep.host, ep.port)
        })?;

    let started = Instant::now();
    let left = || -> Option<Duration> {
        let ms = deadline_ms.saturating_sub(started.elapsed().as_millis() as u64);
        (ms > 0).then_some(Duration::from_millis(ms))
    };

    let request = upl_request(tag);
    if let Some(d) = left() {
        let _ = sock.set_write_timeout(Some(d));
    }
    sock.write_all(&request)
        .map_err(|e| format!("could not send UPL {tag} to the scale: {e}"))?;

    let mut buf: Vec<u8> = Vec::new();
    let mut chunk = [0u8; 8192];
    loop {
        let Some(d) = left() else {
            return Err(format!(
                "section {tag} timed out after {deadline_ms} ms ({} bytes)",
                buf.len()
            ));
        };
        if sock.set_read_timeout(Some(d)).is_err() {
            break;
        }
        match sock.read(&mut chunk) {
            Ok(0) => break, // peer went away; the END check below decides
            Ok(n) => {
                buf.extend_from_slice(&chunk[..n]);
                if buf.ends_with(end.as_bytes()) {
                    break;
                }
            }
            Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
            // The socket's read timeout is always set to whatever is left of
            // the section deadline, so this means the deadline expired before
            // END arrived — report that rather than a missing END marker.
            Err(e)
                if matches!(
                    e.kind(),
                    io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut
                ) =>
            {
                return Err(format!(
                    "section {tag} timed out after {deadline_ms} ms ({} bytes)",
                    buf.len()
                ));
            }
            Err(e) => return Err(format!("read from scale failed for section {tag}: {e}")),
        }
    }

    if !buf.starts_with(start.as_bytes()) {
        return Err(if buf.is_empty() {
            format!("the scale sent no data for section {tag}")
        } else {
            format!(
                "section {tag} did not start with DWL: {}",
                String::from_utf8_lossy(&buf[..buf.len().min(60)])
            )
        });
    }
    if !buf.ends_with(end.as_bytes()) {
        return Err(format!(
            "section {tag} was truncated ({} bytes, no END marker)",
            buf.len()
        ));
    }
    Ok(buf)
}

/// Send one section to the scale and return whatever it says back.
///
/// The write framing is **not vendor-confirmed**: it mirrors the device's own
/// `DWL…END` reply, which is the only shape ever observed on the wire. Treat
/// the reply as diagnostics only — the caller must read the section back to
/// know whether the scale stored anything.
pub fn upload_section(ep: &Endpoint, tag: &str, rows: &[u8]) -> Result<Vec<u8>, String> {
    upload_section_with_deadline(ep, tag, rows, SCALE_SECTION_MS)
}

/// The same write with an injectable reply window.
pub fn upload_section_with_deadline(
    ep: &Endpoint,
    tag: &str,
    rows: &[u8],
    deadline_ms: u64,
) -> Result<Vec<u8>, String> {
    let addr = (ep.host.as_str(), ep.port)
        .to_socket_addrs()
        .map_err(|e| format!("could not resolve scale at {}:{}: {e}", ep.host, ep.port))?
        .next()
        .ok_or_else(|| format!("could not resolve scale at {}:{}", ep.host, ep.port))?;

    let mut sock =
        TcpStream::connect_timeout(&addr, Duration::from_millis(deadline_ms)).map_err(|e| {
            format!("could not connect to scale at {}:{}: {e}", ep.host, ep.port)
        })?;
    sock.write_all(&section_block(tag, rows))
        .map_err(|e| format!("could not send {tag} to the scale: {e}"))?;
    let _ = sock.flush();

    // Collect an acknowledgement if one comes, but never wait long: an
    // unconfirmed write must look like a quick no-reply, not like a hang.
    let started = Instant::now();
    let mut reply = Vec::new();
    let mut chunk = [0u8; 4096];
    loop {
        let left = deadline_ms.saturating_sub(started.elapsed().as_millis() as u64);
        if left == 0 {
            break;
        }
        if sock
            .set_read_timeout(Some(Duration::from_millis(left.min(1_000))))
            .is_err()
        {
            break;
        }
        match sock.read(&mut chunk) {
            Ok(0) => break,
            Ok(n) => reply.extend_from_slice(&chunk[..n]),
            Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
            Err(e) if matches!(e.kind(), io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut) => {
                break
            }
            Err(e) => return Err(format!("read from scale failed while writing {tag}: {e}")),
        }
    }
    Ok(reply)
}

/// Why a transport operation could not complete.
#[derive(Debug, PartialEq, Eq)]
pub enum TransportError {
    /// The scale or backend does not implement this operation at all.
    NotSupported(String),
    /// The scale could not be reached.
    Unreachable(String),
    /// The scale answered with something the protocol does not allow.
    Protocol(String),
    /// Local file I/O failed.
    Io(String),
}
impl std::fmt::Display for TransportError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::NotSupported(s) | Self::Unreachable(s) | Self::Protocol(s) | Self::Io(s) => {
                f.write_str(s)
            }
        }
    }
}
pub type TransportResult<T> = Result<T, TransportError>;

/// One logical file a transport offers. The network protocol has no file
/// listing — it streams the scale's live configuration as sections, which
/// assemble into a single `.TMS`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TransportFile {
    pub remote_path: String,
    pub local_name: String,
    /// Not known until the sections are fetched.
    pub size: u64,
}

/// Read-only network transport for a DL-series scale: discover by beacon,
/// fetch every section in device order, assemble a device-exact `.TMS`.
#[derive(Default)]
pub struct ScaleTransport {
    endpoint: Option<Endpoint>,
}

impl ScaleTransport {
    pub fn new() -> Self {
        Self::default()
    }

    /// Cached so list and retrieve do not each pay for the next beacon wait.
    fn ensure_endpoint(&mut self) -> TransportResult<Endpoint> {
        if let Some(ep) = &self.endpoint {
            return Ok(ep.clone());
        }
        let ep = discover(SCALE_DISCOVER_MS).map_err(TransportError::Unreachable)?;
        self.endpoint = Some(ep.clone());
        Ok(ep)
    }

    /// Where discovery settled, if it has run yet — the CLI prints this so a
    /// failed fetch is diagnosable.
    pub fn endpoint(&self) -> Option<&Endpoint> {
        self.endpoint.as_ref()
    }

    /// Offer the scale's live configuration as the single logical file.
    pub fn list_files(&mut self) -> TransportResult<Vec<TransportFile>> {
        self.ensure_endpoint()?;
        Ok(vec![TransportFile {
            remote_path: "LIVE".into(),
            local_name: "SCALE_LIVE.TMS".into(),
            size: 0,
        }])
    }

    /// Fetch every section in device order and assemble the file in memory,
    /// so a failure part-way through never leaves a half-written file behind.
    pub fn retrieve(&mut self, remote_path: &str, local_path: &Path) -> TransportResult<()> {
        if !remote_path.is_empty() && remote_path != "LIVE" {
            return Err(TransportError::NotSupported(format!(
                "the network transport only offers the scale's live configuration (LIVE), not '{remote_path}'"
            )));
        }
        let ep = self.ensure_endpoint()?;

        let mut content = ECS_HEADER.as_bytes().to_vec();
        let mut first = true;
        for &section in NETWORK_SECTIONS {
            match fetch_section(&ep, section) {
                Ok(block) => content.extend_from_slice(&block),
                Err(e) => {
                    // A failure on the very first section usually means the
                    // cached address went stale (scale rebooted / DHCP moved
                    // it), so drop it and let the next attempt rediscover.
                    if first {
                        self.endpoint = None;
                    }
                    return Err(TransportError::Protocol(e));
                }
            }
            first = false;
        }
        content.extend_from_slice(ECS_FOOTER.as_bytes());

        fs::write(local_path, &content).map_err(|e| {
            TransportError::Io(format!("cannot write {}: {e}", local_path.display()))
        })?;
        Ok(())
    }

    /// Push a local `.TMS` back to the scale, one section at a time in device
    /// order, then read every section back and compare it with what was sent.
    ///
    /// The write framing mirrors the device's own `DWL…END` replies; it was
    /// never captured from a vendor write session, so the read-back is the only
    /// proof the scale took the data. A mismatch is reported, never retried —
    /// a half-applied write is the worst moment to guess again.
    pub fn upload(&mut self, local_path: &Path, remote_path: &str) -> TransportResult<()> {
        if !remote_path.is_empty() && remote_path != "LIVE" {
            return Err(TransportError::NotSupported(format!(
                "the network transport only offers the scale's live configuration (LIVE), not '{remote_path}'"
            )));
        }
        let bytes = fs::read(local_path).map_err(|e| {
            TransportError::Io(format!("cannot read {}: {e}", local_path.display()))
        })?;
        let ep = self.ensure_endpoint()?;

        for &section in NETWORK_SECTIONS {
            if VOLATILE_SECTIONS.contains(&section) {
                continue;
            }
            let rows = section_rows(&bytes, section);
            upload_section(&ep, section, &rows).map_err(TransportError::Protocol)?;
        }

        let mut mismatched = Vec::new();
        for &section in NETWORK_SECTIONS {
            if VOLATILE_SECTIONS.contains(&section) {
                continue;
            }
            let expected = section_block(section, &section_rows(&bytes, section));
            let got = fetch_section(&ep, section).map_err(TransportError::Protocol)?;
            if got != expected {
                mismatched.push(section);
            }
        }
        if !mismatched.is_empty() {
            return Err(TransportError::Protocol(format!(
                "the scale did not store what was sent for {} — the write framing has never been confirmed against a vendor session, so stop and compare by hand before sending anything else",
                mismatched.join(", ")
            )));
        }
        Ok(())
    }

    /// Read every section and report how the local file differs from the scale,
    /// **without writing anything**. This is the read-only half of `upload`, so a
    /// push can be previewed with the exact comparison the write would verify
    /// against afterwards.
    pub fn preview(
        &mut self,
        local_path: &Path,
        remote_path: &str,
    ) -> TransportResult<Vec<(&'static str, SectionPreview)>> {
        if !remote_path.is_empty() && remote_path != "LIVE" {
            return Err(TransportError::NotSupported(format!(
                "the network transport only offers the scale's live configuration (LIVE), not '{remote_path}'"
            )));
        }
        let bytes = fs::read(local_path).map_err(|e| {
            TransportError::Io(format!("cannot read {}: {e}", local_path.display()))
        })?;
        let ep = self.ensure_endpoint()?;

        let mut out = Vec::new();
        for &section in NETWORK_SECTIONS {
            if VOLATILE_SECTIONS.contains(&section) {
                continue;
            }
            let local_rows = section_rows(&bytes, section);
            let got = fetch_section(&ep, section).map_err(TransportError::Protocol)?;
            let remote_rows = section_rows(&got, section);
            let state = if local_rows == remote_rows {
                SectionPreview::Unchanged
            } else if local_rows.is_empty() {
                SectionPreview::WouldClear {
                    remote_rows: count_row_lines(&remote_rows),
                }
            } else {
                SectionPreview::WouldReplace {
                    local_rows: count_row_lines(&local_rows),
                    remote_rows: count_row_lines(&remote_rows),
                }
            };
            out.push((section, state));
        }
        Ok(out)
    }
}

/// How one section of a local file compares with what the scale holds now.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SectionPreview {
    /// The scale already matches; a push would not change this section.
    Unchanged,
    /// The file has rows here and the scale holds something else.
    WouldReplace { local_rows: usize, remote_rows: usize },
    /// The file has no rows for this section, so a push would empty it.
    WouldClear { remote_rows: usize },
}

/// Rows in a section's raw bytes, for reporting.
fn count_row_lines(rows: &[u8]) -> usize {
    rows.split(|b| *b == b'\n')
        .filter(|line| !line.is_empty())
        .count()
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
    // Issue #14 / 2b: the old 8-byte "SCAL" handshake was a placeholder —
    // the real protocol is known now, and a read-only UPL request is both the
    // truthful probe and a safe one: it only ever asks the scale to send.
    let request = upl_request("INF");
    if let Err(e) = s.write_all(&request) {
        r.observations.push(format!("probe send failed: {e}"));
    } else {
        r.observations.push(format!(
            "sent read-only request: {}",
            String::from_utf8_lossy(&request).trim_end()
        ));
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
        if r.raw.starts_with(b"\r\nDWL\t") && r.raw.windows(4).any(|w| w == b"END\t") {
            notes.push("the answer is a framed DWL…END section response — that is the read-only UPL transfer protocol (#7/#8), not an error.".into());
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
        // Keep the earlier observations (connected / sent request /
        // received count) so the full transcript is inspectable.
        let mut transcript = std::mem::take(&mut r.observations);
        transcript.extend(notes);
        r.observations = transcript;
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
    #[test]
    fn upl_request_wraps_a_three_letter_tag_verbatim() {
        assert_eq!(upl_request("INF"), b"UPL\tINF\t\r\n".to_vec());
        assert_eq!(upl_request("BMP"), b"UPL\tBMP\t\r\n".to_vec());
    }
    #[test]
    fn empty_section_block_is_wire_perfect() {
        assert_eq!(
            empty_section_block("INF"),
            b"\r\nDWL\tINF\t\r\nEND\tINF\t\r\n".to_vec()
        );
    }
    #[test]
    fn beacon_udp_carries_the_tcp_port() {
        assert_eq!(parse_beacon(b"UDP\t33581\tS1\t\r\n"), Some(33581));
    }
    #[test]
    fn beacon_ptr_is_not_a_port_beacon() {
        assert_eq!(parse_beacon(b"PTR\t\r\n"), None);
    }
    #[test]
    fn beacon_udp_with_port_zero_is_not_a_port_beacon() {
        assert_eq!(parse_beacon(b"UDP\t0\t\r\n"), None);
    }
    #[test]
    fn beacon_garbage_is_ignored() {
        assert_eq!(parse_beacon(b"garbage"), None);
    }
    #[test]
    fn network_sections_shape_is_valid() {
        for (i, tag) in NETWORK_SECTIONS.iter().enumerate() {
            assert_eq!(tag.len(), 3, "{tag} is not three characters long");
            assert!(
                tag.bytes().all(|b| b.is_ascii_uppercase()),
                "{tag} is not three uppercase ASCII letters"
            );
            assert!(
                !NETWORK_SECTIONS[..i].contains(tag),
                "{tag} appears twice in NETWORK_SECTIONS"
            );
        }
        // Device order and the editor's own tab order are deliberately
        // separate lists; merging them would reorder a retrieved file.
        assert_ne!(NETWORK_SECTIONS, REBUILT_TAGS);
    }
}
