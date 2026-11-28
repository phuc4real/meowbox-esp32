//! Third-party tools the YouTube relay runs, kept outside the agent in `%LOCALAPPDATA%\MeowBox\bin` so they can be
//! updated without a new agent release: yt-dlp (onedir build) and ffmpeg (only for YouTube live).

pub mod ffmpeg;
pub mod ytdlp;

use sha2::{Digest, Sha256};
use std::path::Path;
use std::time::Duration;
use tokio::io::AsyncWriteExt;

pub const UA: &str = "MeowBoxAgent";

/// Bytes done / total of the download running now (one per kind: yt-dlp, ffmpeg, the agent installer), for the
/// window's progress bars. `total` 0 = nothing running or size unknown.
pub struct Meter {
    done: std::sync::atomic::AtomicU64,
    total: std::sync::atomic::AtomicU64,
}

impl Meter {
    pub const fn new() -> Self {
        Self { done: std::sync::atomic::AtomicU64::new(0), total: std::sync::atomic::AtomicU64::new(0) }
    }
    pub fn set(&self, done: u64, total: u64) {
        use std::sync::atomic::Ordering::Relaxed;
        self.done.store(done, Relaxed);
        self.total.store(total, Relaxed);
    }
    /// (done, total) in bytes.
    pub fn get(&self) -> (u64, u64) {
        use std::sync::atomic::Ordering::Relaxed;
        (self.done.load(Relaxed), self.total.load(Relaxed))
    }
    pub fn clear(&self) {
        self.set(0, 0);
    }
}

impl Default for Meter {
    fn default() -> Self {
        Self::new()
    }
}

/// First `name.exe` on PATH (like shutil.which).
pub fn which(name: &str) -> Option<std::path::PathBuf> {
    let path = std::env::var_os("PATH")?;
    std::env::split_paths(&path).map(|d| d.join(format!("{name}.exe"))).find(|p| p.is_file())
}

/// HTTP client for GitHub (system proxy from the environment, unlike the LAN client). Built once, no idle
/// connections kept, for the same reason as `discovery::lan_client`.
pub fn web_client() -> reqwest::Client {
    static C: std::sync::LazyLock<reqwest::Client> = std::sync::LazyLock::new(|| {
        reqwest::Client::builder().user_agent(UA).pool_max_idle_per_host(0).build().expect("http client")
    });
    C.clone()
}

/// GET a GitHub API URL as JSON.
pub async fn github_json(url: &str, timeout: Duration) -> anyhow::Result<serde_json::Value> {
    let r = web_client()
        .get(url)
        .header("Accept", "application/vnd.github+json")
        .timeout(timeout)
        .send()
        .await?
        .error_for_status()?;
    Ok(r.json().await?)
}

/// Small download into memory.
pub async fn get_bytes(url: &str, timeout: Duration) -> anyhow::Result<Vec<u8>> {
    let r = web_client().get(url).timeout(timeout).send().await?.error_for_status()?;
    Ok(r.bytes().await?.to_vec())
}

/// The checksum for `name` in a `sha256sum`-style list ("<hex>  <name>" or "<hex> *<name>").
pub fn checksum_for(sums: &str, name: &str) -> Option<String> {
    sums.lines().find_map(|l| {
        let mut it = l.split_whitespace();
        let (hash, file) = (it.next()?, it.next_back()?);
        (file.trim_start_matches('*') == name).then(|| hash.to_ascii_lowercase())
    })
}

/// Stream a download to `dest`, checking its SHA-256; `progress(done, total)` after each chunk. Read timeout per
/// chunk, not for the whole file (ffmpeg is ~75 MB).
pub async fn download_checked(
    url: &str,
    dest: &Path,
    want_sha256: &str,
    mut progress: impl FnMut(u64, u64),
) -> anyhow::Result<()> {
    let mut r = web_client().get(url).send().await?.error_for_status()?;
    let total = r.content_length().unwrap_or(0);
    let mut f = tokio::fs::File::create(dest).await?;
    let mut h = Sha256::new();
    let mut done = 0u64;
    while let Some(chunk) = tokio::time::timeout(Duration::from_secs(60), r.chunk()).await?? {
        h.update(&chunk);
        f.write_all(&chunk).await?;
        done += chunk.len() as u64;
        progress(done, total);
    }
    f.flush().await?;
    drop(f);
    if hex::encode(h.finalize()) != want_sha256 {
        let _ = tokio::fs::remove_file(dest).await;
        anyhow::bail!("checksum mismatch, not installed");
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn checksums() {
        let sums = "abc123  yt-dlp\nDEF456  yt-dlp_win.zip\n789 *ffmpeg.zip\n";
        assert_eq!(checksum_for(sums, "yt-dlp_win.zip").as_deref(), Some("def456"));
        assert_eq!(checksum_for(sums, "ffmpeg.zip").as_deref(), Some("789"));
        assert_eq!(checksum_for(sums, "yt-dlp_win_x86.zip"), None);
    }
}
