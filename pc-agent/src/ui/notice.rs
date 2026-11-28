//! When the tray shows a toast (docs/enhance_agent.md, B6). Pure logic on (state, time), so it is tested without
//! a tray: `Watch` turns link state changes into events, `Throttle` lets at most one per kind through every 10
//! minutes. Toasts that answer the user's own action (Meow Coding, test notification) bypass both.

use crate::state::{AgentState, LinkError, ToastKind};
use std::collections::HashMap;
use std::time::{Duration, Instant};

/// How long the speaker must be gone, after having been connected, before a toast says so.
pub const LOST_AFTER: Duration = Duration::from_secs(120);
/// At most one toast per kind in this window.
pub const QUIET: Duration = Duration::from_secs(600);

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub enum Kind {
    /// Speaker gone for LOST_AFTER after having been connected.
    Lost,
    /// The speaker refused the token.
    Token,
    /// Another PC took the speaker while we had it.
    TakenOver,
    Core(ToastKind),
}

/// Link state → toast events. Each episode toasts once: the flags reset when the link is up again.
#[derive(Debug, Default)]
pub struct Watch {
    /// Connected at least once since the agent started (the status line says "lost" instead of "not found").
    pub ever_connected: bool,
    was_connected: bool,
    lost_since: Option<Instant>,
    lost_told: bool,
    token_told: bool,
    busy_told: bool,
}

impl Watch {
    pub fn update(&mut self, st: &AgentState, now: Instant) -> Option<Kind> {
        if st.connected {
            *self = Self { ever_connected: true, was_connected: true, ..Self::default() };
            return None;
        }
        if self.was_connected {
            self.was_connected = false;
            self.lost_since = Some(now);
        }
        match st.error {
            LinkError::Token if !self.token_told => {
                self.token_told = true;
                Some(Kind::Token)
            }
            // busy at start (another PC had it first) is only in the status line; taken from us is a toast
            LinkError::Busy if self.ever_connected && !self.busy_told => {
                self.busy_told = true;
                Some(Kind::TakenOver)
            }
            LinkError::None | LinkError::NotFound
                if !self.lost_told && self.lost_since.is_some_and(|t| now.duration_since(t) >= LOST_AFTER) =>
            {
                self.lost_told = true;
                Some(Kind::Lost)
            }
            _ => None,
        }
    }
}

#[derive(Debug, Default)]
pub struct Throttle {
    last: HashMap<Kind, Instant>,
}

impl Throttle {
    /// True (and remembered) when no toast of this kind was let through in the last QUIET.
    pub fn allow(&mut self, kind: &Kind, now: Instant) -> bool {
        if self.last.get(kind).is_some_and(|t| now.duration_since(*t) < QUIET) {
            return false;
        }
        self.last.insert(kind.clone(), now);
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn st(connected: bool, error: LinkError) -> AgentState {
        AgentState { connected, error, ..Default::default() }
    }

    #[test]
    fn first_connection_and_short_drops_are_quiet() {
        let (mut w, t0) = (Watch::default(), Instant::now());
        assert_eq!(w.update(&st(false, LinkError::None), t0), None, "searching at start");
        assert_eq!(w.update(&st(false, LinkError::NotFound), t0), None, "not found before ever connecting");
        assert_eq!(w.update(&st(true, LinkError::None), t0), None, "connected for the first time");
        assert_eq!(w.update(&st(false, LinkError::NotFound), t0), None);
        assert_eq!(w.update(&st(false, LinkError::NotFound), t0 + Duration::from_secs(119)), None);
        assert_eq!(w.update(&st(true, LinkError::None), t0 + Duration::from_secs(119)), None, "back in time");
    }

    #[test]
    fn lost_for_two_minutes_toasts_once() {
        let (mut w, t0) = (Watch::default(), Instant::now());
        w.update(&st(true, LinkError::None), t0);
        w.update(&st(false, LinkError::None), t0);
        let at = |s| t0 + Duration::from_secs(s);
        assert_eq!(w.update(&st(false, LinkError::NotFound), at(120)), Some(Kind::Lost));
        assert_eq!(w.update(&st(false, LinkError::NotFound), at(121)), None, "once per episode");
        assert_eq!(w.update(&st(false, LinkError::NotFound), at(900)), None);
        w.update(&st(true, LinkError::None), at(901));
        w.update(&st(false, LinkError::NotFound), at(902));
        assert_eq!(w.update(&st(false, LinkError::NotFound), at(1022)), Some(Kind::Lost), "next episode");
    }

    #[test]
    fn token_and_take_over_toast_once_per_episode() {
        let (mut w, t0) = (Watch::default(), Instant::now());
        assert_eq!(w.update(&st(false, LinkError::Busy), t0), None, "busy before we ever had it");
        assert_eq!(w.update(&st(false, LinkError::Token), t0), Some(Kind::Token));
        assert_eq!(w.update(&st(false, LinkError::Token), t0), None, "retries keep the same error");
        w.update(&st(true, LinkError::None), t0);
        assert_eq!(w.update(&st(false, LinkError::Busy), t0), Some(Kind::TakenOver));
        assert_eq!(w.update(&st(false, LinkError::Busy), t0 + LOST_AFTER * 2), None, "no 'lost' on top of it");
    }

    #[test]
    fn one_toast_per_kind_every_ten_minutes() {
        let (mut th, t0) = (Throttle::default(), Instant::now());
        assert!(th.allow(&Kind::Token, t0));
        assert!(!th.allow(&Kind::Token, t0 + Duration::from_secs(599)));
        assert!(th.allow(&Kind::Lost, t0), "other kinds are counted apart");
        assert!(th.allow(&Kind::Core(ToastKind::FfmpegStart), t0));
        assert!(th.allow(&Kind::Core(ToastKind::FfmpegDone), t0), "start and done are different kinds");
        assert!(th.allow(&Kind::Token, t0 + QUIET));
    }
}
