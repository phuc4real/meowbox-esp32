//! ffmpeg, only for YouTube live: yt-dlp will not download live HLS without it. The agent fetches an LGPL ffmpeg the
//! first time a live stream is played (or from Settings): FFmpeg-Builds (formerly BtbN) "win64-lgpl-shared" (~75 MB zip:
//! ffmpeg.exe + its DLLs; ffprobe and ffplay are left out), checked against the release's checksums.sha256. Live
//! audio is only copied (AAC in MPEG-TS), never re-encoded, so no GPL/nonfree encoder is needed.

use std::os::windows::process::CommandExt;
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::time::Duration;

/// BtbN/FFmpeg-Builds moved here (late 2026; the old name still redirects).
const REPO: &str = "FFmpeg-Builds/FFmpeg-Builds";

#[derive(Debug, Clone, Default)]
pub struct Status {
    pub busy: bool,
    pub error: String,
    pub pct: u32,
    pub path: Option<PathBuf>,
    /// The ffmpeg in use is the one the agent downloaded (not the user's own).
    pub own: bool,
}

#[derive(Default)]
struct State {
    busy: bool,
    error: String,
    pct: u32,
    fresh: bool,
}

static STATE: Mutex<State> = Mutex::new(State { busy: false, error: String::new(), pct: 0, fresh: false });
/// One download at a time; a second caller waits for the first.
static INSTALL: tokio::sync::Mutex<()> = tokio::sync::Mutex::const_new(());

fn state() -> std::sync::MutexGuard<'static, State> {
    STATE.lock().unwrap_or_else(|e| e.into_inner())
}

/// `bin\ffmpeg`: one folder per downloaded build (`<build>\ffmpeg.exe` + DLLs) and `current` naming the one in use.
/// A running stream locks its ffmpeg's files, so a new build never replaces a folder, it goes next to it. The Python
/// agent kept ffmpeg.exe straight in `bin\ffmpeg`; that is still used until a build is downloaded.
pub fn dir() -> PathBuf {
    crate::bin_dir().join("ffmpeg")
}

fn current() -> Option<PathBuf> {
    let name = std::fs::read_to_string(dir().join("current")).ok()?.trim().to_owned();
    let exe = dir().join(name).join("ffmpeg.exe");
    (exe.is_file()).then_some(exe)
}

/// ffmpeg in PATH first (the user's own), else the one the agent downloaded.
pub fn path() -> Option<PathBuf> {
    super::which("ffmpeg").or_else(current).or_else(|| Some(dir().join("ffmpeg.exe")).filter(|p| p.is_file()))
}

pub fn version() -> Option<String> {
    let out = std::process::Command::new(path()?)
        .arg("-version")
        .creation_flags(crate::win::CREATE_NO_WINDOW)
        .output()
        .ok()?;
    let text = String::from_utf8_lossy(&out.stdout);
    Some(text.strip_prefix("ffmpeg version ").and_then(|r| r.split_whitespace().next()).unwrap_or("?").to_owned())
}

pub fn status() -> Status {
    let s = state();
    let path = path();
    let own = path.as_ref().is_some_and(|p| p.starts_with(dir()));
    Status { busy: s.busy, error: s.error.clone(), pct: s.pct, path, own }
}

/// (major, minor) of a release build for win64, LGPL, shared: `ffmpeg-n9.0.2-982-g836df4e858-win64-lgpl-shared-9.0.zip`
/// (and the older `ffmpeg-n7.1-latest-win64-lgpl-shared-7.1.zip`). Master builds (`ffmpeg-N-…`), GPL, static and
/// arm64 builds are not taken.
pub fn asset_version(name: &str) -> Option<(u32, u32)> {
    let rest = name.strip_prefix("ffmpeg-n")?.strip_suffix(".zip")?;
    let (_, tail) = rest.rsplit_once("-win64-lgpl-shared-")?;
    let (a, b) = tail.split_once('.')?;
    if !(a.bytes().all(|c| c.is_ascii_digit()) && b.bytes().all(|c| c.is_ascii_digit())) {
        return None;
    }
    Some((a.parse().ok()?, b.parse().ok()?))
}

