//! agent.json. Read as a JSON object so keys this build does not know (or written by another version) survive a
//! save, and a value of the wrong type only falls back to its default instead of losing the whole file.

use serde_json::{Map, Value, json};
use std::path::{Path, PathBuf};

#[derive(Debug, Clone, PartialEq)]
pub struct Config {
    /// Name of the speaker to use (MeowBox-1A2B); "" = whichever answers at the saved IP.
    pub speaker: String,
    /// IP[:port] typed by hand.
    pub host: String,
    /// IP[:port] of the last successful connection.
    pub last_host: String,
    /// Notify token (Notifications page on the speaker).
    pub token: String,
    pub relay_port: u16,
    /// Meow Coding: Claude Code hooks POST here (127.0.0.1 only).
    pub coding_port: u16,
    /// Tray: pause sending stats.
    pub paused: bool,
    /// Check GitHub daily for a newer agent and yt-dlp (Settings window).
    pub auto_update: bool,
    /// Everything as read from the file, known keys included; known keys are overwritten on save.
    extra: Map<String, Value>,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            speaker: String::new(),
            host: String::new(),
            last_host: String::new(),
            token: String::new(),
            relay_port: 8765,
            coding_port: 8767,
            paused: false,
            auto_update: true,
            extra: Map::new(),
        }
    }
}

fn get_str(m: &Map<String, Value>, k: &str, d: &str) -> String {
    m.get(k).and_then(Value::as_str).unwrap_or(d).to_owned()
}

fn get_port(m: &Map<String, Value>, k: &str, d: u16) -> u16 {
    // Python wrote ints; `meowbox config --relay-port` could only write an int, but a hand edit may give a string.
    match m.get(k) {
        Some(Value::Number(n)) => n.as_u64().and_then(|v| u16::try_from(v).ok()).filter(|&v| v > 0).unwrap_or(d),
        Some(Value::String(s)) => s.trim().parse().ok().filter(|&v: &u16| v > 0).unwrap_or(d),
        _ => d,
    }
}

fn get_bool(m: &Map<String, Value>, k: &str, d: bool) -> bool {
    m.get(k).and_then(Value::as_bool).unwrap_or(d)
}

impl Config {
    pub fn from_map(m: Map<String, Value>) -> Self {
        let d = Self::default();
        Self {
            speaker: get_str(&m, "speaker", &d.speaker),
            host: get_str(&m, "host", &d.host),
            last_host: get_str(&m, "last_host", &d.last_host),
            token: get_str(&m, "token", &d.token),
            relay_port: get_port(&m, "relay_port", d.relay_port),
            coding_port: get_port(&m, "coding_port", d.coding_port),
            paused: get_bool(&m, "paused", d.paused),
            auto_update: get_bool(&m, "auto_update", d.auto_update),
            extra: m,
        }
    }

    pub fn to_map(&self) -> Map<String, Value> {
        let mut m = self.extra.clone();
        for (k, v) in [
            ("speaker", json!(self.speaker)),
            ("host", json!(self.host)),
            ("last_host", json!(self.last_host)),
            ("token", json!(self.token)),
            ("relay_port", json!(self.relay_port)),
            ("coding_port", json!(self.coding_port)),
            ("paused", json!(self.paused)),
            ("auto_update", json!(self.auto_update)),
        ] {
            m.insert(k.to_owned(), v);
        }
        m
    }

    /// Missing or unreadable file: defaults (like the Python agent).
    pub fn load(path: &Path) -> Self {
        let m = std::fs::read(path)
            .ok()
            .and_then(|b| serde_json::from_slice::<Value>(&b).ok())
            .and_then(|v| match v {
                Value::Object(m) => Some(m),
                _ => None,
            })
            .unwrap_or_default();
        Self::from_map(m)
    }

    /// Atomic: write agent.tmp, then rename over agent.json.
    pub fn save(&self, path: &Path) -> std::io::Result<()> {
        if let Some(dir) = path.parent() {
            std::fs::create_dir_all(dir)?;
        }
        let tmp: PathBuf = path.with_extension("tmp");
        let mut text = serde_json::to_string_pretty(&Value::Object(self.to_map()))?;
        text.push('\n');
        std::fs::write(&tmp, text)?;
        std::fs::rename(&tmp, path)
    }

    /// The speaker address to show or open: in use, else last working, else typed.
    pub fn known_host(&self) -> &str {
        if !self.last_host.is_empty() { &self.last_host } else { &self.host }
    }
}

/// The config shared by the core thread and the UI: changes are saved to `path` right away, unless it is an
/// in-memory one (`meowbox --host … run`: a one-off address must not replace the saved speaker).
#[derive(Clone)]
pub struct SharedConfig {
    inner: std::sync::Arc<std::sync::Mutex<Config>>,
    path: Option<PathBuf>,
}

impl SharedConfig {
    pub fn new(cfg: Config, path: PathBuf) -> Self {
        Self { inner: std::sync::Arc::new(std::sync::Mutex::new(cfg)), path: Some(path) }
    }

    /// Changes stay in memory and are never written anywhere.
    pub fn in_memory(cfg: Config) -> Self {
        Self { inner: std::sync::Arc::new(std::sync::Mutex::new(cfg)), path: None }
    }

    pub fn load(path: PathBuf) -> Self {
        Self::new(Config::load(&path), path)
    }

    pub fn get(&self) -> Config {
        self.inner.lock().unwrap_or_else(|e| e.into_inner()).clone()
    }

    /// Change and save (only if something changed). A failed save is logged; the change stays in memory.
    pub fn update(&self, f: impl FnOnce(&mut Config)) {
        let mut c = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        let before = c.clone();
        f(&mut c);
        if *c != before
            && let Some(path) = &self.path
            && let Err(e) = c.save(path)
        {
            tracing::warn!("could not save {}: {e}", path.display());
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn missing_file_gives_defaults() {
        let c = Config::load(Path::new("Z:\\does\\not\\exist.json"));
        assert_eq!(c, Config::default());
    }

    #[test]
    fn unknown_keys_survive_and_bad_types_fall_back() {
        let dir = std::env::temp_dir().join(format!("meowbox-cfg-{}", std::process::id()));
        let p = dir.join("agent.json");
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(&p, r#"{"token":"abc","relay_port":"9000","paused":"yes","future_key":[1,2]}"#).unwrap();
        let mut c = Config::load(&p);
        assert_eq!((c.token.as_str(), c.relay_port, c.paused), ("abc", 9000, false));
        c.speaker = "MeowBox-TEST".into();
        c.save(&p).unwrap();
        let v: Value = serde_json::from_slice(&std::fs::read(&p).unwrap()).unwrap();
        assert_eq!(v["future_key"], json!([1, 2]));
        assert_eq!(v["relay_port"], json!(9000));
        assert_eq!(v["speaker"], json!("MeowBox-TEST"));
        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn in_memory_config_is_never_saved() {
        let dir = std::env::temp_dir().join(format!("meowbox-mem-{}", std::process::id()));
        let p = dir.join("agent.json");
        let shared = SharedConfig::in_memory(Config::default());
        shared.update(|c| c.last_host = "1.2.3.4".into());
        assert_eq!(shared.get().last_host, "1.2.3.4");
        assert!(!p.exists() && !dir.exists());
    }

    #[test]
    fn port_zero_or_out_of_range_is_default() {
        let m: Map<String, Value> = serde_json::from_str(r#"{"relay_port":0,"coding_port":70000}"#).unwrap();
        let c = Config::from_map(m);
        assert_eq!((c.relay_port, c.coding_port), (8765, 8767));
    }
}
