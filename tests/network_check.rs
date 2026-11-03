//! Port of the Qt-side fake-scale check (`tests/network_check.cpp` on the
//! `qt-network-transport` branch): a local fake scale that speaks the beacon
//! and the read-only UPL exchange, driven through the real backend code.

use std::collections::HashMap;
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream, UdpSocket};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

use tms_editor::{
    discover, empty_section_block, fetch_section, fetch_section_with_deadline, probe_scale,
    section_block, Endpoint, ScaleTransport, SectionPreview, ECS_FOOTER, ECS_HEADER,
    NETWORK_SECTIONS, SCALE_BEACON_PORT, SCALE_INFO_PORT,
};

#[derive(Clone, Copy)]
enum Mode {
    /// Answer every known section with a framed row block, unknown sections
    /// with the wire-perfect empty block — what a real scale does.
    Honest,
    /// Answer with something that is not DWL-framed at all.
    Garbled,
    /// Start the DWL header, then hang up before END.
    Truncated,
}

/// What the fake scale has been told to store, per section tag.
type ScaleState = Arc<Mutex<HashMap<String, Vec<u8>>>>;

/// A request is complete at its trailing CRLF — except a write, whose rows end
/// in CRLF too, so it is only complete at the END closer.
fn is_complete(req: &[u8]) -> bool {
    if !req.ends_with(b"\r\n") {
        return false;
    }
    if !req.starts_with(b"\r\nDWL\t") {
        return true;
    }
    let text = String::from_utf8_lossy(req);
    let tag = text.split('\t').nth(1).unwrap_or("");
    req.ends_with(format!("END\t{tag}\t\r\n").as_bytes())
}

fn serve(mut stream: TcpStream, mode: Mode, state: ScaleState) {
    let mut req = Vec::new();
    let mut chunk = [0u8; 4096];
    loop {
        match stream.read(&mut chunk) {
            Ok(0) => return,
            Ok(n) => {
                req.extend_from_slice(&chunk[..n]);
                if is_complete(&req) {
                    break;
                }
            }
            Err(_) => return,
        }
    }
    let text = String::from_utf8_lossy(&req);
    let tag = text.split('\t').nth(1).unwrap_or("").to_owned();

    // A write looks exactly like the framing the device itself emits: a DWL
    // header, the rows, then the END closer.
    if req.starts_with(b"\r\nDWL\t") {
        let rows = tms_editor::section_rows(&req, &tag);
        state.lock().unwrap_or_else(|e| e.into_inner()).insert(tag, rows);
        return; // no acknowledgement is modelled — the read-back is the proof
    }

    let response = match mode {
        Mode::Garbled => b"GARBAGE\r\n".to_vec(),
        Mode::Truncated => format!("\r\nDWL\t{tag}\t\r\npartial row").into_bytes(),
        Mode::Honest => {
            let stored = state
                .lock()
                .unwrap_or_else(|e| e.into_inner())
                .get(&tag)
                .cloned();
            match stored {
                Some(rows) => section_block(&tag, &rows),
                None if NETWORK_SECTIONS.contains(&tag.as_str()) => {
                    format!("\r\nDWL\t{tag}\t\r\n{tag}\t1\tOK\t\r\nEND\t{tag}\t\r\n").into_bytes()
                }
                None => empty_section_block(&tag),
            }
        }
    };
    let _ = stream.write_all(&response);
}

/// Start a fake scale on an ephemeral TCP port; each connection is served
/// sequentially, which matches the one-request-at-a-time real exchange.
fn spawn_fake_scale(mode: Mode) -> u16 {
    let listener = TcpListener::bind("127.0.0.1:0").expect("bind fake scale");
    let port = listener.local_addr().expect("local_addr").port();
    let state: ScaleState = Arc::new(Mutex::new(HashMap::new()));
    thread::spawn(move || {
        for stream in listener.incoming() {
            match stream {
                Ok(s) => serve(s, mode, Arc::clone(&state)),
                Err(_) => break,
            }
        }
    });
    port
}

