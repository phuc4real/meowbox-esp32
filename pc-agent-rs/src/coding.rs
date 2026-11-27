//! Meow Coding (Claude Code sessions for the speaker's cat screen). Phase 3 ports the tracker, the hook server and
//! the settings.json installer; until then the link sends an empty list, as the Python agent does with no session.

use serde_json::{Map, Value};

#[derive(Default)]
pub struct Tracker;

impl Tracker {
    /// Read new transcript lines (tokens, context, title).
    pub fn refresh_usage(&self) {}

    /// Sessions for the speaker, most important first (at most 4).
    pub fn snapshot(&self) -> Vec<Value> {
        Vec::new()
    }

    /// Plan usage limits from Claude Code's status line; empty until reported.
    pub fn limits_msg(&self) -> Map<String, Value> {
        Map::new()
    }
}
