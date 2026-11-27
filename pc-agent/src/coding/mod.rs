//! Meow Coding: what Claude Code is doing, for the speaker's cat screen.
//!
//! Claude Code sends every hook event to http://127.0.0.1:<coding_port>/hook: command hooks in
//! ~/.claude/settings.json (written by `meowbox coding install`) that run curl in the background (`async`) and always
//! exit 0, so Claude Code never waits for them and shows no "hook failed" when the agent is not running. Background
//! hooks can arrive out of order; `event` keeps a late PostToolUse from undoing "done" or a late PreToolUse from
//! hiding a permission prompt. The tracker keeps one entry per session (state, kind of action, title) and reads the
//! session's transcript for tokens and context use. The core sends a summary to the speaker as
//! {"t":"coding","s":[...]} whenever it changes (firmware/web/API.md, /ws/pc protocol).
//!
//! Claude Code's status line (also written by `meowbox coding install`, only when no other status line is set)
//! POSTs its input to /status: it carries the plan's usage limits (5-hour and weekly windows, in %) and the exact
//! context use. The answer is the status line text. API-key sessions have no plan limits.
//!
//! Only short labels leave the PC: the session title, the project folder, the git branch and the kind of action
//! ("read", "bash", "image"...; the speaker words it). No prompt text, commands, file names or contents.
//!
//! Port of the Python agent's coding.py; the hook and status-line commands are byte-identical so both builds share
//! one installation.

pub mod hooks;
pub mod server;
pub mod transcript;

use serde_json::{Map, Value, json};
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::time::{Duration, Instant};

pub const DEFAULT_PORT: u16 = 8767;
/// Sent to the speaker, most important first.
const MAX_SESSIONS: usize = 4;
/// "working" with no event for 5 min (interrupted with Esc: no Stop hook) → idle.
const WORK_STALE: Duration = Duration::from_secs(300);
/// Finished turn shown as done for 10 min, then idle.
const DONE_STALE: Duration = Duration::from_secs(600);
/// No event for an hour → forgotten.
const DROP_AFTER: Duration = Duration::from_secs(3600);

pub const EVENTS: &[&str] = &[
    "SessionStart",
    "UserPromptSubmit",
    "PreToolUse",
    "PostToolUse",
    "PostToolUseFailure",
    "PermissionRequest",
    "PermissionDenied",
    "Notification",
    "PreCompact",
    "Stop",
    "StopFailure",
    "SessionEnd",
];

/// Models with a 200K context window; the rest have 1M.
const SMALL_WINDOW: &[&str] = &[
    "claude-haiku-4",
    "claude-3",
    "claude-opus-4-5",
    "claude-sonnet-4-5",
    "claude-opus-4-1",
    "claude-opus-4-2",
    "claude-sonnet-4-2",
];

fn home() -> PathBuf {
    std::env::var_os("USERPROFILE").map(PathBuf::from).unwrap_or_default()
}

/// Where Claude Code keeps its transcripts: CLAUDE_CONFIG_DIR, else ~/.claude.
pub fn claude_dir() -> PathBuf {
    std::env::var_os("CLAUDE_CONFIG_DIR").map(PathBuf::from).unwrap_or_else(|| home().join(".claude"))
}

/// The settings file the hooks go into (always ~/.claude/settings.json, as the Python agent).
pub fn claude_settings() -> PathBuf {
    home().join(".claude").join("settings.json")
}

pub fn window(model: &str) -> u64 {
    if SMALL_WINDOW.iter().any(|p| model.starts_with(p)) { 200_000 } else { 1_000_000 }
}

