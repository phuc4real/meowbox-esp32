//! Tokens, context use, model and title from a Claude Code transcript (.jsonl), read incrementally.

use serde_json::Value;
use std::collections::HashSet;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;

#[derive(Debug, Clone, Default)]
pub struct Usage {
    /// Bytes already read (whole lines only).
    pub offset: u64,
    /// API message ids already counted.
    pub seen: HashSet<String>,
    pub tokens: u64,
    /// Context of the last message: input + cache read + cache creation.
    pub ctx: u64,
    pub model: String,
    pub title: String,
}

fn n(u: &Value, k: &str) -> u64 {
    u.get(k).and_then(Value::as_u64).unwrap_or(0)
}

/// Read what was appended since last time. Usage is per API message; a message is split over several lines with
/// the same id and the same usage, so it is counted once. A line still being written waits for the next read.
pub fn read(p: &Path, u: &mut Usage) -> std::io::Result<()> {
    let size = std::fs::metadata(p)?.len();
    if size < u.offset {
        // rewritten
        u.offset = 0;
        u.tokens = 0;
        u.seen.clear();
    }
    if size == u.offset {
        return Ok(());
    }
    let mut f = std::fs::File::open(p)?;
    f.seek(SeekFrom::Start(u.offset))?;
    let mut data = Vec::with_capacity((size - u.offset) as usize);
    f.take(size - u.offset).read_to_end(&mut data)?;
    let end = data.iter().rposition(|&b| b == b'\n').map_or(0, |i| i + 1);
    u.offset += end as u64;
    for raw in data[..end].split(|&b| b == b'\n') {
        let has = |needle: &[u8]| raw.windows(needle.len()).any(|w| w == needle);
        if !has(b"\"usage\"") && !has(b"\"ai-title\"") {
            continue;
        }
        let Ok(j) = serde_json::from_slice::<Value>(raw) else { continue };
        let typ = j.get("type").and_then(Value::as_str);
        if typ == Some("ai-title") {
            if let Some(t) = j.get("aiTitle").and_then(Value::as_str).filter(|t| !t.is_empty()) {
                u.title = t.to_owned();
            }
            continue;
        }
        if typ != Some("assistant") || j.get("isSidechain").and_then(Value::as_bool).unwrap_or(false) {
            continue;
        }
        let Some(m) = j.get("message") else { continue };
        let Some(us) = m.get("usage").filter(|x| x.as_object().is_some_and(|o| !o.is_empty())) else { continue };
        let Some(mid) = m.get("id").and_then(Value::as_str).filter(|x| !x.is_empty()) else { continue };
        if !u.seen.insert(mid.to_owned()) {
            continue;
        }
        let model = m.get("model").and_then(Value::as_str).filter(|x| !x.is_empty()).unwrap_or(&u.model).to_owned();
        if !model.is_empty() && !model.starts_with('<') {
            // "<synthetic>" lines carry no real usage
            u.model = model;
        }
        let (inp, out) = (n(us, "input_tokens"), n(us, "output_tokens"));
        let (cr, cw) = (n(us, "cache_read_input_tokens"), n(us, "cache_creation_input_tokens"));
        u.tokens += inp + out + cr + cw;
        u.ctx = inp + cr + cw;
    }
    Ok(())
}
