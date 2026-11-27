//! Meow Coding checks: the Python test_agent.py cases, outputs taken from the Python coding.py for the same inputs,
//! and event-order / transcript / settings.json cases. Nothing outside a temporary folder is touched.

use super::hooks::{self, HOOK_HEADER, Install};
use super::*;
use serde_json::json;
use std::sync::Arc;

fn tmp(name: &str) -> PathBuf {
    let d = std::env::temp_dir().join(format!("meowbox-coding-{}-{name}", std::process::id()));
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).unwrap();
    d
}

#[test]
fn model_labels_like_python() {
    for (m, want) in [
        ("claude-opus-5-5", "Opus 5.5"),
        ("claude-sonnet-4-5-20250929", "Sonnet 4.5"),
        ("claude-opus-4-20250514", "Opus 4"),
        ("claude-haiku-4-5", "Haiku 4.5"),
        ("claude-3-5-sonnet", ""),
        ("claude-opus-45", "Opus 45"),
        ("claude-fable-5-2[1m]", "Fable 5.2"),
        ("claude-opus-4-1-x", "Opus 4.1"),
        ("claude-Opus-4", ""),
        ("gpt-4", ""),
        ("", ""),
    ] {
        assert_eq!(model_label(m), want, "{m}");
    }
    assert_eq!(window("claude-opus-4-5-20251101"), 200_000);
    assert_eq!(window("claude-opus-5-5"), 1_000_000);
}

#[test]
fn small_helpers_like_python() {
    assert_eq!(short("  hello   world  ", 48), "hello world");
    assert_eq!(short(&"a".repeat(50), 48), format!("{}…", "a".repeat(47)));
    assert_eq!(short("xin chào các bạn", 8), "xin chà…");
    assert_eq!(short("", 5), "");
    for (p, want) in [
        ("D:\\repo\\meowbox", "meowbox"),
        ("D:\\repo\\meowbox\\", "meowbox"),
        ("/home/x/proj/", "proj"),
        ("C:", ""),
        ("C:\\", ""),
        ("proj", "proj"),
    ] {
        assert_eq!(basename(p), want, "{p}");
    }
    assert_eq!(action("Read", &json!({"file_path": "a.PNG"})), "image");
    assert_eq!(action("Read", &json!({"file_path": "a.rs"})), "read");
    assert_eq!(action("mcp__claude-in-chrome__navigate", &json!({})), "web");
    assert_eq!(action("mcp__x__y", &json!({})), "tool");
    assert_eq!(action("Bash", &json!({})), "bash");
    assert_eq!(action("Foo", &json!({})), "tool");
    assert_eq!(action("Read", &json!({})), "read");
    for (v, want) in [
        (json!(1764000000), 1764000000),
        (json!(1764000000123u64), 1764000000),
        (json!("2026-11-27T13:00:00Z"), 1795784400),
        (json!("2026-11-27T13:00:00.5+07:00"), 1795759200),
        (json!(""), 0),
        (Value::Null, 0),
        (json!("garbage"), 0),
    ] {
        assert_eq!(epoch(&v), want, "{v}");
    }
}

#[test]
fn status_line_like_python() {
    let tr = Tracker::default();
    assert_eq!(tr.status(&json!({})), "=^.^= MeowBox");
    let ev = json!({"rate_limits": {"five_hour": {"used_percentage": 72.5, "resets_at": 1764000000},
                                    "seven_day": {"utilization": 12}}});
    assert_eq!(tr.status(&ev), "=o.o= MeowBox · 5h 72% · 7d 12%");
    assert_eq!(Value::Object(tr.limits_msg()), json!({"h5": 72, "h5r": 1764000000, "d7": 12}));
    assert_eq!(tr.status(&json!({"rate_limits": {"five_hour": {"used_percentage": 95}}})), "=x.x= MeowBox · 5h 95%");
    assert_eq!(Value::Object(tr.limits_msg()), json!({"h5": 95}));
}

#[test]
fn transcript_paths() {
    let root = tmp("paths").join(".claude");
    let proj = root.join("projects").join("p");
    std::fs::create_dir_all(&proj).unwrap();
    std::fs::write(proj.join("s.jsonl"), "").unwrap();
    assert!(transcript_ok_in(&proj.join("s.jsonl").to_string_lossy(), &root));
    let other = root.parent().unwrap().join("other.jsonl");
    for p in [
        r"\\lab-host\share\t.jsonl".to_owned(),
        "//lab-host/share/t.jsonl".to_owned(),
        r"\\?\UNC\lab-host\share\t.jsonl".to_owned(),
        other.to_string_lossy().into_owned(),
        proj.join("s.txt").to_string_lossy().into_owned(),
        proj.join("..").join("..").join("..").join("x.jsonl").to_string_lossy().into_owned(),
        String::new(),
    ] {
        assert!(!transcript_ok_in(&p, &root), "{p:?} must not be read");
    }
    assert_eq!(git_branch(r"\\lab-host\share\proj"), "");
}