/// Beam the 60 s beacon (at test speed) at the discovery port until stopped.
fn start_beacon(tcp_port: u16) -> Arc<AtomicBool> {
    let stop = Arc::new(AtomicBool::new(false));
    let flag = Arc::clone(&stop);
    thread::spawn(move || {
        let sock = UdpSocket::bind("127.0.0.1:0").expect("bind beacon socket");
        let msg = format!("UDP\t{tcp_port}\t1\t\r\n");
        while !flag.load(Ordering::SeqCst) {
            let _ = sock.send_to(msg.as_bytes(), ("127.0.0.1", SCALE_BEACON_PORT));
            thread::sleep(Duration::from_millis(100));
        }
    });
    stop
}

/// Discovery binds two fixed UDP ports, so tests that use it — or that hold
/// those ports on purpose — must not run concurrently within this binary.
static DISCOVERY_PORTS: Mutex<()> = Mutex::new(());

fn lock_discovery_ports() -> std::sync::MutexGuard<'static, ()> {
    DISCOVERY_PORTS.lock().unwrap_or_else(|e| e.into_inner())
}

#[test]
fn fake_scale_end_to_end() {
    let _ports = lock_discovery_ports();
    let scale_port = spawn_fake_scale(Mode::Honest);
    let stop = start_beacon(scale_port);

    // Discovery hears the beacon and takes the port out of it.
    let ep = discover(4_000).expect("discover should hear the fake beacon");
    assert_eq!(ep.host, "127.0.0.1");
    assert_eq!(ep.port, scale_port);
    assert!(ep.heard_beacon);

    // A read-only section exchange is framed correctly.
    let block = fetch_section(&ep, "INF").expect("fetch INF");
    assert!(block.starts_with(b"\r\nDWL\tINF\t\r\n"), "{block:?}");
    assert!(block.ends_with(b"END\tINF\t\r\n"), "{block:?}");

    // An unknown section comes back as the wire-perfect empty block.
    let unknown = fetch_section(&ep, "ZZZ").expect("unknown section fetch");
    assert_eq!(unknown, empty_section_block("ZZZ"));

    // A truncated answer is rejected, never mistaken for a good one.
    let trunc = Endpoint {
        host: "127.0.0.1".into(),
        port: spawn_fake_scale(Mode::Truncated),
        heard_beacon: false,
    };
    let err = fetch_section(&trunc, "INF").expect_err("truncated must fail");
    assert!(err.contains("truncated"), "{err}");

    // A garbled answer is rejected too.
    let garb = Endpoint {
        host: "127.0.0.1".into(),
        port: spawn_fake_scale(Mode::Garbled),
        heard_beacon: false,
    };
    let err = fetch_section(&garb, "INF").expect_err("garbled must fail");
    assert!(err.contains("DWL"), "{err}");

    // Full retrieval assembles a device-exact file: header, every section in
    // device order, footer.
    let out = std::env::temp_dir().join(format!("dlx50_fake_scale_{}.TMS", std::process::id()));
    let mut transport = ScaleTransport::new();
    transport.retrieve("LIVE", &out).expect("retrieve");
    let content = std::fs::read(&out).expect("read assembled file");
    let text = String::from_utf8_lossy(&content);
    assert!(text.starts_with(ECS_HEADER), "missing ECS header");
    assert!(text.ends_with(ECS_FOOTER), "missing ECS footer");
    for &section in NETWORK_SECTIONS {
        assert!(
            text.contains(&format!("\r\nDWL\t{section}\t\r\n")),
            "section {section} missing from the assembled file"
        );
    }
    std::fs::remove_file(&out).ok();

    stop.store(true, Ordering::SeqCst);
}

#[test]
fn probe_sends_a_read_only_upl_request() {
    let port = spawn_fake_scale(Mode::Honest);
    let r = probe_scale("127.0.0.1", port, 3_000);
    assert!(r.connected, "{}", r.summary);
    // The probe only ever asks the scale to send.
    assert!(r.raw.starts_with(b"\r\nDWL\tINF\t\r\n"), "{:?}", r.raw);
    let joined = r.observations.join("\n");
    assert!(joined.contains("UPL\tINF"), "{joined}");
    assert!(joined.contains("framed DWL"), "{joined}");
}

