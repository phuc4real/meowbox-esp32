//! What the relay asks yt-dlp, and what it picks from the answer: URL check, argv, the -J cache, format choice and
//! the response headers.

use serde_json::Value;
use std::collections::HashMap;
use std::sync::Mutex;
use std::time::{Duration, Instant};

/// The relay only plays YouTube links: http(s), host youtube.com / youtu.be or a subdomain, then "/" or the end,
/// no whitespace anywhere. yt-dlp gets the link after "--", so it can never be read as an option.
pub fn check_url(url: &str) -> bool {
    if url.chars().any(char::is_whitespace) {
        return false;
    }
    let lower = url.to_ascii_lowercase();
    let Some(rest) = lower.strip_prefix("https://").or_else(|| lower.strip_prefix("http://")) else {
        return false;
    };
    let host = rest.split('/').next().unwrap_or("");
    let base = ["youtube.com", "youtu.be"].into_iter().find(|d| host == *d || host.ends_with(&format!(".{d}")));
    let Some(base) = base else { return false };
    let sub = &host[..host.len() - base.len()];
    // each label before the base: [a-z0-9-]+
    sub.is_empty()
        || (sub.ends_with('.')
            && sub[..sub.len() - 1]
                .split('.')
                .all(|l| !l.is_empty() && l.chars().all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '-')))
}

/// Options for every yt-dlp run (after the exe).
pub const BASE_ARGS: &[&str] = &["--no-warnings", "--quiet", "--no-progress"];

/// argv after the exe for `-J` (info JSON). Only for a checked URL.
pub fn info_args(url: &str, single: bool) -> Vec<String> {
    let mut a: Vec<String> = BASE_ARGS.iter().map(|s| s.to_string()).collect();
    a.extend(["-J".into(), "--flat-playlist".into()]);
    if single {
        a.push("--no-playlist".into());
    }
    a.extend(["--".into(), url.into()]);
    a
}

/// -J results for 30 min, keyed by (url, single).
pub struct InfoCache(Mutex<HashMap<(String, bool), (Instant, Value)>>);

impl Default for InfoCache {
    fn default() -> Self {
        Self(Mutex::new(HashMap::new()))
    }
}

impl InfoCache {
    pub fn get(&self, url: &str, single: bool) -> Option<Value> {
        let m = self.0.lock().unwrap_or_else(|e| e.into_inner());
        m.get(&(url.to_owned(), single))
            .filter(|(t, _)| t.elapsed() < Duration::from_secs(1800))
            .map(|(_, v)| v.clone())
    }

    pub fn put(&self, url: &str, single: bool, v: Value) {
        let mut m = self.0.lock().unwrap_or_else(|e| e.into_inner());
        m.retain(|_, (t, _)| t.elapsed() < Duration::from_secs(1800));
        m.insert((url.to_owned(), single), (Instant::now(), v));
    }
}

pub fn entry_url(e: &Value) -> String {
    let u = ["url", "webpage_url", "id"]
        .iter()
        .find_map(|k| e.get(*k).and_then(Value::as_str).filter(|s| !s.is_empty()))
        .unwrap_or("");
    if u.starts_with("http") { u.to_owned() } else { format!("https://www.youtube.com/watch?v={u}") }
}

fn s<'a>(v: &'a Value, k: &str) -> &'a str {
    v.get(k).and_then(Value::as_str).unwrap_or("")
}

fn f(v: &Value, k: &str) -> f64 {
    v.get(k).and_then(Value::as_f64).unwrap_or(0.0)
}

/// Best AAC-in-MP4 audio-only format (YouTube itag 140 is ~128 kbps), not above 192 kbps.
pub fn pick_aac(info: &Value) -> Option<Value> {
    let fmts: Vec<&Value> = info
        .get("formats")
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
        .filter(|x| {
            s(x, "vcodec") == "none"
                && s(x, "acodec").starts_with("mp4a")
                && matches!(s(x, "protocol"), "https" | "http")
        })
        .collect();
    let good: Vec<&Value> = fmts.iter().copied().filter(|x| f(x, "abr") <= 192.0).collect();
    let pool = if good.is_empty() { fmts } else { good };
    // max() keeps the last of equals; Python's max keeps the first
    pool.into_iter().rev().max_by(|a, b| f(a, "abr").total_cmp(&f(b, "abr"))).cloned()
}

/// Live: YouTube's audio-only HLS formats (233 ~48 kbps, 234 ~128 kbps; acodec reported as None): the best one.
pub fn pick_hls(info: &Value) -> Option<Value> {
    let known = |id: &str| match id {
        "233" => 48.0,
        "234" => 128.0,
        _ => 0.0,
    };
    let rate = |x: &Value| {
        let t = f(x, "tbr");
        if t > 0.0 {
            return t;
        }
        let a = f(x, "abr");
        if a > 0.0 {
            return a;
        }
        let id = match x.get("format_id") {
            Some(Value::String(s)) => s.clone(),
            Some(v) => v.to_string(),
            None => String::new(),
        };
        known(&id)
    };
    info.get("formats")
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
        .filter(|x| s(x, "protocol").starts_with("m3u8") && s(x, "vcodec") == "none" && s(x, "acodec") != "none")
        .rev()
        .max_by(|a, b| rate(a).total_cmp(&rate(b)))
        .cloned()
}

