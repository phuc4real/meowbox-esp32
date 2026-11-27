//! ~/.claude/settings.json: add, refresh and remove MeowBox's hook and status-line commands. Other hooks and
//! settings stay as they are; the first version of the file is kept as settings.json.meowbox.bak. The commands are
//! byte-identical to the Python agent's, so an install by either build is "current" for the other.

use super::EVENTS;
use serde_json::{Map, Value, json};
use std::path::{Path, PathBuf};

pub const HOOK_HEADER: &str = "X-MeowBox-Hook";

pub fn hook_key_path() -> PathBuf {
    crate::app_dir().join("hook.key")
}

fn random_hex16() -> String {
    use windows::Win32::Security::Cryptography::{BCRYPT_USE_SYSTEM_PREFERRED_RNG, BCryptGenRandom};
    let mut b = [0u8; 16];
    // SAFETY: fills the buffer from the system RNG.
    let ok = unsafe { BCryptGenRandom(None, &mut b, BCRYPT_USE_SYSTEM_PREFERRED_RNG) }.is_ok();
    assert!(ok, "system random number generator failed");
    hex::encode(b)
}

/// Per-install secret that the installed hook and status line commands send in X-MeowBox-Hook, so only they (not a
/// web page, nor another program or user on this PC) can feed the cat screen. Made on first use.
pub fn hook_key_at(path: &Path) -> String {
    let k = std::fs::read_to_string(path).map(|s| s.trim().to_owned()).unwrap_or_default();
    if k.len() == 32 && k.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b)) {
        return k;
    }
    let k = random_hex16();
    if let Some(d) = path.parent() {
        let _ = std::fs::create_dir_all(d);
    }
    if let Err(e) = std::fs::write(path, &k) {
        tracing::warn!("cannot save {}: {e}", path.display());
    }
    k
}

pub fn hook_key() -> String {
    hook_key_at(&hook_key_path())
}

/// What the commands need: the key and the port.
#[derive(Clone)]
pub struct Install {
    pub key: String,
    pub port: u16,
}

impl Install {
    pub fn new(port: u16) -> Self {
        Self { key: hook_key(), port }
    }

    fn curl_headers(&self) -> String {
        format!("-H \"{HOOK_HEADER}: {}\" -H \"Content-Type: application/json\"", self.key)
    }

    pub fn hook_url(&self) -> String {
        format!("http://127.0.0.1:{}/hook", self.port)
    }

    /// Run in the background by Claude Code with the event on stdin. `|| exit 0`: no error when the agent is off.
    pub fn hook_command(&self) -> String {
        format!("curl -s -m 3 -o /dev/null {} --data-binary @- {} || exit 0", self.curl_headers(), self.hook_url())
    }

    /// Claude Code runs this with its status JSON on stdin and shows what it prints. Agent not running: an empty line.
    pub fn status_command(&self) -> String {
        format!("curl -s -m 1 {} --data-binary @- http://127.0.0.1:{}/status", self.curl_headers(), self.port)
    }
}

fn s(v: Option<&Value>) -> String {
    match v {
        Some(Value::String(s)) => s.clone(),
        Some(Value::Null) | None => String::new(),
        Some(o) => o.to_string(),
    }
}

/// Ours: the command hook above, or the HTTP hook of the first Meow Coding builds (replaced on install).
fn is_ours(h: &Value) -> bool {
    let target =
        if h.get("type").and_then(Value::as_str) == Some("http") { s(h.get("url")) } else { s(h.get("command")) };
    target.contains("http://127.0.0.1:") && target.contains("/hook")
}

fn is_our_status(sl: Option<&Value>) -> bool {
    let Some(sl) = sl.filter(|v| v.is_object()) else { return false };
    let cmd = s(sl.get("command"));
    cmd.contains("127.0.0.1:") && cmd.trim_end().ends_with("/status")
}

/// The file as an object; a missing file is empty. A file that is not JSON (or not an object) is an error, so it is
/// never overwritten by mistake.
fn read_settings(path: &Path) -> anyhow::Result<Map<String, Value>> {
    match std::fs::read(path) {
        Ok(b) => match serde_json::from_slice::<Value>(&b)? {
            Value::Object(m) => Ok(m),
            _ => anyhow::bail!("{} is not a JSON object", path.display()),
        },
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(Map::new()),
        Err(e) => Err(e.into()),
    }
}

fn all_hooks(cfg: &Map<String, Value>) -> Vec<&Value> {
    cfg.get("hooks")
        .and_then(Value::as_object)
        .into_iter()
        .flat_map(|h| h.values())
        .filter_map(Value::as_array)
        .flatten()
        .filter(|g| g.is_object())
        .filter_map(|g| g.get("hooks").and_then(Value::as_array))
        .flatten()
        .filter(|h| h.is_object())
        .collect()
}

pub fn hooks_installed_at(path: &Path) -> bool {
    read_settings(path).map(|c| all_hooks(&c).into_iter().any(is_ours)).unwrap_or(false)
}

pub fn hooks_installed() -> bool {
    hooks_installed_at(&super::claude_settings())
}

