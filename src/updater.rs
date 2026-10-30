use crate::{compare_versions, parse_sha256sums, sha256_hex};
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::mpsc::{self, Receiver, TryRecvError};
use std::time::Duration;

const RELEASE_API: &str =
    "https://api.github.com/repos/kubuntugamer/dlx50-tms-editor/releases/latest";

#[derive(Clone, Debug)]
pub struct Release {
    pub tag: String,
    pub notes: String,
    pub asset_name: String,
    pub asset_url: String,
    pub checksums_url: Option<String>,
}
fn settings_path() -> PathBuf {
    #[cfg(target_os = "windows")]
    {
        if let Some(p) = std::env::var_os("APPDATA") {
            return PathBuf::from(p).join("dlx50-tms-editor/settings");
        }
    }
    #[cfg(target_os = "macos")]
    {
        if let Some(p) = std::env::var_os("HOME") {
            return PathBuf::from(p).join("Library/Application Support/dlx50-tms-editor/settings");
        }
    }
    if let Some(p) = std::env::var_os("XDG_CONFIG_HOME") {
        return PathBuf::from(p).join("dlx50-tms-editor/settings");
    }
    std::env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_else(std::env::temp_dir)
        .join(".config/dlx50-tms-editor/settings")
}
pub fn startup_check_allowed() -> bool {
    if std::env::args().any(|a| a.eq_ignore_ascii_case("--no-update-check")) {
        return false;
    }
    fs::read_to_string(settings_path())
        .map(|s| {
            !s.lines()
                .any(|l| l.trim().eq_ignore_ascii_case("autoUpdateCheck=false"))
        })
        .unwrap_or(true)
}
pub fn schedule_install(release: Release) -> Receiver<Result<String, String>> {
    let (tx, rx) = mpsc::channel();
    std::thread::spawn(move || {
        let _ = tx.send(download_and_install(&release));
    });
    rx
}
pub fn poll_install(rx: &Receiver<Result<String, String>>) -> Option<Result<String, String>> {
    match rx.try_recv() {
        Ok(result) => Some(result),
        Err(TryRecvError::Empty) => None,
        Err(TryRecvError::Disconnected) => {
            Some(Err("update installer stopped unexpectedly".into()))
        }
    }
}
pub fn schedule_startup_check() -> Receiver<Result<Option<Release>, String>> {
    let (tx, rx) = mpsc::channel();
    std::thread::spawn(move || {
        std::thread::sleep(Duration::from_secs(3));
        let _ = tx.send(fetch_latest());
    });
    rx
}
pub fn poll_check(
    rx: &Receiver<Result<Option<Release>, String>>,
) -> Option<Result<Option<Release>, String>> {
    match rx.try_recv() {
        Ok(r) => Some(r),
        Err(TryRecvError::Empty) => None,
        Err(TryRecvError::Disconnected) => Some(Err("update check stopped unexpectedly".into())),
    }
}
pub fn set_startup_check_enabled(enabled: bool) -> Result<(), String> {
    let path = settings_path();
    let parent = path
        .parent()
        .ok_or("could not determine settings directory")?;
    fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    fs::write(path, format!("autoUpdateCheck={enabled}\n")).map_err(|e| e.to_string())
}
pub fn disable_startup_check() -> Result<(), String> {
    set_startup_check_enabled(false)
}

