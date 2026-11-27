//! The /ws/pc link against a fake speaker (like pc-agent/mock_speaker.py and the firmware): ping, hello with the
//! token, auth / busy / take over, media_cmd answered with media, repeated-key messages ignored.

use axum::extract::State;
use axum::extract::ws::{Message, WebSocket, WebSocketUpgrade};
use axum::response::IntoResponse;
use axum::routing::get;
use axum::{Json, Router};
use meowbox_agent::config::{Config, SharedConfig};
use meowbox_agent::link::{CoreHandle, Options};
use meowbox_agent::state::{LinkError, SharedState};
use serde_json::{Value, json};
use std::sync::{Arc, Mutex};
use std::time::Duration;
use tokio::sync::{broadcast, mpsc};

#[derive(Clone, Copy, PartialEq, Debug)]
enum Answer {
    Accept,
    Auth,
    Busy,
}

#[derive(Clone)]
struct Fake {
    /// Every text frame the agent sends.
    inbox: mpsc::UnboundedSender<Value>,
    /// Frames to send to the connected agent.
    outbox: broadcast::Sender<String>,
    answer: Arc<Mutex<Answer>>,
}

async fn ping() -> Json<Value> {
    Json(json!({"name": "MeowBox-TEST", "host": "meowbox-test", "ver": "test", "proto": 2, "proto_min": 2}))
}

async fn ws_pc(State(f): State<Fake>, ws: WebSocketUpgrade) -> impl IntoResponse {
    ws.on_upgrade(move |sock| conn(f, sock))
}

async fn conn(f: Fake, mut sock: WebSocket) {
    let mut out = f.outbox.subscribe();
    let mut first = true;
    loop {
        tokio::select! {
            m = sock.recv() => {
                let Some(Ok(Message::Text(t))) = m else { return };
                let v: Value = serde_json::from_str(&t).unwrap();
                let _ = f.inbox.send(v.clone());
                if first {
                    first = false;
                    let answer = *f.answer.lock().unwrap();
                    let reply = match answer {
                        Answer::Auth => Some(json!({"t": "auth"})),
                        Answer::Busy if v.get("take") != Some(&json!(true)) => Some(json!({"t": "busy", "pc": "OTHER-PC"})),
                        _ => None,
                    };
                    if let Some(r) = reply {
                        let _ = sock.send(Message::Text(r.to_string().into())).await;
                        return;
                    }
                }
            }
            o = out.recv() => {
                let Ok(o) = o else { return };
                if sock.send(Message::Text(o.into())).await.is_err() { return }
            }
        }
    }
}

async fn start_fake() -> (String, Fake, mpsc::UnboundedReceiver<Value>) {
    let (inbox, rx) = mpsc::unbounded_channel();
    let fake = Fake { inbox, outbox: broadcast::channel(16).0, answer: Arc::new(Mutex::new(Answer::Accept)) };
    let app = Router::new().route("/api/ping", get(ping)).route("/ws/pc", get(ws_pc)).with_state(fake.clone());
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let host = listener.local_addr().unwrap().to_string();
    tokio::spawn(async move { axum::serve(listener, app).await.unwrap() });
    (host, fake, rx)
}

async fn next_of(rx: &mut mpsc::UnboundedReceiver<Value>, t: &str) -> Value {
    tokio::time::timeout(Duration::from_secs(10), async {
        loop {
            let v = rx.recv().await.expect("fake speaker gone");
            if v["t"] == t {
                return v;
            }
        }
    })
    .await
    .unwrap_or_else(|_| panic!("no {t} message within 10 s"))
}

async fn wait_state(state: &SharedState, what: &str, f: impl Fn(&meowbox_agent::state::AgentState) -> bool) {
    for _ in 0..100 {
        if f(&state.get()) {
            return;
        }
        tokio::time::sleep(Duration::from_millis(100)).await;
    }
    panic!("state never became {what}: {:?}", state.get());
}