#[test]
fn discover_fails_when_both_beacon_ports_are_taken() {
    let _ports = lock_discovery_ports();
    // Hold both discovery ports so the passive listener cannot bind either.
    let Ok(beacon) = UdpSocket::bind(("0.0.0.0", SCALE_BEACON_PORT)) else {
        panic!("ports unavailable for test: UDP {SCALE_BEACON_PORT} is already in use");
    };
    let Ok(info) = UdpSocket::bind(("0.0.0.0", SCALE_INFO_PORT)) else {
        panic!("ports unavailable for test: UDP {SCALE_INFO_PORT} is already in use");
    };

    let err = discover(100).expect_err("discover must fail when neither port can be bound");
    assert!(
        err.contains(&SCALE_BEACON_PORT.to_string()) && err.contains(&SCALE_INFO_PORT.to_string()),
        "the error should name both ports: {err}"
    );

    drop(beacon);
    drop(info);
}

#[test]
fn fetch_section_times_out_on_a_silent_scale() {
    // Accept the connection, then say nothing at all; the client must give up
    // on its own deadline instead of waiting for the peer to close.
    let listener = TcpListener::bind("127.0.0.1:0").expect("bind silent scale");
    let port = listener.local_addr().expect("local_addr").port();
    thread::spawn(move || {
        if let Ok((open, _)) = listener.accept() {
            thread::sleep(Duration::from_secs(10));
            drop(open);
        }
    });

    let ep = Endpoint {
        host: "127.0.0.1".into(),
        port,
        heard_beacon: false,
    };
    let started = std::time::Instant::now();
    let err = fetch_section_with_deadline(&ep, "INF", 200)
        .expect_err("a silent scale must time out");
    assert!(err.contains("timed out"), "{err}");
    assert!(
        started.elapsed() < Duration::from_secs(1),
        "the deadline was not honoured: {:?}",
        started.elapsed()
    );
}

#[test]
fn retrieve_writes_header_footer_and_every_network_section() {
    let _ports = lock_discovery_ports();
    let scale_port = spawn_fake_scale(Mode::Honest);
    let stop = start_beacon(scale_port);

    let out = std::env::temp_dir().join(format!(
        "dlx50_retrieve_shapes_{}.TMS",
        std::process::id()
    ));
    let mut transport = ScaleTransport::new();
    transport
        .retrieve("LIVE", &out)
        .expect("retrieve from the fake scale");
    stop.store(true, Ordering::SeqCst);

    let text = std::fs::read_to_string(&out).expect("read the assembled file");
    let _ = std::fs::remove_file(&out);

    assert!(
        text.starts_with(ECS_HEADER),
        "the file must start with the ECS header: {text:?}"
    );
    assert!(
        text.ends_with(ECS_FOOTER),
        "the file must end with the ECS footer: {text:?}"
    );
    for &section in NETWORK_SECTIONS {
        assert_eq!(
            text.matches(&format!("END\t{section}\t\r\n")).count(),
            1,
            "section {section} must have exactly one END closer"
        );
    }
    // One closer per section, plus the file's own "END\tECS\t" footer.
    assert_eq!(
        text.matches("\r\nEND\t").count(),
        NETWORK_SECTIONS.len() + 1,
        "unexpected number of END markers"
    );
}

