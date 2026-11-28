//! What the tray shows. Written by the core (its own thread), read by the UI thread.

use tokio::sync::watch;

#[derive(Debug, Clone, Default, PartialEq)]
pub struct AgentState {
    pub connected: bool,
    /// "ip:port" in use or being tried.
    pub host: String,
    /// Speaker name (MeowBox-1A2B).
    pub name: String,
    /// Speaker firmware version.
    pub ver: String,
    /// last | manual: which saved IP answered.
    pub how: String,
    /// IP being tried right now ("", "start", "ip 1.2.3.4").
    pub searching: String,
    pub error: LinkError,
    /// PC holding the speaker when error == Busy.
    pub busy_with: String,
    /// Seconds until the next attempt.
    pub retry_in: u32,
    pub reconnects: u32,
    /// 0 = relay off.
    pub relay_port: u16,
    /// Last relay_test result.
    pub relay_ok: Option<bool>,
    /// Newer agent release found on GitHub.
    pub new_version: String,
    /// The speaker's theme (GET /api/theme after connecting): the agent window wears it. "" = not known yet.
    pub theme: String,
}

/// What a toast from the core is about: the tray shows at most one per kind every 10 minutes, and a new agent
/// version only once (docs/enhance_agent.md, B6).
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub enum ToastKind {
    FfmpegStart,
    FfmpegDone,
    FfmpegFailed,
    NewVersion(String),
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub enum LinkError {
    #[default]
    None,
    Token,
    Port,
    NotFound,
    Busy,
    ProtoAgent,
    ProtoSpeaker,
}

impl AgentState {
    pub fn ip(&self) -> &str {
        self.host.split(':').next().unwrap_or("")
    }
}

/// Shared state: the core changes it with `update`, listeners see the new value (tokio watch channel, so a
/// listener that falls behind only sees the latest state).
#[derive(Clone)]
pub struct SharedState {
    tx: watch::Sender<AgentState>,
}

impl Default for SharedState {
    fn default() -> Self {
        Self { tx: watch::Sender::new(AgentState::default()) }
    }
}

impl SharedState {
    /// Change fields in place; listeners are woken only if something actually changed.
    pub fn update(&self, f: impl FnOnce(&mut AgentState)) {
        self.tx.send_if_modified(|s| {
            let before = s.clone();
            f(s);
            *s != before
        });
    }

    pub fn get(&self) -> AgentState {
        self.tx.borrow().clone()
    }

    pub fn subscribe(&self) -> watch::Receiver<AgentState> {
        self.tx.subscribe()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn update_wakes_only_on_change() {
        let s = SharedState::default();
        let mut rx = s.subscribe();
        rx.mark_unchanged();
        s.update(|st| st.retry_in = 0);
        assert!(!rx.has_changed().unwrap());
        s.update(|st| st.host = "192.168.1.50:80".into());
        assert!(rx.has_changed().unwrap());
        assert_eq!(s.get().ip(), "192.168.1.50");
    }
}