pub fn title(info: &Value) -> String {
    let t = [s(info, "fulltitle"), s(info, "title")].into_iter().find(|t| !t.is_empty()).unwrap_or("YouTube");
    // Python: re.sub(r"[\r\n]+", " ", t)
    let mut out = String::with_capacity(t.len());
    let mut in_run = false;
    for c in t.chars() {
        let nl = c == '\r' || c == '\n';
        if nl && !in_run {
            out.push(' ');
        } else if !nl {
            out.push(c);
        }
        in_run = nl;
    }
    out
}

/// Response headers for the speaker's player.
pub fn headers(
    info: &Value,
    title: &str,
    index: Option<(usize, usize)>,
    ctype: &str,
    br: Option<u32>,
) -> Vec<(String, String)> {
    let mut h = vec![
        ("Content-Type".to_owned(), ctype.to_owned()),
        ("Cache-Control".into(), "no-cache".into()),
        ("icy-name".into(), title.to_owned()),
        ("X-Is-Live".into(), info.get("is_live").and_then(Value::as_bool).unwrap_or(false).to_string()),
    ];
    if let Some(br) = br.filter(|b| *b > 0) {
        h.push(("icy-br".into(), br.to_string()));
    }
    if let Some(d) = info.get("duration").and_then(Value::as_f64).filter(|d| *d != 0.0) {
        h.push(("X-Duration".into(), crate::json::round(d).to_string()));
    }
    if let Some((i, n)) = index {
        h.push(("X-Playlist-Index".into(), i.to_string()));
        h.push(("X-Playlist-Count".into(), n.to_string()));
    }
    h
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn urls_like_the_python_agent() {
        for u in [
            "https://www.youtube.com/watch?v=abc",
            "http://youtu.be/abc",
            "https://music.youtube.com/playlist?list=x",
            "HTTPS://M.YOUTUBE.COM/watch?v=abc",
            "https://youtube.com",
        ] {
            assert!(check_url(u), "accepts {u}");
        }
        for u in [
            "",
            "-x",
            "--version",
            "youtu.be/abc",
            "ftp://youtube.com/x",
            "https://youtube.com.example.org/x",
            "https://evil.example/youtube.com",
            "https://notyoutube.com/x",
            "https://youtube.com/x y",
            "file:///c:/x",
            "https://.youtube.com/x",
            "https://a..youtube.com/x",
            "https://youtube.com:443/x",
            "https://youtube.com?x=1",
        ] {
            assert!(!check_url(u), "refuses {u:?}");
        }
    }

    #[test]
    fn argv_ends_with_the_url_after_dashes() {
        for single in [true, false] {
            let a = info_args("https://youtu.be/abc", single);
            assert_eq!(&a[a.len() - 2..], ["--", "https://youtu.be/abc"]);
            assert_eq!(a.contains(&"--no-playlist".to_string()), single);
            assert!(a.starts_with(&BASE_ARGS.iter().map(|s| s.to_string()).collect::<Vec<_>>()));
        }
    }

    #[test]
    fn picks() {
        let info = json!({"formats": [
            {"format_id": "139", "vcodec": "none", "acodec": "mp4a.40.5", "protocol": "https", "abr": 48},
            {"format_id": "140", "vcodec": "none", "acodec": "mp4a.40.2", "protocol": "https", "abr": 129.5},
            {"format_id": "141", "vcodec": "none", "acodec": "mp4a.40.2", "protocol": "https", "abr": 256},
            {"format_id": "251", "vcodec": "none", "acodec": "opus", "protocol": "https", "abr": 160},
            {"format_id": "18", "vcodec": "avc1", "acodec": "mp4a.40.2", "protocol": "https", "abr": 96},
            {"format_id": "233", "vcodec": "none", "acodec": null, "protocol": "m3u8_native"},
            {"format_id": "234", "vcodec": "none", "acodec": null, "protocol": "m3u8_native"}
        ]});
        assert_eq!(pick_aac(&info).unwrap()["format_id"], "140");
        assert_eq!(pick_hls(&info).unwrap()["format_id"], "234");
        assert!(pick_aac(&json!({"formats": [{"vcodec": "none", "acodec": "opus", "protocol": "https"}]})).is_none());
        // all above 192 kbps: the best of them
        let hi = json!({"formats": [{"format_id": "a", "vcodec": "none", "acodec": "mp4a", "protocol": "https", "abr": 256}]});
        assert_eq!(pick_aac(&hi).unwrap()["format_id"], "a");
    }

    #[test]
    fn headers_and_title() {
        let info = json!({"title": "Lofi\nradio", "is_live": false, "duration": 212.6});
        assert_eq!(title(&info), "Lofi radio");
        let h = headers(&info, &title(&info), Some((2, 9)), "audio/aac", Some(128));
        let get = |k: &str| h.iter().find(|(a, _)| a == k).map(|(_, v)| v.as_str());
        assert_eq!(get("X-Is-Live"), Some("false"));
        assert_eq!(get("X-Duration"), Some("213"));
        assert_eq!(get("icy-br"), Some("128"));
        assert_eq!((get("X-Playlist-Index"), get("X-Playlist-Count")), (Some("2"), Some("9")));
        assert_eq!(entry_url(&json!({"id": "abc"})), "https://www.youtube.com/watch?v=abc");
        assert_eq!(entry_url(&json!({"url": "https://youtu.be/x"})), "https://youtu.be/x");
    }
}