/// Python's json.dumps(indent=2, ensure_ascii=False) + "\n"; atomic, with a one-time backup.
fn write_settings(path: &Path, cfg: Map<String, Value>) -> anyhow::Result<()> {
    if let Some(d) = path.parent() {
        std::fs::create_dir_all(d)?;
    }
    if path.exists() {
        let mut bak = path.as_os_str().to_owned();
        bak.push(".meowbox.bak");
        let bak = PathBuf::from(bak);
        if !bak.exists() {
            // keep the oldest copy: the file as it was before MeowBox touched it
            std::fs::copy(path, &bak)?;
        }
    }
    let tmp = path.with_extension("tmp");
    let mut text = serde_json::to_string_pretty(&Value::Object(cfg))?;
    text.push('\n');
    std::fs::write(&tmp, text)?;
    std::fs::rename(&tmp, path)?;
    Ok(())
}

/// Remove our hooks; groups left empty go, events left empty go.
fn strip(hooks: &Map<String, Value>) -> Map<String, Value> {
    let mut out = Map::new();
    for (ev, groups) in hooks {
        let Some(groups) = groups.as_array() else {
            out.insert(ev.clone(), groups.clone());
            continue;
        };
        let mut kept = Vec::new();
        for g in groups {
            if let Some(hs) = g.get("hooks").and_then(Value::as_array).filter(|_| g.is_object()) {
                let rest: Vec<Value> = hs.iter().filter(|h| !(h.is_object() && is_ours(h))).cloned().collect();
                if rest.is_empty() {
                    continue;
                }
                let mut g2 = g.clone();
                g2["hooks"] = Value::Array(rest);
                kept.push(g2);
            } else {
                kept.push(g.clone());
            }
        }
        if !kept.is_empty() {
            out.insert(ev.clone(), Value::Array(kept));
        }
    }
    out
}

/// Add one background command hook per event (replacing earlier MeowBox ones, HTTP ones included) and our status
/// line unless the user has their own.
pub fn install_at(path: &Path, inst: &Install) -> anyhow::Result<()> {
    let mut cfg = read_settings(path)?;
    let mut hooks = strip(cfg.get("hooks").and_then(Value::as_object).unwrap_or(&Map::new()));
    for ev in EVENTS {
        let entry =
            json!({"hooks": [{"type": "command", "command": inst.hook_command(), "async": true, "timeout": 5}]});
        match hooks.get_mut(*ev).and_then(Value::as_array_mut) {
            Some(a) => a.push(entry),
            None => {
                hooks.insert((*ev).to_owned(), json!([entry]));
            }
        }
    }
    cfg.insert("hooks".into(), Value::Object(hooks));
    let sl = cfg.get("statusLine");
    let none = sl.is_none_or(|v| v.is_null() || v.as_object().is_some_and(|o| o.is_empty()) || v == &json!(""));
    if none || is_our_status(sl) {
        // never replace someone's own status line
        cfg.insert("statusLine".into(), json!({"type": "command", "command": inst.status_command(), "padding": 0}));
    } else {
        tracing::info!("Claude Code already has a status line: the 5-hour limit will not reach the speaker");
    }
    write_settings(path, cfg)
}

pub fn install(port: u16) -> anyhow::Result<()> {
    install_at(&super::claude_settings(), &Install::new(port))
}

/// Hooks written by an older agent (no key in the command, or another port): write them again, or the server
/// refuses them. Nothing to do when Meow Coding is off. True if the file was rewritten.
pub fn refresh_at(path: &Path, inst: &Install) -> anyhow::Result<bool> {
    let Ok(cfg) = read_settings(path) else { return Ok(false) };
    let ours: Vec<&Value> = all_hooks(&cfg).into_iter().filter(|h| is_ours(h)).collect();
    let sl = cfg.get("statusLine");
    let stale = ours.iter().any(|h| s(h.get("command")) != inst.hook_command())
        || (is_our_status(sl) && s(sl.and_then(|v| v.get("command"))) != inst.status_command());
    if !ours.is_empty() && stale {
        install_at(path, inst)?;
        tracing::info!("Meow Coding: Claude Code hooks updated");
        return Ok(true);
    }
    Ok(false)
}

pub fn refresh(port: u16) -> anyhow::Result<bool> {
    refresh_at(&super::claude_settings(), &Install::new(port))
}

pub fn uninstall_at(path: &Path) -> anyhow::Result<()> {
    let mut cfg = read_settings(path)?;
    if !cfg.contains_key("hooks") && !is_our_status(cfg.get("statusLine")) {
        return Ok(());
    }
    let hooks = strip(cfg.get("hooks").and_then(Value::as_object).unwrap_or(&Map::new()));
    if hooks.is_empty() {
        cfg.remove("hooks");
    } else {
        cfg.insert("hooks".into(), Value::Object(hooks));
    }
    if is_our_status(cfg.get("statusLine")) {
        cfg.remove("statusLine");
    }
    write_settings(path, cfg)
}

pub fn uninstall() -> anyhow::Result<()> {
    uninstall_at(&super::claude_settings())
}
