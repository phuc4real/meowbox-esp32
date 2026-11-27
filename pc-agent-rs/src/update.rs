//! Is there a newer agent on GitHub Releases? (docs/firmware_v0.2.0.md, 2.1). Only tells; the Settings page
//! downloads and runs the installer when asked.

use std::time::Duration;

pub fn releases_page() -> String {
    format!("https://github.com/{}/releases", crate::REPO)
}

fn num(v: &str) -> (u64, u64, u64) {
    let mut out = [0u64; 3];
    for (i, part) in v.trim_start_matches('v').split('.').take(3).enumerate() {
        let digits: String = part.chars().filter(char::is_ascii_digit).collect();
        out[i] = digits.parse().unwrap_or(0);
    }
    (out[0], out[1], out[2])
}

/// Tag of the latest (non-draft, non-prerelease) release without the "v", e.g. "0.3.1"; None on any error.
pub async fn latest_version() -> Option<String> {
    let url = format!("https://api.github.com/repos/{}/releases/latest", crate::REPO);
    match crate::tools::github_json(&url, Duration::from_secs(8)).await {
        Ok(j) => j
            .get("tag_name")
            .and_then(|t| t.as_str())
            .map(|t| t.trim_start_matches('v').to_owned())
            .filter(|t| !t.is_empty()),
        Err(e) => {
            // offline, rate limit, no release yet: stay quiet
            tracing::debug!("update check failed: {e}");
            None
        }
    }
}

pub fn newer_than(v: &str, running: &str) -> bool {
    !v.is_empty() && num(v) > num(running)
}

pub fn newer_than_running(v: &str) -> bool {
    newer_than(v, crate::VERSION)
}

/// Installed by the installer (Inno Setup leaves unins000.exe next to the exe), as opposed to a dev build.
pub fn installed_build() -> bool {
    std::env::current_exe().ok().and_then(|p| p.parent().map(|d| d.join("unins000.exe").is_file())).unwrap_or(false)
}

/// Download MeowBoxAgent-Setup-<v>.exe into the temp folder; returns its path.
pub async fn download_installer(v: &str) -> anyhow::Result<std::path::PathBuf> {
    let name = format!("MeowBoxAgent-Setup-{v}.exe");
    let url = format!("https://github.com/{}/releases/download/v{v}/{name}", crate::REPO);
    let dst = std::env::temp_dir().join(&name);
    let mut r = crate::tools::web_client().get(url).send().await?.error_for_status()?;
    let mut f = tokio::fs::File::create(&dst).await?;
    use tokio::io::AsyncWriteExt;
    while let Some(chunk) = tokio::time::timeout(Duration::from_secs(60), r.chunk()).await?? {
        f.write_all(&chunk).await?;
    }
    f.flush().await?;
    Ok(dst)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn versions() {
        assert!(newer_than("0.3.1", "0.3.0"));
        assert!(newer_than("v1.0", "0.9.9"));
        assert!(newer_than("0.10.0", "0.9.0"));
        assert!(!newer_than("0.3.0", "0.3.0"));
        assert!(!newer_than("0.2.5", "0.3.0"));
        assert!(!newer_than("", "0.3.0"));
        assert!(newer_than("0.3.1-rc1", "0.3.0"));
    }
}
