//! Port of the Qt-side fake-scale check (`tests/network_check.cpp` on the
//! `qt-network-transport` branch): a local fake scale that speaks the beacon
//! and the read-only UPL exchange, driven through the real backend code.

use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream, UdpSocket};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::Duration;

use tms_editor::{
    discover, empty_section_block, fetch_section, probe_scale, Endpoint, ScaleTransport,
    ECS_FOOTER, ECS_HEADER, NETWORK_SECTIONS, SCALE_BEACON_PORT,
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

fn serve(mut stream: TcpStream, mode: Mode) {
    let mut req = Vec::new();
    let mut chunk = [0u8; 512];
    loop {
        match stream.read(&mut chunk) {
            Ok(0) => return,
            Ok(n) => {
                req.extend_from_slice(&chunk[..n]);
                if req.ends_with(b"\r\n") {
                    break;
                }
            }
            Err(_) => return,
        }
    }
    let text = String::from_utf8_lossy(&req);
    let tag = text.split('\t').nth(1).unwrap_or("").to_owned();
    let response = match mode {
        Mode::Garbled => b"GARBAGE\r\n".to_vec(),
        Mode::Truncated => format!("\r\nDWL\t{tag}\t\r\npartial row").into_bytes(),
        Mode::Honest => {
            if NETWORK_SECTIONS.contains(&tag.as_str()) {
                format!("\r\nDWL\t{tag}\t\r\n{tag}\t1\tOK\t\r\nEND\t{tag}\t\r\n").into_bytes()
            } else {
                empty_section_block(&tag)
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
    thread::spawn(move || {
        for stream in listener.incoming() {
            match stream {
                Ok(s) => serve(s, mode),
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

#[test]
fn fake_scale_end_to_end() {
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

    // The write direction stays refused.
    let err = transport.upload(&out, "LIVE").expect_err("upload must fail");
    assert!(
        err.to_string().to_lowercase().contains("not implemented"),
        "{err}"
    );

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