/// claude-opus-5-5 → "Opus 5.5", claude-sonnet-4-5-20250929 → "Sonnet 4.5", claude-opus-4-20250514 → "Opus 4".
/// Same as the Python regex `claude-([a-z]+)-(\d+)(?:-(\d))?(?:\D|$)`.
pub fn model_label(model: &str) -> String {
    let Some(rest) = model.strip_prefix("claude-") else { return String::new() };
    let name_len = rest.bytes().take_while(|b| b.is_ascii_lowercase()).count();
    if name_len == 0 || rest.as_bytes().get(name_len) != Some(&b'-') {
        return String::new();
    }
    let name = &rest[..name_len];
    let after = &rest[name_len + 1..];
    let major_len = after.bytes().take_while(u8::is_ascii_digit).count();
    if major_len == 0 {
        return String::new();
    }
    let major = &after[..major_len];
    let tail = &after.as_bytes()[major_len..];
    let minor = match tail {
        [b'-', d, rest @ ..] if d.is_ascii_digit() && rest.first().is_none_or(|c| !c.is_ascii_digit()) => {
            Some(*d as char)
        }
        _ => None,
    };
    let mut cap = name[..1].to_ascii_uppercase();
    cap.push_str(&name[1..]);
    match minor {
        Some(m) => format!("{cap} {major}.{m}"),
        None => format!("{cap} {major}"),
    }
}

/// Last path component (either slash), as Python's ntpath.basename(p.rstrip("/\\")).
pub fn basename(p: &str) -> &str {
    let p = p.trim_end_matches(['/', '\\']);
    let last = p.rsplit(['/', '\\']).next().unwrap_or("");
    // "C:foo" → "foo", "C:" → ""
    match last.as_bytes() {
        [d, b':', ..] if d.is_ascii_alphabetic() => &last[2..],
        _ => last,
    }
}

/// Whitespace collapsed, cut to n characters with "…".
pub fn short(s: &str, n: usize) -> String {
    let s = s.split_whitespace().collect::<Vec<_>>().join(" ");
    if s.chars().count() <= n { s } else { s.chars().take(n - 1).chain(['…']).collect() }
}

const IMAGE: &[&str] = &[".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp", ".svg", ".ico"];

fn kind(tool: &str) -> Option<&'static str> {
    Some(match tool {
        "Read" => "read",
        "Edit" | "MultiEdit" | "NotebookEdit" => "edit",
        "Write" => "write",
        "Bash" | "PowerShell" | "Monitor" => "bash",
        "Grep" | "Glob" | "LSP" => "search",
        "WebFetch" | "WebSearch" => "web",
        "Agent" | "Task" | "SendMessage" => "agent",
        "TodoWrite" | "EnterPlanMode" | "ExitPlanMode" => "plan",
        "AskUserQuestion" => "ask",
        "Skill" | "ToolSearch" => "skill",
        _ => return None,
    })
}

/// The kind of thing the agent is doing, for the speaker to word ("read" → "Đang đọc file").
pub fn action(tool: &str, inp: &Value) -> &'static str {
    let path = inp.get("file_path").map(|v| match v {
        Value::String(s) => s.to_lowercase(),
        Value::Null => String::new(),
        other => other.to_string().to_lowercase(),
    });
    if tool == "Read" && path.is_some_and(|p| IMAGE.iter().any(|e| p.ends_with(e))) {
        return "image";
    }
    if tool.starts_with("mcp__") {
        return if tool.contains("browser") || tool.contains("chrome") { "web" } else { "tool" };
    }
    kind(tool).unwrap_or("tool")
}

/// A UNC path (\\server\share, //server/share, \\?\UNC\...): opening it would reach another computer.
pub fn is_network_path(p: &str) -> bool {
    p.replace('/', "\\").starts_with("\\\\")
}

/// Claude Code keeps transcripts as .jsonl files under its own folder; anything else is not read.
pub fn transcript_ok(p: &str) -> bool {
    transcript_ok_in(p, &claude_dir())
}

pub fn transcript_ok_in(p: &str, root: &Path) -> bool {
    if p.is_empty() || is_network_path(p) {
        return false;
    }
    let (Ok(rp), Ok(root)) = (std::path::absolute(p), std::path::absolute(root)) else { return false };
    let (rp, root) = (normalize(&rp), normalize(&root));
    // also through symlinks / junctions, when the file exists
    let real = std::fs::canonicalize(&rp).ok();
    let real_root = std::fs::canonicalize(&root).ok();
    let inside = |f: &Path, r: &Path| f.extension().is_some_and(|e| e == "jsonl") && f.starts_with(r);
    match (real, real_root) {
        (Some(f), Some(r)) => inside(&f, &r),
        _ => inside(&rp, &root),
    }
}

