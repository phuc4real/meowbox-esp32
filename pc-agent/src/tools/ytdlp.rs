//! yt-dlp, the official onedir build (`yt-dlp_win.zip`: yt-dlp.exe + _internal\), checked against the release's
//! SHA2-256SUMS. YouTube changes often, so yt-dlp must be updatable without a new agent release.
//!
//! Each version gets its own folder, `bin\yt-dlp\<tag>\`, and `bin\yt-dlp\current` names the one in use. Windows
//! locks a running exe's files, so an update never touches a folder a stream may be using: new streams use the new
//! tag at once and old folders are deleted when nothing runs from them any more.

use std::path::PathBuf;
use std::time::{Duration, SystemTime};

use std::sync::atomic::{AtomicUsize, Ordering};

const REPO: &str = "yt-dlp/yt-dlp";

/// yt-dlp processes running now (relay streams and lookups): old folders are only deleted when this is 0.
static RUNNING: AtomicUsize = AtomicUsize::new(0);

/// Held while a yt-dlp process runs.
pub struct InUse(());

impl InUse {
    pub fn new() -> Self {
        RUNNING.fetch_add(1, Ordering::SeqCst);
        Self(())
    }
}

impl Default for InUse {
    fn default() -> Self {
        Self::new()
    }
}

impl Drop for InUse {
    fn drop(&mut self) {
        RUNNING.fetch_sub(1, Ordering::SeqCst);
    }
}
const ASSET: &str = "yt-dlp_win.zip";

pub fn root() -> PathBuf {
    crate::bin_dir().join("yt-dlp")
}

fn current_file() -> PathBuf {
    root().join("current")
}

fn stamp() -> PathBuf {
    root().join("checked")
}

/// Tag of the installed version (from `current`), if its exe exists.
pub fn version() -> Option<String> {
    let tag = std::fs::read_to_string(current_file()).ok()?.trim().to_owned();
    (!tag.is_empty() && root().join(&tag).join("yt-dlp.exe").is_file()).then_some(tag)
}

/// yt-dlp.exe of the installed version.
pub fn exe() -> Option<PathBuf> {
    version().map(|t| root().join(t).join("yt-dlp.exe"))
}

/// Tag of the latest release on GitHub, None on any error.
pub async fn latest() -> Option<String> {
    match super::github_json(&format!("https://api.github.com/repos/{REPO}/releases/latest"), Duration::from_secs(10))
        .await
    {
        Ok(j) => j.get("tag_name").and_then(|t| t.as_str()).filter(|t| !t.is_empty()).map(str::to_owned),
        Err(e) => {
            tracing::info!("yt-dlp latest check failed: {e}");
            None
        }
    }
}

/// The folder inside the zip that holds yt-dlp.exe ("" when it is at the top).
fn exe_prefix(names: &[String]) -> Option<String> {
    names
        .iter()
        .filter_map(|n| n.replace('\\', "/").strip_suffix("yt-dlp.exe").map(str::to_owned))
        .filter(|p| p.is_empty() || p.ends_with('/'))
        .min_by_key(|p| p.len())
}

/// Extract the part of the zip under the exe's folder into `dest`.
fn unpack(zip_path: &std::path::Path, dest: &std::path::Path) -> anyhow::Result<()> {
    let mut z = zip::ZipArchive::new(std::fs::File::open(zip_path)?)?;
    let names: Vec<String> = z.file_names().filter_map(|n| n.ok().map(|n| n.into_owned())).collect();
    let prefix = exe_prefix(&names).ok_or_else(|| anyhow::anyhow!("yt-dlp.exe not found in the archive"))?;
    for i in 0..z.len() {
        let mut f = z.by_index(i)?;
        let Some(rel) = f.enclosed_name() else { continue }; // refuses ../ and absolute paths
        let rel_s = rel.to_string_lossy().replace('\\', "/");
        let Some(inner) = rel_s.strip_prefix(prefix.as_str()) else { continue };
        if inner.is_empty() {
            continue;
        }
        let out = dest.join(inner);
        if f.is_dir() {
            std::fs::create_dir_all(&out)?;
        } else {
            if let Some(p) = out.parent() {
                std::fs::create_dir_all(p)?;
            }
            std::io::copy(&mut f, &mut std::fs::File::create(&out)?)?;
        }
    }
    Ok(())
}