/// (name, download url, size) of the newest release build.
async fn asset() -> anyhow::Result<(String, String, u64)> {
    let j =
        super::github_json(&format!("https://api.github.com/repos/{REPO}/releases/latest"), Duration::from_secs(15))
            .await?;
    let assets = j.get("assets").and_then(|a| a.as_array()).cloned().unwrap_or_default();
    assets
        .iter()
        .filter_map(|a| {
            let name = a.get("name")?.as_str()?;
            Some((asset_version(name)?, name, a.get("browser_download_url")?.as_str()?, a.get("size")?.as_u64()?))
        })
        .max_by_key(|x| x.0)
        .map(|(_, n, u, s)| (n.to_owned(), u.to_owned(), s))
        .ok_or_else(|| anyhow::anyhow!("no win64 LGPL shared build in the latest release"))
}

fn unpack(zip_path: &Path, new: &Path) -> anyhow::Result<()> {
    let mut z = zip::ZipArchive::new(std::fs::File::open(zip_path)?)?;
    let _ = std::fs::remove_dir_all(new);
    std::fs::create_dir_all(new)?;
    for i in 0..z.len() {
        let mut f = z.by_index(i)?;
        let name = f.name()?.replace('\\', "/");
        let base = name.rsplit('/').next().unwrap_or("").to_owned();
        let keep =
            (name.contains("/bin/") && (base == "ffmpeg.exe" || base.ends_with(".dll"))) || base == "LICENSE.txt";
        if keep && !base.is_empty() && !f.is_dir() {
            std::io::copy(&mut f, &mut std::fs::File::create(new.join(&base))?)?;
        }
    }
    if !new.join("ffmpeg.exe").is_file() {
        anyhow::bail!("ffmpeg.exe not found in the archive");
    }
    Ok(())
}

/// Delete builds other than `keep` and the Python agent's flat layout; files a running ffmpeg holds stay until next
/// time.
fn cleanup(keep: &str) {
    let Ok(rd) = std::fs::read_dir(dir()) else { return };
    for e in rd.flatten() {
        let name = e.file_name().to_string_lossy().into_owned();
        let p = e.path();
        if p.is_dir() {
            if name != keep {
                let _ = std::fs::remove_dir_all(&p);
            }
        } else if name == "ffmpeg.exe" || name.ends_with(".dll") || name == "LICENSE.txt" {
            let _ = std::fs::remove_file(&p);
        }
    }
}

async fn install_inner() -> anyhow::Result<String> {
    let (name, url, size) = asset().await?;
    let sums = String::from_utf8_lossy(
        &super::get_bytes(
            &format!("https://github.com/{REPO}/releases/latest/download/checksums.sha256"),
            Duration::from_secs(30),
        )
        .await?,
    )
    .into_owned();
    let want = super::checksum_for(&sums, &name).ok_or_else(|| anyhow::anyhow!("no checksum for {name}"))?;
    tracing::info!("ffmpeg: downloading {name} ({} MB)", size / 1_048_576);
    let tmp = std::env::temp_dir().join(format!("meowbox-{name}"));
    super::download_checked(&url, &tmp, &want, |done, total| {
        state().pct = (done * 100 / total.max(size).max(1)) as u32;
    })
    .await?;
    let build = name.trim_end_matches(".zip").to_owned();
    let (zp, b2) = (tmp.clone(), build.clone());
    let r = tokio::task::spawn_blocking(move || -> anyhow::Result<()> {
        let partial = dir().join(format!("{b2}.partial"));
        let dest = dir().join(&b2);
        unpack(&zp, &partial)?;
        if dest.exists() {
            // the same build again (Settings "download again"): replace it if nothing runs from it
            std::fs::remove_dir_all(&dest)?;
        }
        std::fs::rename(&partial, &dest)?;
        let cur = dir().join("current.tmp");
        std::fs::write(&cur, &b2)?;
        std::fs::rename(&cur, dir().join("current"))?;
        cleanup(&b2);
        Ok(())
    })
    .await?;
    let _ = std::fs::remove_file(&tmp);
    if r.is_err() {
        let _ = std::fs::remove_dir_all(dir().join(format!("{build}.partial")));
    }
    r?;
    state().fresh = true;
    let v = version().unwrap_or_else(|| "?".into());
    tracing::info!("ffmpeg {v} installed in {}", dir().join(&build).display());
    Ok(v)
}