/// Resolve "." and ".." without touching the disk.
fn normalize(p: &Path) -> PathBuf {
    let mut out = PathBuf::new();
    for c in p.components() {
        match c {
            std::path::Component::ParentDir => {
                out.pop();
            }
            std::path::Component::CurDir => {}
            other => out.push(other),
        }
    }
    out
}

/// The branch checked out in `cwd` or a parent (up to 8 levels), from .git/HEAD only: no git process, and no file
/// access at all for a network path.
pub fn git_branch(cwd: &str) -> String {
    if cwd.is_empty() || is_network_path(cwd) {
        return String::new();
    }
    let mut p = PathBuf::from(cwd);
    for _ in 0..8 {
        let mut head = p.join(".git");
        if head.is_file() {
            // worktree / submodule: "gitdir: <path>"
            match std::fs::read_to_string(&head)
                .ok()
                .and_then(|t| t.split_once("gitdir:").map(|x| x.1.trim().to_owned()))
            {
                Some(g) => head = PathBuf::from(g),
                None => return String::new(),
            }
        }
        let h = head.join("HEAD");
        if h.is_file() {
            let Ok(r) = std::fs::read_to_string(&h) else { return String::new() };
            let r = r.trim();
            return if r.starts_with("ref:") {
                r.rsplit('/').next().unwrap_or("").to_owned()
            } else {
                r.chars().take(7).collect()
            };
        }
        match p.parent() {
            Some(parent) if parent != p => p = parent.to_owned(),
            _ => return String::new(),
        }
    }
    String::new()
}

/// resets_at: epoch seconds (or ms) or an ISO 8601 string → epoch seconds (0 if unknown).
pub fn epoch(v: &Value) -> i64 {
    match v {
        Value::Number(n) => {
            let f = n.as_f64().unwrap_or(0.0);
            if f > 1e12 { (f / 1000.0) as i64 } else { f as i64 }
        }
        Value::String(s) if !s.is_empty() => iso_epoch(s).unwrap_or(0),
        _ => 0,
    }
}

/// "2026-11-27T13:00:00Z", "2026-11-27T13:00:00.123+07:00", "2026-11-27 13:00:00" (naive = local time, as Python's
/// datetime.timestamp()).
fn iso_epoch(s: &str) -> Option<i64> {
    let s = s.trim();
    let b = s.as_bytes();
    if b.len() < 19 || b[4] != b'-' || b[7] != b'-' || !(b[10] == b'T' || b[10] == b' ') {
        return None;
    }
    let num = |a: usize, n: usize| -> Option<i64> { s.get(a..a + n)?.parse().ok() };
    let (y, mo, d, h, mi, se) = (num(0, 4)?, num(5, 2)?, num(8, 2)?, num(11, 2)?, num(14, 2)?, num(17, 2)?);
    let mut rest = &s[19..];
    if let Some(r) = rest.strip_prefix('.') {
        rest = r.trim_start_matches(|c: char| c.is_ascii_digit());
    }
    let offset = match rest {
        "" => -local_offset_secs(),
        "Z" | "z" => 0,
        tz => {
            let sign = match tz.as_bytes().first()? {
                b'+' => 1,
                b'-' => -1,
                _ => return None,
            };
            let t = tz[1..].replace(':', "");
            let (oh, om): (i64, i64) = (t.get(0..2)?.parse().ok()?, t.get(2..4).unwrap_or("00").parse().ok()?);
            sign * (oh * 3600 + om * 60)
        }
    };
    // days from civil (Howard Hinnant)
    let (y2, m2) = if mo <= 2 { (y - 1, mo + 9) } else { (y, mo - 3) };
    let era = y2.div_euclid(400);
    let yoe = y2 - era * 400;
    let doy = (153 * m2 + 2) / 5 + d - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    let days = era * 146_097 + doe - 719_468;
    Some(days * 86_400 + h * 3600 + mi * 60 + se - offset)
}