pub fn fetch_latest() -> Result<Option<Release>, String> {
    let client = reqwest::blocking::Client::builder()
        .user_agent(concat!("dlx50-tms-editor/", env!("TMS_APP_VERSION")))
        .timeout(Duration::from_secs(15))
        .build()
        .map_err(|e| e.to_string())?;
    let v: serde_json::Value = client
        .get(RELEASE_API)
        .header("Accept", "application/vnd.github+json")
        .header("X-GitHub-Api-Version", "2022-11-28")
        .send()
        .map_err(|e| e.to_string())?
        .error_for_status()
        .map_err(|e| e.to_string())?
        .json()
        .map_err(|e| e.to_string())?;
    let tag = v["tag_name"].as_str().unwrap_or_default().to_owned();
    if tag.is_empty() || compare_versions(&tag, env!("TMS_APP_VERSION")) <= 0 {
        return Ok(None);
    }
    #[cfg(target_os = "windows")]
    let wanted = "tms_editor-windows-x64.zip";
    #[cfg(target_os = "linux")]
    let wanted_suffix = ".deb";
    #[cfg(not(any(target_os = "windows", target_os = "linux")))]
    return Err("automatic updates are supported only for Linux and Windows releases".into());
    let assets = v["assets"].as_array().ok_or("release has no assets")?;
    let mut chosen = None;
    let mut sums = None;
    for asset in assets {
        let name = asset["name"].as_str().unwrap_or_default();
        let url = asset["browser_download_url"].as_str().unwrap_or_default();
        if name.eq_ignore_ascii_case("SHA256SUMS.txt") {
            sums = Some(url.to_owned());
            continue;
        }
        #[cfg(target_os = "windows")]
        let matches = name.eq_ignore_ascii_case(wanted);
        #[cfg(target_os = "linux")]
        let matches = name.to_ascii_lowercase().ends_with(wanted_suffix);
        if matches && !url.is_empty() {
            chosen = Some((name.to_owned(), url.to_owned()));
        }
    }
    let (asset_name, asset_url) = chosen
        .ok_or("no compatible Linux .deb or Windows zip asset was found in the latest release")?;
    Ok(Some(Release {
        tag,
        notes: v["body"].as_str().unwrap_or_default().to_owned(),
        asset_name,
        asset_url,
        checksums_url: sums,
    }))
}