#[test]
fn git_branch_from_head() {
    let d = tmp("git");
    std::fs::create_dir_all(d.join(".git")).unwrap();
    std::fs::write(d.join(".git").join("HEAD"), "ref: refs/heads/feature/cat-screen\n").unwrap();
    std::fs::create_dir_all(d.join("src").join("deep")).unwrap();
    assert_eq!(git_branch(&d.join("src").join("deep").to_string_lossy()), "cat-screen");
    std::fs::write(d.join(".git").join("HEAD"), "0123456789abcdef\n").unwrap();
    assert_eq!(git_branch(&d.to_string_lossy()), "0123456");
}

fn ev(sid: &str, name: &str, extra: Value) -> Value {
    let mut v = json!({"session_id": sid, "hook_event_name": name});
    if let (Some(o), Some(e)) = (v.as_object_mut(), extra.as_object()) {
        o.extend(e.clone());
    }
    v
}

fn state_of(tr: &Tracker, sid: &str) -> (String, String) {
    let g = tr.lock();
    let s = &g.sessions[sid];
    (s.state.to_owned(), s.act.to_owned())
}

#[test]
fn events_in_and_out_of_order() {
    let tr = Tracker::default();
    tr.event(&ev("a", "SessionStart", json!({"cwd": "C:\\proj\\meowbox"})));
    assert_eq!(state_of(&tr, "a"), ("idle".into(), "".into()));
    tr.event(&ev("a", "UserPromptSubmit", json!({})));
    tr.event(&ev("a", "PreToolUse", json!({"tool_name": "Edit", "tool_input": {}})));
    assert_eq!(state_of(&tr, "a"), ("work".into(), "edit".into()));
    tr.event(&ev("a", "Stop", json!({})));
    // background hooks: a PostToolUse / PreToolUse arriving after Stop must not undo "done"
    tr.event(&ev("a", "PostToolUse", json!({"tool_name": "Edit"})));
    tr.event(&ev("a", "PreToolUse", json!({"tool_name": "Bash"})));
    assert_eq!(state_of(&tr, "a"), ("done".into(), "".into()));
    // permission prompt overtaking its PreToolUse: stays "wait"
    tr.event(&ev("a", "UserPromptSubmit", json!({})));
    tr.event(&ev("a", "PermissionRequest", json!({"tool_name": "Bash"})));
    tr.event(&ev("a", "PreToolUse", json!({"tool_name": "Bash"})));
    assert_eq!(state_of(&tr, "a"), ("wait".into(), "bash".into()));
    tr.event(&ev("a", "PreToolUse", json!({"tool_name": "AskUserQuestion"})));
    assert_eq!(state_of(&tr, "a"), ("wait".into(), "ask".into()));
    tr.event(&ev("a", "Notification", json!({"message": "Claude needs your permission to use Bash"})));
    assert_eq!(state_of(&tr, "a").0, "wait");
    tr.event(&ev("a", "PreCompact", json!({})));
    assert_eq!(state_of(&tr, "a"), ("work".into(), "compact".into()));
    tr.event(&ev("a", "StopFailure", json!({})));
    assert_eq!(state_of(&tr, "a").0, "err");
    // missing fields: ignored
    tr.event(&json!({"hook_event_name": "Stop"}));
    tr.event(&json!({"session_id": "b"}));
    assert_eq!(tr.session_ids(), vec!["a".to_string()]);
    tr.event(&ev("a", "SessionEnd", json!({})));
    assert!(tr.session_ids().is_empty());
}

#[test]
fn snapshot_order_and_fields() {
    let tr = Tracker::default();
    tr.event(&ev("idle-session-0001", "SessionStart", json!({"cwd": "C:\\work\\api"})));
    tr.event(&ev("waiting-session-02", "UserPromptSubmit", json!({"cwd": "C:\\work\\web\\"})));
    tr.event(&ev("waiting-session-02", "PermissionRequest", json!({"tool_name": "Write"})));
    tr.event(&ev("working-session-03", "UserPromptSubmit", json!({})));
    let s = tr.snapshot();
    let ids: Vec<&str> = s.iter().map(|x| x["id"].as_str().unwrap()).collect();
    assert_eq!(ids, ["waiting-", "working-", "idle-ses"]);
    assert_eq!(s[0]["st"], "wait");
    assert_eq!(s[0]["act"], "write");
    assert_eq!(s[0]["dir"], "web");
    assert_eq!(s[0]["ag"], "claude");
    assert!(s[0].get("ctx").is_none(), "ctx left out until known");
    for k in ["id", "ag", "name", "dir", "br", "st", "act", "for", "tok", "model"] {
        assert!(s[2].get(k).is_some(), "{k}");
    }
    for i in 0..6 {
        tr.event(&ev(&format!("extra-{i}"), "SessionStart", json!({})));
    }
    assert_eq!(tr.snapshot().len(), 4);
}