/// Delete version folders other than `keep`, only while no yt-dlp runs (a running one may still read its folder).
pub fn cleanup(keep: &str) {
    if RUNNING.load(Ordering::SeqCst) > 0 {
        return;
    }
    let Ok(rd) = std::fs::read_dir(root()) else { return };
    for e in rd.flatten() {
        let name = e.file_name().to_string_lossy().into_owned();
        if e.path().is_dir() && name != keep && std::fs::remove_dir_all(e.path()).is_ok() {
            tracing::info!("yt-dlp {name} removed");
        }
    }
}

/// Download the latest onedir build, check it, unpack it next to the one in use and switch to it. Returns the
/// installed tag; the error is a short reason for the user.
pub async fn update(progress: impl Fn(&str)) -> anyhow::Result<String> {
    let tag = latest().await.ok_or_else(|| anyhow::anyhow!("cannot reach GitHub"))?;
    if version().as_deref() == Some(tag.as_str()) {
        return Ok(tag);
    }
    let base = format!("https://github.com/{REPO}/releases/download/{tag}/");
    progress("sums");
    let sums =
        String::from_utf8_lossy(&super::get_bytes(&format!("{base}SHA2-256SUMS"), Duration::from_secs(20)).await?)
            .into_owned();
    let want =
        super::checksum_for(&sums, ASSET).ok_or_else(|| anyhow::anyhow!("no checksum for {ASSET} in this release"))?;
    progress("download");
    std::fs::create_dir_all(root())?;
    let zip_path = root().join(format!("{tag}.zip"));
    super::download_checked(&format!("{base}{ASSET}"), &zip_path, &want, |_, _| {}).await?;
    progress("unpack");
    let partial = root().join(format!("{tag}.partial"));
    let dest = root().join(&tag);
    let (zp, pp, dp) = (zip_path.clone(), partial.clone(), dest.clone());
    let r = tokio::task::spawn_blocking(move || -> anyhow::Result<()> {
        let _ = std::fs::remove_dir_all(&pp);
        unpack(&zp, &pp)?;
        if !pp.join("yt-dlp.exe").is_file() {
            anyhow::bail!("yt-dlp.exe missing after unpacking");
        }
        let _ = std::fs::remove_dir_all(&dp);
        std::fs::rename(&pp, &dp)?;
        Ok(())
    })
    .await?;
    let _ = std::fs::remove_file(&zip_path);
    if let Err(e) = r {
        let _ = std::fs::remove_dir_all(&partial);
        return Err(e);
    }
    let tmp = root().join("current.tmp");
    std::fs::write(&tmp, &tag)?;
    std::fs::rename(&tmp, current_file())?;
    cleanup(&tag);
    tracing::info!("yt-dlp {tag} installed in {}", dest.display());
    Ok(tag)
}

/// Agent start: install yt-dlp if missing (the installer may have been offline); with `daily_check`, update it at
/// most once a day when GitHub has a newer release. Errors are logged, never returned.
pub async fn maintain(daily_check: bool) {
    let r: anyhow::Result<()> = async {
        if let Some(tag) = version() {
            cleanup(&tag); // folders left by an update while a stream was running
        } else {
            update(|_| {}).await?;
            let _ = std::fs::write(stamp(), "");
            return Ok(());
        }
        if !daily_check {
            return Ok(());
        }
        let fresh = std::fs::metadata(stamp())
            .and_then(|m| m.modified())
            .ok()
            .and_then(|t| SystemTime::now().duration_since(t).ok())
            .is_some_and(|age| age < Duration::from_secs(86_400));
        if fresh {
            return Ok(());
        }
        let latest = latest().await;
        let _ = std::fs::write(stamp(), "");
        if latest.is_some() && latest != version() {
            update(|_| {}).await?;
        }
        Ok(())
    }
    .await;
    if let Err(e) = r {
        tracing::warn!("yt-dlp maintenance: {e}");
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn finds_the_exe_folder() {
        let v = |a: &[&str]| a.iter().map(|s| s.to_string()).collect::<Vec<_>>();
        assert_eq!(exe_prefix(&v(&["yt-dlp.exe", "_internal/base.dll"])).as_deref(), Some(""));
        assert_eq!(exe_prefix(&v(&["yt-dlp_win/", "yt-dlp_win/yt-dlp.exe"])).as_deref(), Some("yt-dlp_win/"));
        assert_eq!(exe_prefix(&v(&["notyt-dlp.exe"])), None);
    }
}
