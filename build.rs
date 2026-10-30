fn main() {
    println!("cargo:rerun-if-env-changed=GITHUB_REF_NAME");
    let version = std::env::var("GITHUB_REF_NAME")
        .ok()
        .and_then(|name| name.strip_prefix('v').map(str::to_owned))
        .filter(|name| !name.is_empty())
        .unwrap_or_else(|| std::env::var("CARGO_PKG_VERSION").unwrap_or_else(|_| "0.0.0".into()));
    println!("cargo:rustc-env=TMS_APP_VERSION={version}");
}