#[test]
fn transcript_reading() {
    let root = tmp("transcript").join(".claude");
    std::fs::create_dir_all(&root).unwrap();
    let path = root.join("s.jsonl");
    let msg = |id: &str, model: &str, inp: u64, cr: u64| {
        json!({"type": "assistant", "message": {"id": id, "model": model,
            "usage": {"input_tokens": inp, "output_tokens": 10, "cache_read_input_tokens": cr,
                      "cache_creation_input_tokens": 5}}})
        .to_string()
    };
    let lines = [
        msg("m1", "claude-opus-5-5", 100, 1000),
        msg("m1", "claude-opus-5-5", 100, 1000), // same message split over two lines: counted once
        json!({"type": "assistant", "isSidechain": true, "message": {"id": "sub", "usage": {"input_tokens": 999}}})
            .to_string(),
        msg("m2", "<synthetic>", 1, 0),
        json!({"type": "ai-title", "aiTitle": "Rust agent rewrite"}).to_string(),
        json!({"type": "user", "message": {"content": "no usage here"}}).to_string(),
    ];
    std::fs::write(&path, lines.join("\n") + "\n" + &msg("m3", "claude-opus-5-5", 7, 0)[..20]).unwrap();

    let tr = Arc::new(Tracker::with_claude_dir(root.clone()));
    tr.event(&ev("s1", "UserPromptSubmit", json!({"transcript_path": path.to_string_lossy()})));
    tr.refresh(true);
    let snap = tr.snapshot();
    assert_eq!(snap[0]["name"], "Rust agent rewrite");
    assert_eq!(snap[0]["tok"], 1115 + 16, "m1 once + m2, not the sidechain, not the half-written m3");
    assert_eq!(snap[0]["model"], "Opus 5.5", "<synthetic> does not replace the model");
    assert_eq!(snap[0]["ctx"], 0, "ctx of the last message (m2: 1 + 0 + 5) of a 1M window rounds to 0");

    // the rest of m3 arrives
    let full = std::fs::read_to_string(&path).unwrap();
    let cut = full.rfind('\n').unwrap() + 1;
    std::fs::write(&path, format!("{}{}\n", &full[..cut], msg("m3", "claude-opus-5-5", 7, 0))).unwrap();
    tr.refresh(true);
    assert_eq!(tr.snapshot()[0]["tok"], 1131 + 22);

    // status line context overrides the estimate
    tr.status(&json!({"session_id": "s1", "context_window": {"used_percentage": 41.6}}));
    assert_eq!(tr.snapshot()[0]["ctx"], 42);

    // a transcript outside the Claude folder is not taken
    tr.event(&ev("s2", "UserPromptSubmit", json!({"transcript_path": "C:\\Windows\\win.ini"})));
    assert!(tr.lock().sessions["s2"].transcript.is_empty());
}

#[test]
fn hook_key_and_commands_match_python() {
    let d = tmp("key");
    let p = d.join("hook.key");
    let k = hooks::hook_key_at(&p);
    assert_eq!(k.len(), 32);
    assert_eq!(hooks::hook_key_at(&p), k, "made once");
    std::fs::write(&p, "0123456789abcdef0123456789abcdef").unwrap();
    let inst = Install { key: hooks::hook_key_at(&p), port: 8767 };
    // byte for byte what the Python agent writes for this key
    assert_eq!(
        inst.hook_command(),
        "curl -s -m 3 -o /dev/null -H \"X-MeowBox-Hook: 0123456789abcdef0123456789abcdef\" -H \"Content-Type: application/json\" --data-binary @- http://127.0.0.1:8767/hook || exit 0"
    );
    assert_eq!(
        inst.status_command(),
        "curl -s -m 1 -H \"X-MeowBox-Hook: 0123456789abcdef0123456789abcdef\" -H \"Content-Type: application/json\" --data-binary @- http://127.0.0.1:8767/status"
    );
    std::fs::write(&p, "not a key").unwrap();
    assert_ne!(hooks::hook_key_at(&p), "not a key");
}