fn local_offset_secs() -> i64 {
    use windows::Win32::System::Time::{GetTimeZoneInformation, TIME_ZONE_INFORMATION};
    let mut tz = TIME_ZONE_INFORMATION::default();
    // SAFETY: fills the struct; 2 = daylight time in effect.
    let r = unsafe { GetTimeZoneInformation(&mut tz) };
    let bias = tz.Bias + if r == 2 { tz.DaylightBias } else { tz.StandardBias };
    -(bias as i64) * 60
}

#[derive(Debug, Clone)]
pub struct Session {
    pub id: String,
    pub cwd: String,
    pub branch: String,
    pub title: String,
    pub state: &'static str,
    pub act: &'static str,
    /// When the state changed.
    pub since: Instant,
    /// Last Stop: tool events right after it are late ones.
    pub stop_at: Option<Instant>,
    /// Last hook event.
    pub last: Instant,
    pub transcript: String,
    pub usage: transcript::Usage,
    pub read_at: Option<Instant>,
    /// From the status line (exact); else estimated from the transcript.
    pub ctx_pct: Option<f64>,
}

impl Session {
    fn new(id: &str) -> Self {
        let now = Instant::now();
        Self {
            id: id.to_owned(),
            cwd: String::new(),
            branch: String::new(),
            title: String::new(),
            state: "idle",
            act: "",
            since: now,
            stop_at: None,
            last: now,
            transcript: String::new(),
            usage: Default::default(),
            read_at: None,
            ctx_pct: None,
        }
    }

    fn set_state(&mut self, st: &'static str) {
        if st != self.state {
            self.state = st;
            self.since = Instant::now();
        }
    }
}

fn state_order(s: &str) -> u8 {
    match s {
        "wait" => 0,
        "work" => 1,
        "err" => 2,
        "done" => 3,
        "idle" => 4,
        _ => 9,
    }
}

#[derive(Default)]
struct Inner {
    sessions: HashMap<String, Session>,
    /// {"h5": %, "h5r": reset epoch, "d7": %, "d7r": ...} from the status line.
    limits: Map<String, Value>,
}

#[derive(Default)]
pub struct Tracker {
    inner: Mutex<Inner>,
    /// Transcripts are checked against this folder (tests point it elsewhere).
    claude_dir: Option<PathBuf>,
}

fn str_of(v: Option<&Value>) -> String {
    match v {
        Some(Value::String(s)) => s.clone(),
        Some(Value::Null) | None => String::new(),
        Some(other) => other.to_string(),
    }
}

impl Tracker {
    pub fn with_claude_dir(dir: PathBuf) -> Self {
        Self { inner: Mutex::default(), claude_dir: Some(dir) }
    }