fn core_for(host: &str, token: &str) -> (CoreHandle, std::path::PathBuf) {
    let dir =
        std::env::temp_dir().join(format!("meowbox-link-{}-{}", std::process::id(), host.replace([':', '.'], "-")));
    std::fs::create_dir_all(&dir).unwrap();
    let path = dir.join("agent.json");
    let mut cfg = Config::default();
    cfg.host = host.into();
    cfg.token = token.into();
    let core = CoreHandle::start(SharedConfig::new(cfg, path.clone()), SharedState::default(), Options::default());
    (core, path)
}

#[tokio::test(flavor = "multi_thread")]
async fn hello_stats_media_and_commands() {
    let (host, fake, mut rx) = start_fake().await;
    let (core, path) = core_for(&host, "secret");

    let hello = next_of(&mut rx, "hello").await;
    assert_eq!(hello["token"], "secret");
    assert_eq!(hello["proto"], 2);
    assert_eq!(hello["ver"], meowbox_agent::VERSION);
    assert!(hello.get("take").is_none());
    assert!(!hello["name"].as_str().unwrap().is_empty());

    assert_eq!(next_of(&mut rx, "coding").await["s"], json!([]));
    let stats = next_of(&mut rx, "stats").await;
    assert!(stats["cpu"].is_i64() && stats["ram"].is_i64());
    next_of(&mut rx, "media").await;
    wait_state(core.state(), "connected", |s| s.connected && s.name == "MeowBox-TEST" && s.how == "manual").await;

    // the working IP and the speaker name are saved
    let saved: Value = serde_json::from_slice(&std::fs::read(&path).unwrap()).unwrap();
    assert_eq!((saved["last_host"].as_str(), saved["speaker"].as_str()), (Some(host.as_str()), Some("MeowBox-TEST")));

    // a message with a repeated key is ignored (it could mean busy or media_cmd)
    fake.outbox.send(r#"{"t":"busy","t":"media_cmd","cmd":"nope","pc":"X"}"#.into()).unwrap();
    // an unknown media command is refused but still answered with the media state, right away
    fake.outbox.send(r#"{"t":"media_cmd","cmd":"nope"}"#.into()).unwrap();
    next_of(&mut rx, "media").await;
    tokio::time::sleep(Duration::from_millis(300)).await;
    assert!(core.state().get().connected, "still connected after the repeated-key message");

    // take over: the next hello says so
    *fake.answer.lock().unwrap() = Answer::Busy;
    core.reconnect_now(true);
    let hello = next_of(&mut rx, "hello").await;
    assert_eq!(hello["take"], true);
    wait_state(core.state(), "connected again", |s| s.connected).await;
}

#[tokio::test(flavor = "multi_thread")]
async fn wrong_token_and_busy() {
    let (host, fake, mut rx) = start_fake().await;
    *fake.answer.lock().unwrap() = Answer::Auth;
    let (core, _) = core_for(&host, "wrong");
    next_of(&mut rx, "hello").await;
    wait_state(core.state(), "token error", |s| !s.connected && s.error == LinkError::Token).await;
    // it waits 60 s before trying again
    wait_state(core.state(), "counting down", |s| s.retry_in > 50).await;

    *fake.answer.lock().unwrap() = Answer::Busy;
    core.reconnect_now(false);
    let hello = next_of(&mut rx, "hello").await;
    assert!(hello.get("take").is_none());
    wait_state(core.state(), "busy", |s| s.error == LinkError::Busy && s.busy_with == "OTHER-PC").await;
}

#[tokio::test(flavor = "multi_thread")]
async fn no_speaker_at_that_address() {
    // a port nothing listens on
    let l = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    let host = l.local_addr().unwrap().to_string();
    drop(l);
    let (core, _) = core_for(&host, "x");
    wait_state(core.state(), "not found", |s| s.error == LinkError::NotFound && s.host.is_empty()).await;
}