#[test]
fn upload_writes_every_section_and_read_back_matches() {
    let _ports = lock_discovery_ports();
    let scale_port = spawn_fake_scale(Mode::Honest);
    let stop = start_beacon(scale_port);

    let plu_rows: &[u8] = b"PLU\t1\t1001\tWidget\t0\t0\t799,2\t0\t0\t\r\n";
    let dpt_rows: &[u8] = b"DPT\t1\tGrocery\t\r\n";
    let mut content = ECS_HEADER.as_bytes().to_vec();
    content.extend_from_slice(&section_block("PLU", plu_rows));
    content.extend_from_slice(&section_block("DPT", dpt_rows));
    content.extend_from_slice(ECS_FOOTER.as_bytes());

    let src = std::env::temp_dir().join(format!(
        "dlx50_upload_src_{}.TMS",
        std::process::id()
    ));
    std::fs::write(&src, &content).expect("write source file");

    let mut transport = ScaleTransport::new();
    transport.upload(&src, "LIVE").expect("upload to the fake scale");
    let ep = transport.endpoint().cloned().expect("discovered endpoint");
    let _ = std::fs::remove_file(&src);

    // The rows the fake stored are what a read returns.
    assert_eq!(
        fetch_section(&ep, "PLU").expect("read PLU back"),
        section_block("PLU", plu_rows)
    );
    assert_eq!(
        fetch_section(&ep, "DPT").expect("read DPT back"),
        section_block("DPT", dpt_rows)
    );
    // A section the file does not carry is written as empty and stays empty.
    assert_eq!(
        fetch_section(&ep, "INF").expect("read INF back"),
        empty_section_block("INF")
    );

    stop.store(true, Ordering::SeqCst);
}

#[test]
fn preview_reports_differences_and_writes_nothing() {
    let _ports = lock_discovery_ports();
    let scale_port = spawn_fake_scale(Mode::Honest);
    let stop = start_beacon(scale_port);

    // Seed the scale through the real write path, so the state it holds is a
    // file the scale actually accepted.
    let seeded_plu: &[u8] = b"PLU\t1\t1001\tWidget\t6\t0\t100,2\t0\t0\t\r\n";
    let seeded_dpt: &[u8] = b"DPT\t1\tGrocery\t\r\n";
    let mut seeded = ECS_HEADER.as_bytes().to_vec();
    seeded.extend_from_slice(&section_block("PLU", seeded_plu));
    seeded.extend_from_slice(&section_block("DPT", seeded_dpt));
    seeded.extend_from_slice(ECS_FOOTER.as_bytes());
    let seeded_path = std::env::temp_dir().join(format!("dlx50_seed_{}.TMS", std::process::id()));
    std::fs::write(&seeded_path, &seeded).expect("write seeded file");

    let mut transport = ScaleTransport::new();
    transport
        .upload(&seeded_path, "LIVE")
        .expect("seed the fake scale");
    let ep = transport.endpoint().cloned().expect("discovered endpoint");

    // A candidate that reprices the PLU and carries no other section.
    let candidate_plu: &[u8] = b"PLU\t1\t1001\tWidget\t6\t0\t199,2\t0\t0\t\r\n";
    let mut candidate = ECS_HEADER.as_bytes().to_vec();
    candidate.extend_from_slice(&section_block("PLU", candidate_plu));
    candidate.extend_from_slice(ECS_FOOTER.as_bytes());
    let candidate_path =
        std::env::temp_dir().join(format!("dlx50_cand_{}.TMS", std::process::id()));
    std::fs::write(&candidate_path, &candidate).expect("write candidate file");

    let changes = transport.preview(&candidate_path, "LIVE").expect("preview");
    let state = |tag: &str| {
        changes
            .iter()
            .find(|(section, _)| *section == tag)
            .map(|(_, s)| *s)
    };
    assert_eq!(
        state("PLU"),
        Some(SectionPreview::WouldReplace {
            local_rows: 1,
            remote_rows: 1,
        }),
        "a reprice must be reported as a replacement"
    );
    assert!(
        matches!(state("DPT"), Some(SectionPreview::WouldClear { .. })),
        "a section absent from the file would be cleared, got {:?}",
        state("DPT")
    );

    // The point of the mode: the scale still holds the seeded price and the
    // seeded DPT row, even though the candidate would clear DPT.
    assert_eq!(
        fetch_section(&ep, "PLU").expect("read PLU back"),
        section_block("PLU", seeded_plu),
        "preview must not write to the scale"
    );
    assert_eq!(
        fetch_section(&ep, "DPT").expect("read DPT back"),
        section_block("DPT", seeded_dpt),
        "preview must not clear DPT either"
    );

    let _ = std::fs::remove_file(&seeded_path);
    let _ = std::fs::remove_file(&candidate_path);
    stop.store(true, Ordering::SeqCst);
}