/// Clears `busy` however the install ends.
struct Busy;

impl Drop for Busy {
    fn drop(&mut self) {
        state().busy = false;
    }
}

/// Download + verify + unpack ffmpeg into bin\ffmpeg\<build>. One at a time: a second caller waits for the first
/// and then finds it installed. Returns the version; the error is a short reason.
pub async fn install() -> anyhow::Result<String> {
    let _one = INSTALL.lock().await;
    let _busy = Busy;
    if current().is_some() && state().fresh {
        return Ok(version().unwrap_or_else(|| "?".into()));
    }
    {
        let mut s = state();
        s.busy = true;
        s.error.clear();
        s.pct = 0;
    }
    let r = install_inner().await;
    if let Err(e) = &r {
        state().error = e.to_string().chars().take(120).collect();
        tracing::warn!("ffmpeg install failed: {e}");
    }
    r
}

/// Start an install in the background unless one is running; `on_done` runs after it (success or not). `busy` is set
/// here, before the task starts, so a second call right after this one starts nothing; `install` clears it at the end.
pub fn install_background(on_done: impl FnOnce() + Send + 'static) {
    {
        let mut s = state();
        if s.busy {
            return;
        }
        s.busy = true;
    }
    tokio::spawn(async move {
        let _ = install().await;
        on_done();
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn asset_names() {
        assert_eq!(asset_version("ffmpeg-n7.1-latest-win64-lgpl-shared-7.1.zip"), Some((7, 1)));
        assert_eq!(asset_version("ffmpeg-n8.0-latest-win64-lgpl-shared-8.0.zip"), Some((8, 0)));
        assert_eq!(asset_version("ffmpeg-n7.1-latest-win64-gpl-shared-7.1.zip"), None);
        assert_eq!(asset_version("ffmpeg-n7.1-latest-win64-lgpl-shared-7.1.tar.xz"), None);
        assert_eq!(asset_version("ffmpeg-master-latest-win64-lgpl-shared.zip"), None);
        // names since late 2026
        assert_eq!(asset_version("ffmpeg-n9.0.2-982-g836df4e858-win64-lgpl-shared-9.0.zip"), Some((9, 0)));
        assert_eq!(asset_version("ffmpeg-n8.1.3-41-ga8029b6528-win64-lgpl-shared-8.1.zip"), Some((8, 1)));
        assert_eq!(asset_version("ffmpeg-N-129333-g7fdb4c2341-win64-lgpl-shared.zip"), None);
        assert_eq!(asset_version("ffmpeg-n9.0.2-982-g836df4e858-winarm64-lgpl-shared-9.0.zip"), None);
        assert_eq!(asset_version("ffmpeg-n9.0.2-982-g836df4e858-win64-gpl-shared-9.0.zip"), None);
    }

    /// Downloads ~75 MB into %LOCALAPPDATA%\MeowBox\bin\ffmpeg: run by hand with LOCALAPPDATA pointing somewhere
    /// disposable: `cargo test -- --ignored ffmpeg_download`.
    #[tokio::test]
    #[ignore]
    async fn ffmpeg_download() {
        let v = install().await.expect("install");
        assert!(dir().join("ffmpeg.exe").is_file());
        assert!(std::fs::read_dir(dir()).unwrap().any(|e| e.unwrap().file_name().to_string_lossy().ends_with(".dll")));
        assert!(!v.is_empty());
        assert!(status().pct >= 99 && !status().busy && status().error.is_empty());
    }
}
