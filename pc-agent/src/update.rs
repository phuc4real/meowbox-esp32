//! Is there a newer agent on GitHub Releases? (docs/firmware_v0.2.0.md, 2.1). Only tells; the Settings page
//! downloads and runs the installer when asked.

use std::time::Duration;

pub fn releases_page() -> String {
    format!("https://github.com/{}/releases", crate::REPO)
}

fn num(v: &str) -> (u64, u64, u64) {
    let mut out = [0u64; 3];
    for (i, part) in v.trim_start_matches('v').split('.').take(3).enumerate() {
        // leading digits only: "0-rc1" is 0, not 1
        let digits: String = part.chars().take_while(char::is_ascii_digit).collect();
        out[i] = digits.parse().unwrap_or(0);
    }
    (out[0], out[1], out[2])
}

/// The newest agent on GitHub: the tag without the "v" (e.g. "0.3.1") of the latest release, not a draft or a
/// prerelease, that carries the agent's installer (a firmware-only release is no new agent); None on any error.
pub async fn latest_version() -> Option<String> {
    let url = format!("https://api.github.com/repos/{}/releases?per_page=20", crate::REPO);
    match crate::tools::github_json(&url, Duration::from_secs(8)).await {
        Ok(j) => newest_agent(&j),
        Err(e) => {
            // offline, rate limit, no release yet: stay quiet
            tracing::debug!("update check failed: {e}");
            None
        }
    }
}

/// The first published release in a GitHub release list (newest first) that has the agent's installer.
fn newest_agent(list: &serde_json::Value) -> Option<String> {
    let published = |r: &&serde_json::Value| {
        !r.get("draft").and_then(|v| v.as_bool()).unwrap_or(false)
            && !r.get("prerelease").and_then(|v| v.as_bool()).unwrap_or(false)
    };
    list.as_array()?.iter().filter(published).find_map(agent_release)
}

/// The version of a GitHub release JSON when it has `MeowBoxAgent-Setup-<version>.exe` among its assets.
fn agent_release(j: &serde_json::Value) -> Option<String> {
    let v = j.get("tag_name")?.as_str()?.trim_start_matches('v').to_owned();
    let want = format!("MeowBoxAgent-Setup-{v}.exe");
    let assets = j.get("assets")?.as_array()?;
    let has = assets.iter().any(|a| a.get("name").and_then(|n| n.as_str()) == Some(want.as_str()));
    (valid_version(&v) && has).then_some(v)
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

/// A release version as the release script makes it: 2 to 4 numbers with dots ("0.3.0"). Anything else (path
/// characters, a prerelease suffix) is never put into a URL or a file name.
pub fn valid_version(v: &str) -> bool {
    let parts: Vec<&str> = v.split('.').collect();
    (2..=4).contains(&parts.len())
        && parts.iter().all(|p| !p.is_empty() && p.len() <= 6 && p.bytes().all(|b| b.is_ascii_digit()))
}

/// Bytes of the installer download running now.
pub static METER: crate::tools::Meter = crate::tools::Meter::new();

/// Download MeowBoxAgent-Setup-<v>.exe into the temp folder and check it against the release's SHA2-256SUMS (made by
/// firmware/tools/release.py) before anything runs it. A release without that file is refused: the user downloads
/// it from the release page instead.
pub async fn download_installer(v: &str) -> anyhow::Result<std::path::PathBuf> {
    if !valid_version(v) {
        anyhow::bail!("unexpected version {v:?}");
    }
    let name = format!("MeowBoxAgent-Setup-{v}.exe");
    let base = format!("https://github.com/{}/releases/download/v{v}/", crate::REPO);
    let sums = crate::tools::get_bytes(&format!("{base}SHA2-256SUMS"), Duration::from_secs(20))
        .await
        .map_err(|_| anyhow::anyhow!("this release has no SHA2-256SUMS"))?;
    let want = crate::tools::checksum_for(&String::from_utf8_lossy(&sums), &name)
        .ok_or_else(|| anyhow::anyhow!("no checksum for {name}"))?;
    let dst = std::env::temp_dir().join(&name);
    let r = crate::tools::download_checked(&format!("{base}{name}"), &dst, &want, |d, t| METER.set(d, t)).await;
    METER.clear();
    r?;
    Ok(dst)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_a_release_with_the_installer_is_a_new_agent() {
        let rel = |tag: &str, files: &[&str]| {
            let assets: Vec<_> = files.iter().map(|f| serde_json::json!({ "name": f })).collect();
            serde_json::json!({ "tag_name": tag, "assets": assets })
        };
        let with = rel("v0.3.2", &["meowbox-0.3.2.bin", "MeowBoxAgent-Setup-0.3.2.exe", "SHA2-256SUMS"]);
        assert_eq!(agent_release(&with), Some("0.3.2".into()));
        assert_eq!(agent_release(&rel("v0.3.2", &["meowbox-0.3.2.bin", "SHA2-256SUMS"])), None, "firmware only");
        assert_eq!(agent_release(&rel("v0.3.2", &["MeowBoxAgent-Setup-0.3.1.exe"])), None, "another version's");
        assert_eq!(agent_release(&serde_json::json!({ "tag_name": "v0.3.2" })), None);
        // newest first: a firmware-only 0.3.3 and a draft 0.3.4 are skipped, 0.3.2 is the newest agent
        let mut draft = rel("v0.3.4", &["MeowBoxAgent-Setup-0.3.4.exe"]);
        draft["draft"] = serde_json::json!(true);
        let list = serde_json::json!([draft, rel("v0.3.3", &["meowbox-0.3.3.bin"]), with]);
        assert_eq!(newest_agent(&list), Some("0.3.2".into()));
        assert_eq!(newest_agent(&serde_json::json!({ "message": "rate limited" })), None);
    }

    #[test]
    fn versions() {
        assert!(newer_than("0.3.1", "0.3.0"));
        assert!(newer_than("v1.0", "0.9.9"));
        assert!(newer_than("0.10.0", "0.9.0"));
        assert!(!newer_than("0.3.0", "0.3.0"));
        assert!(!newer_than("0.2.5", "0.3.0"));
        assert!(!newer_than("", "0.3.0"));
        assert!(newer_than("0.3.1-rc1", "0.3.0"));
        assert!(!newer_than("0.3.0-rc1", "0.3.0"), "a prerelease of the running version is not newer");
        assert!(!newer_than("0.3.0-rc2", "0.3.0-rc1"));
        for v in ["0.3.0", "1.0", "0.3.0.1"] {
            assert!(valid_version(v), "{v}");
        }
        for v in ["", "0", "0.3.0-rc1", "0.3.0/../../x", "..", "0..3", "0.3.0 ", "v0.3.0", "1.2.3.4.5"] {
            assert!(!valid_version(v), "{v:?}");
        }
    }
}