    fn lock(&self) -> std::sync::MutexGuard<'_, Inner> {
        self.inner.lock().unwrap_or_else(|e| e.into_inner())
    }

    fn transcript_ok(&self, p: &str) -> bool {
        match &self.claude_dir {
            Some(d) => transcript_ok_in(p, d),
            None => transcript_ok(p),
        }
    }

    pub fn session_ids(&self) -> Vec<String> {
        self.lock().sessions.keys().cloned().collect()
    }

    /// A hook event from Claude Code.
    pub fn event(&self, ev: &Value) {
        let sid = str_of(ev.get("session_id"));
        let name = str_of(ev.get("hook_event_name"));
        if sid.is_empty() || name.is_empty() {
            return;
        }
        let cwd = str_of(ev.get("cwd"));
        let tp = str_of(ev.get("transcript_path"));
        let tp_ok = !tp.is_empty() && self.transcript_ok(&tp);
        let mut g = self.lock();
        if name == "SessionEnd" {
            g.sessions.remove(&sid);
            return;
        }
        let s = g.sessions.entry(sid.clone()).or_insert_with(|| Session::new(&sid));
        let now = Instant::now();
        s.last = now;
        if !cwd.is_empty() && (s.cwd.is_empty() || name == "SessionStart") {
            // the project: Bash `cd` moves the session's cwd later
            s.branch = git_branch(&cwd);
            s.cwd = cwd;
        }
        if tp_ok {
            s.transcript = tp;
        }
        let tool = str_of(ev.get("tool_name"));
        let empty = json!({});
        let input = ev.get("tool_input").filter(|v| v.is_object()).unwrap_or(&empty);
        // a tool event that lost the race against Stop
        let late = s.stop_at.is_some_and(|t| now.duration_since(t) < Duration::from_secs(3));
        match name.as_str() {
            "SessionStart" => {
                s.set_state("idle");
                s.act = "";
                s.branch = git_branch(&s.cwd);
            }
            "UserPromptSubmit" => {
                s.stop_at = None;
                s.set_state("work");
                s.act = "";
                s.branch = git_branch(&s.cwd); // a checkout since the last prompt
            }
            "PreToolUse" => {
                if late {
                    return;
                }
                // AskUserQuestion waits for an answer; everything else is work, except right after a permission
                // prompt for this call (its PermissionRequest overtook this PreToolUse)
                if tool == "AskUserQuestion" {
                    s.set_state("wait");
                } else if !(s.state == "wait" && now.duration_since(s.since) < Duration::from_secs(2)) {
                    s.set_state("work");
                }
                s.act = action(&tool, input);
            }
            "PostToolUse" | "PostToolUseFailure" | "PermissionDenied" => {
                if late || s.state == "done" {
                    return;
                }
                s.set_state("work");
            }
            "PermissionRequest" => {
                s.set_state("wait");
                if !tool.is_empty() {
                    s.act = action(&tool, input);
                }
            }
            "Notification" => {
                let kind = str_of(ev.get("notification_type"));
                let msg = str_of(ev.get("message")).to_lowercase();
                if kind == "permission_prompt" || (kind.is_empty() && msg.contains("permission")) {
                    s.set_state("wait");
                }
            }
            "PreCompact" => {
                s.set_state("work");
                s.act = "compact";
            }
            "Stop" => {
                s.stop_at = Some(now);
                s.set_state("done");
                s.act = "";
            }
            "StopFailure" => {
                s.set_state("err");
                s.act = "";
            }
            _ => {}
        }
    }

    /// Status line input: plan usage limits, exact context use. Returns the status line text.
    pub fn status(&self, ev: &Value) -> String {
        let mut lim = Map::new();
        if let Some(rl) = ev.get("rate_limits").filter(|v| v.is_object()) {
            for (key, short_key) in [("five_hour", "h5"), ("seven_day", "d7")] {
                let Some(w) = rl.get(key).filter(|v| v.is_object()) else { continue };
                let pct = w.get("used_percentage").or_else(|| w.get("utilization"));
                if let Some(p) = pct.filter(|p| p.is_number()).and_then(Value::as_f64) {
                    lim.insert(short_key.into(), json!(crate::json::round(p)));
                    let r = epoch(w.get("resets_at").unwrap_or(&Value::Null));
                    if r != 0 {
                        lim.insert(format!("{short_key}r"), json!(r));
                    }
                }
            }
        }
        let sid = str_of(ev.get("session_id"));
        let used = ev.get("context_window").and_then(|c| c.get("used_percentage")).filter(|p| p.is_number());
        let mut g = self.lock();
        if !lim.is_empty() {
            g.limits = lim;
        }
        if let (Some(s), Some(u)) = (g.sessions.get_mut(&sid), used.and_then(Value::as_f64)) {
            s.ctx_pct = Some(u);
        }
        let mut parts = Vec::new();
        if let Some(h5) = g.limits.get("h5") {
            parts.push(format!("5h {h5}%"));
        }
        if let Some(d7) = g.limits.get("d7") {
            parts.push(format!("7d {d7}%"));
        }
        let h5 = g.limits.get("h5").and_then(Value::as_i64).unwrap_or(0);
        // the cat gets nervous near the limit
        let face = if h5 < 70 {
            "=^.^="
        } else if h5 < 90 {
            "=o.o="
        } else {
            "=x.x="
        };
        let mut out = format!("{face} MeowBox");
        if !parts.is_empty() {
            out.push_str(" · ");
            out.push_str(&parts.join(" · "));
        }
        out
    }

    pub fn limits_msg(&self) -> Map<String, Value> {
        self.lock().limits.clone()
    }

    /// Read new transcript lines (tokens, context, title): every 3 s for a busy session, 15 s for a quiet one.
    /// The files are read without holding the lock, so hook events are never held up by a large transcript.
    pub fn refresh_usage(&self) {
        self.refresh(false);
    }

    pub fn refresh(&self, force: bool) {
        let now = Instant::now();
        let todo: Vec<(String, String, transcript::Usage)> = {
            let mut g = self.lock();
            g.sessions
                .values_mut()
                .filter(|s| !s.transcript.is_empty())
                .filter(|s| {
                    let every = if matches!(s.state, "work" | "wait") { 3 } else { 15 };
                    force || s.read_at.is_none_or(|t| now.duration_since(t) >= Duration::from_secs(every))
                })
                .map(|s| {
                    s.read_at = Some(now);
                    (s.id.clone(), s.transcript.clone(), std::mem::take(&mut s.usage))
                })
                .collect()
        };
        for (sid, path, mut usage) in todo {
            if let Err(e) = transcript::read(Path::new(&path), &mut usage) {
                tracing::debug!("transcript {path}: {e}");
            }
            let mut g = self.lock();
            if let Some(s) = g.sessions.get_mut(&sid) {
                if !usage.title.is_empty() {
                    s.title = usage.title.clone();
                }
                s.usage = usage;
            }
        }
    }

    /// What goes to the speaker: at most 4 sessions, waiting first.
    pub fn snapshot(&self) -> Vec<Value> {
        let now = Instant::now();
        let mut g = self.lock();
        g.sessions.retain(|_, s| now.duration_since(s.last) <= DROP_AFTER);
        for s in g.sessions.values_mut() {
            let quiet = now.duration_since(s.last);
            if (s.state == "work" && quiet > WORK_STALE) || (matches!(s.state, "done" | "err") && quiet > DONE_STALE) {
                s.set_state("idle");
                s.act = "";
            }
        }
        let mut ss: Vec<&Session> = g.sessions.values().collect();
        ss.sort_by(|a, b| state_order(a.state).cmp(&state_order(b.state)).then(b.last.cmp(&a.last)));
        ss.into_iter()
            .take(MAX_SESSIONS)
            .map(|s| {
                let mut d = Map::new();
                d.insert("id".into(), json!(s.id.chars().take(8).collect::<String>()));
                d.insert("ag".into(), json!("claude"));
                d.insert("name".into(), json!(short(&s.title, 48)));
                d.insert("dir".into(), json!(short(basename(&s.cwd), 32)));
                d.insert("br".into(), json!(short(&s.branch, 32)));
                d.insert("st".into(), json!(s.state));
                d.insert("act".into(), json!(s.act));
                d.insert("for".into(), json!(now.duration_since(s.since).as_secs()));
                d.insert("tok".into(), json!(s.usage.tokens));
                d.insert("model".into(), json!(model_label(&s.usage.model)));
                if let Some(p) = s.ctx_pct {
                    d.insert("ctx".into(), json!(crate::json::round(p).min(100)));
                } else if !s.usage.model.is_empty() {
                    let pct = 100.0 * s.usage.ctx as f64 / window(&s.usage.model) as f64;
                    d.insert("ctx".into(), json!(crate::json::round(pct).min(100)));
                }
                Value::Object(d)
            })
            .collect()
    }
}

#[cfg(test)]
mod tests;