fn download(url: &str) -> Result<Vec<u8>, String> {
    let client = reqwest::blocking::Client::builder()
        .user_agent("dlx50-tms-editor-updater")
        .timeout(Duration::from_secs(180))
        .build()
        .map_err(|e| e.to_string())?;
    client
        .get(url)
        .send()
        .map_err(|e| e.to_string())?
        .error_for_status()
        .map_err(|e| e.to_string())?
        .bytes()
        .map(|b| b.to_vec())
        .map_err(|e| e.to_string())
}
pub fn download_and_install(release: &Release) -> Result<String, String> {
    let bytes = download(&release.asset_url)?;
    if let Some(url) = &release.checksums_url {
        let sums = download(url)
            .map_err(|e| format!("could not fetch SHA256SUMS.txt; install cancelled: {e}"))?;
        let expected = parse_sha256sums(&sums, &release.asset_name).ok_or_else(|| {
            format!(
                "SHA256SUMS.txt has no entry for {}; install cancelled",
                release.asset_name
            )
        })?;
        let actual = sha256_hex(&bytes);
        if expected != actual {
            return Err(format!("checksum mismatch for {} — refusing to install (expected {expected}, got {actual})",release.asset_name));
        }
    }
    let stage = std::env::temp_dir().join(format!(
        "dlx50-update-{}-{}",
        std::process::id(),
        release.asset_name
    ));
    fs::write(&stage, &bytes).map_err(|e| format!("could not stage update: {e}"))?;
    #[cfg(target_os = "linux")]
    {
        install_deb(&stage)?;
        return Ok("Update installed. Restarting the editor.".into());
    }
    #[cfg(target_os = "windows")]
    {
        install_zip(&stage)?;
        return Ok(
            "Update is staged. The editor will close and relaunch after the files are replaced."
                .into(),
        );
    }
    #[allow(unreachable_code)]
    Err(format!(
        "automatic install is not available on this platform; downloaded file: {}",
        stage.display()
    ))
}
#[cfg(target_os = "linux")]
fn install_deb(file: &Path) -> Result<(), String> {
    let status = Command::new("pkexec")
        .arg("dpkg")
        .arg("-i")
        .arg(file)
        .status()
        .map_err(|e| {
            format!(
                "pkexec could not be started: {e}; package remains at {}",
                file.display()
            )
        })?;
    if !status.success() {
        return Err(format!(
            "installation was not completed (exit {}). Package remains at {}",
            status.code().map_or("unknown".into(), |c| c.to_string()),
            file.display()
        ));
    }
    let current = std::env::current_exe().ok();
    let candidates = [
        Path::new("/usr/bin/dlx50-tms-editor"),
        Path::new("/opt/dlx50-tms-editor/tms_editor"),
    ];
    let relaunch = candidates
        .iter()
        .copied()
        .find(|p| p.exists())
        .or(current.as_deref())
        .ok_or("update installed; please restart the editor")?;
    Command::new(relaunch)
        .args(std::env::args_os().skip(1))
        .spawn()
        .map_err(|e| format!("update installed but could not relaunch: {e}"))?;
    std::process::exit(0);
}
#[cfg(target_os = "windows")]
fn install_zip(file: &Path) -> Result<(), String> {
    let exe = std::env::current_exe().map_err(|e| e.to_string())?;
    let app = exe
        .parent()
        .ok_or("could not determine application directory")?;
    let update = app.join("_update");
    let unzip = update.join("new");
    fs::create_dir_all(&update).map_err(|e| e.to_string())?;
    let persisted = update.join(file.file_name().ok_or("update archive has no filename")?);
    fs::copy(file, &persisted).map_err(|e| e.to_string())?;
    let paths = [
        app.to_string_lossy().into_owned(),
        unzip.to_string_lossy().into_owned(),
        persisted.to_string_lossy().into_owned(),
        update.to_string_lossy().into_owned(),
        exe.to_string_lossy().into_owned(),
    ];
    let script = build_windows_helper(&paths)?;
    let bat = update.join("tms_editor_update.bat");
    fs::write(&bat, script).map_err(|e| e.to_string())?;
    Command::new("cmd.exe")
        .args(["/c"])
        .arg(&bat)
        .current_dir(app)
        .spawn()
        .map_err(|e| e.to_string())?;
    std::process::exit(0);
}
#[cfg(target_os = "windows")]
pub fn build_windows_helper(p: &[String; 5]) -> Result<String, String> {
    for path in p {
        if path.is_empty() || path.chars().any(|c| c == '"' || c < ' ') {
            return Err(format!("unsafe update helper path: {path}"));
        }
    }
    let esc = |s: &str| s.replace('%', "%%");
    let quote = |s: &str| format!("\"{}\"", esc(s));
    let (app, unzip, zip, update, exe) = (&p[0], &p[1], &p[2], &p[3], &p[4]);
    Ok(format!("@echo off\r\nrem Paths are quoted and validated before use.\r\ntimeout /t 2 /nobreak >nul\r\nsetlocal\r\nset \"TMS_UPDATE_ZIP={}\"\r\nset \"TMS_UPDATE_UNZIP={}\"\r\nif exist {} (\r\n rmdir /s /q {}\r\n if errorlevel 1 goto :fail\r\n)\r\npowershell -NoProfile -ExecutionPolicy Bypass -Command \"Expand-Archive -Force -LiteralPath $env:TMS_UPDATE_ZIP -DestinationPath $env:TMS_UPDATE_UNZIP\"\r\nif errorlevel 1 goto :fail\r\nxcopy \"%TMS_UPDATE_UNZIP%\\tms_editor\\*\" {} /E /Y /I /Q /R /H\r\nif errorlevel 1 goto :fail\r\nrmdir /s /q {}\r\nstart \"\" {}\r\nexit /b 0\r\n:fail\r\necho Update failed. Nothing was restarted. Archive: {}\r\npause >nul\r\nexit /b 1\r\n",esc(zip),esc(unzip),quote(unzip),quote(unzip),quote(app),quote(update),quote(exe),quote(zip)))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn checksum_parser_rejects_missing_asset() {
        let sums = format!("{}  other.zip\\n", "a".repeat(64));
        assert!(parse_sha256sums(sums.as_bytes(), "app.zip").is_none());
    }
    #[cfg(target_os = "windows")]
    #[test]
    fn helper_refuses_quotes() {
        let p = [
            "a\"b".into(),
            "b".into(),
            "c".into(),
            "d".into(),
            "e".into(),
        ];
        assert!(build_windows_helper(&p).is_err());
    }
}