#[test]
fn settings_install_refresh_uninstall() {
    let d = tmp("settings");
    let path = d.join("settings.json");
    let inst = Install { key: "0123456789abcdef0123456789abcdef".into(), port: 8767 };
    let old_cmd = "curl -s -m 3 -o /dev/null --data-binary @- http://127.0.0.1:8767/hook || exit 0";
    let foreign = json!({"type": "command", "command": "notify-send done"});
    std::fs::write(
        &path,
        json!({"hooks": {"Stop": [{"hooks": [{"type": "command", "command": old_cmd}, foreign]}]}, "model": "keep-me"})
            .to_string(),
    )
    .unwrap();
    assert!(hooks::hooks_installed_at(&path));
    assert!(hooks::refresh_at(&path, &inst).unwrap(), "old hooks: rewritten");
    let cfg: Value = serde_json::from_str(&std::fs::read_to_string(&path).unwrap()).unwrap();
    assert!(cfg["hooks"].to_string().contains(&inst.key), "new commands carry the key");
    assert_eq!(cfg["model"], "keep-me");
    assert!(cfg["hooks"]["Stop"].to_string().contains("notify-send done"), "someone else's hook kept");
    assert_eq!(cfg["hooks"].as_object().unwrap().len(), EVENTS.len());
    assert_eq!(cfg["statusLine"]["command"], inst.status_command());
    assert!(d.join("settings.json.meowbox.bak").exists(), "backup of the original");
    assert!(!hooks::refresh_at(&path, &inst).unwrap(), "current hooks: left alone");

    hooks::uninstall_at(&path).unwrap();
    let cfg: Value = serde_json::from_str(&std::fs::read_to_string(&path).unwrap()).unwrap();
    assert_eq!(cfg, json!({"hooks": {"Stop": [{"hooks": [foreign]}]}, "model": "keep-me"}));
    assert!(!hooks::hooks_installed_at(&path));

    // someone's own status line is never replaced
    std::fs::write(&path, json!({"statusLine": {"type": "command", "command": "my-line"}}).to_string()).unwrap();
    hooks::install_at(&path, &inst).unwrap();
    let cfg: Value = serde_json::from_str(&std::fs::read_to_string(&path).unwrap()).unwrap();
    assert_eq!(cfg["statusLine"]["command"], "my-line");

    std::fs::write(&path, json!({"model": "x"}).to_string()).unwrap();
    assert!(!hooks::refresh_at(&path, &inst).unwrap(), "Meow Coding off: nothing installed");
    assert_eq!(serde_json::from_str::<Value>(&std::fs::read_to_string(&path).unwrap()).unwrap(), json!({"model": "x"}));

    std::fs::write(&path, "{ not json").unwrap();
    assert!(hooks::install_at(&path, &inst).is_err(), "a broken file is not overwritten");
    assert_eq!(std::fs::read_to_string(&path).unwrap(), "{ not json");
}

#[tokio::test]
async fn hook_server_only_takes_our_hook() {
    let tr = Arc::new(Tracker::default());
    let key = "0123456789abcdef0123456789abcdef".to_string();
    let addr = server::start(tr.clone(), 0, key.clone()).await.unwrap();
    let url = format!("http://{addr}");
    let c = reqwest::Client::builder().no_proxy().build().unwrap();
    let body = json!({"session_id": "t1", "hook_event_name": "SessionStart", "cwd": "C:\\x"}).to_string();
    type Case<'a> = (&'a str, Vec<(&'a str, &'a str)>, u16);
    let cases: [Case; 5] = [
        ("no key", vec![("Content-Type", "application/json")], 403),
        (
            "wrong key",
            vec![(HOOK_HEADER, "00000000000000000000000000000000"), ("Content-Type", "application/json")],
            403,
        ),
        ("text/plain body", vec![(HOOK_HEADER, key.as_str()), ("Content-Type", "text/plain")], 403),
        (
            "from a web page (Origin)",
            vec![(HOOK_HEADER, key.as_str()), ("Content-Type", "application/json"), ("Origin", "https://example.org")],
            403,
        ),
        ("the installed hook", vec![(HOOK_HEADER, key.as_str()), ("Content-Type", "application/json")], 200),
    ];
    for (name, hdrs, want) in cases {
        let mut r = c.post(format!("{url}/hook")).body(body.clone());
        for (k, v) in hdrs {
            r = r.header(k, v);
        }
        assert_eq!(r.send().await.unwrap().status().as_u16(), want, "{name}");
    }
    let r = c.post(format!("{url}/status")).header("Content-Type", "application/json").body("{}").send().await.unwrap();
    assert_eq!(r.status().as_u16(), 403, "/status without the key");
    let r = c
        .post(format!("{url}/status"))
        .header(HOOK_HEADER, key.as_str())
        .header("Content-Type", "application/json; charset=utf-8")
        .body("")
        .send()
        .await
        .unwrap();
    assert_eq!(r.text().await.unwrap(), "=^.^= MeowBox", "empty status body = {{}}");
    assert_eq!(tr.session_ids(), vec!["t1".to_string()], "only the real hook made a session");
}
