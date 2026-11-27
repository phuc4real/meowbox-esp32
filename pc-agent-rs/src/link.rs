//! The core: the `/ws/pc` link to the speaker (stats, media and Meow Coding out; media commands in), with
//! reconnects. Runs on its own tokio runtime; the tray drives it through `CoreHandle`, the CLI `run` command runs it
//! on the main thread.
//!
//! Protocol (firmware/web/API.md, "/ws/pc protocol"): first message `hello` with the notify token; no welcome, so
//! the agent counts as connected unless `busy` / `auth` / `proto` comes back and the speaker closes.

use crate::coding::Tracker;
use crate::config::SharedConfig;
use crate::discovery::{Speaker, find_speaker};
use crate::json::{compact, parse_unique};
use crate::media::Media;
use crate::state::{LinkError, SharedState};
use crate::stats::Stats;
use futures_util::{SinkExt, StreamExt};
use serde_json::{Map, Value, json};
use std::collections::HashSet;
use std::future::Future;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};
use tokio::net::TcpStream;
use tokio::sync::{Notify, mpsc, watch};
use tokio_tungstenite::tungstenite::Message;

const HEARTBEAT: Duration = Duration::from_secs(15);
const CONNECT_TIMEOUT: Duration = Duration::from_secs(10);

#[derive(Debug, Clone, Copy)]
pub struct Options {
    /// Open the YouTube relay (phase 2).
    pub relay: bool,
    /// Keep yt-dlp installed and up to date (phase 2).
    pub ytdlp_update: bool,
}

impl Default for Options {
    fn default() -> Self {
        Self { relay: true, ytdlp_update: true }
    }
}

/// Everything the core shares with the UI.
#[derive(Clone)]
struct Shared {
    cfg: SharedConfig,
    state: SharedState,
    /// "Search now" / "Take over" / saved a new speaker: close the link or cut the wait short.
    kick: Arc<Notify>,
    /// The next hello carries "take": true.
    take: Arc<AtomicBool>,
    /// IPs the relay serves besides the connected speaker (the one the connect wizard is testing).
    extra_allowed: Arc<Mutex<HashSet<String>>>,
    coding: Arc<Tracker>,
    /// Toasts for the tray (set by the tray; the CLI only logs them).
    toast: Arc<Mutex<Option<crate::relay::Toast>>>,
}

impl Shared {
    fn new(cfg: SharedConfig, state: SharedState) -> Self {
        Self {
            cfg,
            state,
            kick: Arc::new(Notify::new()),
            take: Arc::new(AtomicBool::new(false)),
            extra_allowed: Arc::default(),
            coding: Arc::default(),
            toast: Arc::default(),
        }
    }

    fn toast(&self) -> crate::relay::Toast {
        let t = self.toast.clone();
        Arc::new(move |text: String| {
            tracing::info!("toast: {text}");
            if let Some(f) = t.lock().unwrap_or_else(|e| e.into_inner()).as_ref() {
                f(text);
            }
        })
    }

    /// The connected speaker's IP plus the one the connect wizard tests.
    fn allowed(&self) -> HashSet<String> {
        let mut ips = self.extra_allowed.lock().unwrap_or_else(|e| e.into_inner()).clone();
        let st = self.state.get();
        if st.connected && !st.ip().is_empty() {
            ips.insert(st.ip().to_owned());
        }
        ips
    }
}

pub struct CoreHandle {
    shared: Shared,
    stop: watch::Sender<bool>,
    rt: tokio::runtime::Handle,
    thread: Option<std::thread::JoinHandle<()>>,
}

fn runtime() -> tokio::runtime::Runtime {
    tokio::runtime::Builder::new_multi_thread()
        .worker_threads(2)
        .thread_name("meowbox-core")
        .on_thread_start(crate::win::com_init)
        .enable_all()
        .build()
        .expect("tokio runtime")
}

impl CoreHandle {
    /// Start the core in its own thread.
    pub fn start(cfg: SharedConfig, state: SharedState, opts: Options) -> Self {
        let shared = Shared::new(cfg, state);
        let (stop, stop_rx) = watch::channel(false);
        let (rt_tx, rt_rx) = std::sync::mpsc::channel();
        let sh = shared.clone();
        let thread = std::thread::Builder::new()
            .name("meowbox-core".into())
            .spawn(move || {
                crate::win::com_init();
                let rt = runtime();
                let _ = rt_tx.send(rt.handle().clone());
                rt.block_on(run(sh, opts, stop_rx));
                rt.shutdown_timeout(Duration::from_secs(2));
            })
            .expect("core thread");
        let rt = rt_rx.recv().expect("core runtime");
        Self { shared, stop, rt, thread: Some(thread) }
    }

    /// Skip the wait and search again (tray "Search now" / "Take over", or after the dialog saved a speaker).
    pub fn reconnect_now(&self, take: bool) {
        if take {
            self.shared.take.store(true, Ordering::SeqCst);
        }
        self.shared.kick.notify_waiters();
    }

    /// Where the core's short messages go (the tray's balloon).
    pub fn set_toast(&self, f: impl Fn(String) + Send + Sync + 'static) {
        *self.shared.toast.lock().unwrap_or_else(|e| e.into_inner()) = Some(Arc::new(f));
    }

    pub fn allow_ip(&self, ip: &str) {
        self.shared.extra_allowed.lock().unwrap_or_else(|e| e.into_inner()).insert(ip.to_owned());
    }

    pub fn clear_allowed(&self) {
        self.shared.extra_allowed.lock().unwrap_or_else(|e| e.into_inner()).clear();
    }

    pub fn state(&self) -> &SharedState {
        &self.shared.state
    }

    pub fn cfg(&self) -> &SharedConfig {
        &self.shared.cfg
    }

    /// Run a future on the core's runtime (network work for the UI).
    pub fn spawn<F: Future<Output = ()> + Send + 'static>(&self, f: F) {
        self.rt.spawn(f);
    }

    /// Call `f` (on a core thread) after every state change.
    pub fn on_state_change(&self, f: impl Fn() + Send + 'static) {
        let mut rx = self.shared.state.subscribe();
        self.rt.spawn(async move {
            while rx.changed().await.is_ok() {
                f();
            }
        });
    }

    pub fn stop(&mut self) {
        let _ = self.stop.send(true);
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
    }
}

impl Drop for CoreHandle {
    fn drop(&mut self) {
        self.stop();
    }
}

/// CLI `run`: the core on this thread until Ctrl+C.
pub fn run_forever(cfg: SharedConfig, state: SharedState, opts: Options) {
    crate::win::com_init();
    let shared = Shared::new(cfg, state);
    let (stop, stop_rx) = watch::channel(false);
    let rt = runtime();
    rt.block_on(async move {
        tokio::spawn(async move {
            let _ = tokio::signal::ctrl_c().await;
            let _ = stop.send(true);
        });
        run(shared, opts, stop_rx).await;
    });
    rt.shutdown_timeout(Duration::from_secs(2));
}

async fn run(sh: Shared, opts: Options, mut stop: watch::Receiver<bool>) {
    // Meow Coding: hooks written by an older agent are refreshed, then the local hook server
    let cport = sh.cfg.get().coding_port;
    if let Ok(Err(e)) = tokio::task::spawn_blocking(move || crate::coding::hooks::refresh(cport)).await {
        tracing::warn!("Meow Coding: could not update the Claude Code hooks: {e}");
    }
    let key = tokio::task::spawn_blocking(crate::coding::hooks::hook_key).await.unwrap_or_default();
    if let Err(e) = crate::coding::server::start(sh.coding.clone(), cport, key).await {
        tracing::error!("Meow Coding port {cport} busy: {e}");
    }
    let mut port = if opts.relay { sh.cfg.get().relay_port } else { 0 };
    if port > 0 {
        let s2 = sh.clone();
        let ctx = crate::relay::Ctx::new(Arc::new(move || s2.allowed()), sh.toast());
        match crate::relay::start(port, ctx).await {
            Ok(_) => {
                sh.state.update(|s| s.relay_port = port);
                if opts.ytdlp_update {
                    let daily = sh.cfg.get().auto_update;
                    tokio::spawn(crate::tools::ytdlp::maintain(daily));
                }
            }
            Err(e) => {
                tracing::error!("relay port {port} busy: {e}");
                sh.state.update(|s| {
                    s.error = LinkError::Port;
                    s.relay_port = 0;
                });
                port = 0;
            }
        }
    }
    tokio::select! {
        _ = link_loop(&sh, port) => {}
        _ = stop.wait_for(|s| *s) => {}
    }
    tracing::info!("core stopped");
}

/// Wait `secs`, counting down in the state; "Search now" cuts it short.
async fn sleep_with_countdown(sh: &Shared, secs: u64) {
    for left in (1..=secs).rev() {
        sh.state.update(|s| s.retry_in = left as u32);
        tokio::select! {
            _ = sh.kick.notified() => break,
            _ = tokio::time::sleep(Duration::from_secs(1)) => {}
        }
    }
    sh.state.update(|s| s.retry_in = 0);
}

async fn link_loop(sh: &Shared, relay_port: u16) {
    let stats = Arc::new(tokio::sync::Mutex::new(Stats::new()));
    let media = Arc::new(Media::default());
    let mut backoff: u64 = 1;
    loop {
        sh.state.update(|s| {
            s.connected = false;
            s.searching = "start".into();
        });
        let cfg = sh.cfg.get();
        let state = sh.state.clone();
        let sp = find_speaker(&cfg, |step| state.update(|s| s.searching = step.to_owned())).await;
        sh.state.update(|s| s.searching.clear());
        let mut wait = backoff;
        match sp {
            None => {
                sh.state.update(|s| {
                    s.error = LinkError::NotFound;
                    s.host.clear();
                });
                tracing::warn!("speaker {} not found, retry in {backoff}s", cfg.speaker);
            }
            Some(sp) => {
                sh.state.update(|s| {
                    s.host = sp.host.clone();
                    s.name = sp.name.clone();
                    s.ver = sp.ver.clone();
                    s.how = sp.how.clone();
                });
                if sp.proto < 1 {
                    tracing::info!("speaker {} has an old firmware (proto {})", sp.name, sp.proto);
                }
                match connect(&sp.host).await {
                    Ok((ws, local_ip)) => {
                        wait = session(sh, &sp, ws, &local_ip, relay_port, &stats, &media).await;
                        if wait == 0 {
                            backoff = 1;
                        }
                    }
                    Err(e) => {
                        tracing::warn!("connection to {} failed: {e}", sp.host);
                        sh.state.update(|s| s.error = LinkError::NotFound);
                    }
                }
                sh.state.update(|s| {
                    s.connected = false;
                    s.reconnects += 1;
                });
            }
        }
        sleep_with_countdown(sh, if wait == 0 { backoff } else { wait }).await;
        backoff = (backoff * 2).min(30);
    }
}

type Ws = tokio_tungstenite::WebSocketStream<TcpStream>;

/// TCP + WebSocket handshake to ws://host/ws/pc; also returns the local IP of this socket (the relay address the
/// speaker can reach). No Origin header: the speaker refuses upgrades that carry one (web pages).
async fn connect(host: &str) -> anyhow::Result<(Ws, String)> {
    let addr = if host.contains(':') { host.to_owned() } else { format!("{host}:80") };
    let tcp = tokio::time::timeout(CONNECT_TIMEOUT, TcpStream::connect(&addr)).await??;
    let _ = tcp.set_nodelay(true);
    let local_ip = tcp.local_addr()?.ip().to_string();
    let (ws, _) =
        tokio::time::timeout(CONNECT_TIMEOUT, tokio_tungstenite::client_async(format!("ws://{host}/ws/pc"), tcp))
            .await??;
    Ok((ws, local_ip))
}

/// After a successful connection: keep the IP for next time and fill the speaker name if empty.
fn remember(cfg: &SharedConfig, sp: &Speaker) {
    cfg.update(|c| {
        if c.last_host != sp.host {
            c.last_host = sp.host.clone();
        }
        if c.speaker.is_empty() {
            c.speaker = sp.name.clone();
        }
    });
}

pub fn hello(token: &str, relay: Option<String>, take: bool) -> Value {
    let mut h = Map::new();
    h.insert("t".into(), json!("hello"));
    h.insert("name".into(), json!(crate::win::pc_name()));
    h.insert("ver".into(), json!(crate::VERSION));
    h.insert("proto".into(), json!(crate::PROTO));
    h.insert("token".into(), json!(token));
    if let Some(r) = relay {
        h.insert("relay".into(), json!(r));
    }
    if take {
        h.insert("take".into(), json!(true));
    }
    Value::Object(h)
}

/// What the speaker told us, after refusing repeated keys and anything that is not an object.
#[derive(Debug, PartialEq)]
pub enum Incoming {
    Busy(String),
    Auth,
    Proto(i64),
    MediaCmd(String),
    Other,
}

pub fn parse_incoming(text: &str) -> Option<Incoming> {
    let v = parse_unique(text).ok()?;
    let o = v.as_object()?;
    let s = |k: &str| o.get(k).and_then(Value::as_str).unwrap_or("").to_owned();
    Some(match o.get("t").and_then(Value::as_str) {
        Some("busy") => Incoming::Busy(s("pc")),
        Some("auth") => Incoming::Auth,
        Some("proto") => Incoming::Proto(o.get("need").and_then(Value::as_i64).unwrap_or(0)),
        Some("media_cmd") => Incoming::MediaCmd(s("cmd")),
        _ => Incoming::Other,
    })
}

/// One connection. Returns seconds to wait before the next attempt (0 = the normal reconnect backoff).
async fn session(
    sh: &Shared,
    sp: &Speaker,
    ws: Ws,
    local_ip: &str,
    relay_port: u16,
    stats: &Arc<tokio::sync::Mutex<Stats>>,
    media: &Arc<Media>,
) -> u64 {
    let (mut sink, mut stream) = ws.split();
    let relay = (relay_port > 0).then(|| format!("http://{local_ip}:{relay_port}"));
    let take = sh.take.swap(false, Ordering::SeqCst);
    let cfg = sh.cfg.get();
    if let Err(e) = sink.send(Message::text(compact(&hello(&cfg.token, relay, take)))).await {
        tracing::warn!("connection to {} failed: {e}", sp.host);
        return 0;
    }
    // no "welcome" message: connected unless a busy/auth/proto answer comes back right away
    sh.state.update(|s| {
        s.connected = true;
        s.error = LinkError::None;
        s.busy_with.clear();
    });
    remember(&sh.cfg, sp);
    tracing::info!("connected to {} ({}, {})", sp.name, sp.host, sp.how);

    let (tx, mut rx) = mpsc::channel::<Message>(32);
    let writer = tokio::spawn(async move {
        while let Some(m) = rx.recv().await {
            let close = matches!(m, Message::Close(_));
            if sink.send(m).await.is_err() || close {
                break;
            }
        }
    });
    let sender = tokio::spawn(sender(sh.clone(), tx.clone(), stats.clone(), media.clone()));

    let mut hb = tokio::time::interval(HEARTBEAT);
    hb.tick().await;
    let mut waiting_pong = false;
    let wait = loop {
        tokio::select! {
            _ = sh.kick.notified() => {
                let _ = tx.send(Message::Close(None)).await;
                break 0;
            }
            _ = hb.tick() => {
                if waiting_pong {
                    tracing::warn!("no answer from {} for {}s, reconnecting", sp.host, HEARTBEAT.as_secs());
                    break 0;
                }
                waiting_pong = true;
                if tx.send(Message::Ping(Default::default())).await.is_err() {
                    break 0;
                }
            }
            msg = stream.next() => {
                let Some(Ok(msg)) = msg else { break 0 };
                waiting_pong = false; // any frame proves the link is alive
                let Message::Text(text) = msg else {
                    if matches!(msg, Message::Close(_)) { break 0 }
                    continue;
                };
                match parse_incoming(&text) {
                    Some(Incoming::Busy(pc)) => {
                        // another PC holds the speaker (or just took it over from us)
                        tracing::info!("speaker busy with {pc}");
                        sh.state.update(|s| { s.connected = false; s.error = LinkError::Busy; s.busy_with = pc; });
                        break 60;
                    }
                    Some(Incoming::Auth) => {
                        tracing::warn!("speaker refused the token: open 'Connect speaker' and enter it");
                        sh.state.update(|s| { s.connected = false; s.error = LinkError::Token; });
                        break 60;
                    }
                    Some(Incoming::Proto(need)) => {
                        tracing::warn!("speaker needs agent protocol {need} (have {})", crate::PROTO);
                        sh.state.update(|s| { s.connected = false; s.error = LinkError::ProtoAgent; });
                        break 300;
                    }
                    Some(Incoming::MediaCmd(cmd)) => {
                        media.command(&cmd).await;
                        let m = media.read().await;
                        if tx.send(Message::text(compact(&m))).await.is_err() {
                            break 0;
                        }
                    }
                    Some(Incoming::Other) | None => {}
                }
            }
        }
    };
    sender.abort();
    drop(tx);
    let _ = tokio::time::timeout(Duration::from_secs(1), writer).await;
    wait
}

/// Every second: Meow Coding when changed (or every 30 s), stats unless paused, media when changed (or every 5 s).
async fn sender(sh: Shared, tx: mpsc::Sender<Message>, stats: Arc<tokio::sync::Mutex<Stats>>, media: Arc<Media>) {
    let mut media_last: Option<Value> = None;
    let mut media_t = Instant::now();
    let mut code_last: Option<(Vec<Value>, Map<String, Value>)> = None;
    let mut code_t = Instant::now();
    let send = |v: Value| {
        let tx = tx.clone();
        async move { tx.send(Message::text(compact(&v))).await.is_ok() }
    };
    loop {
        let coding = sh.coding.clone();
        let _ = tokio::task::spawn_blocking(move || coding.refresh_usage()).await;
        let cs = sh.coding.snapshot();
        let lim = sh.coding.limits_msg();
        let key_s: Vec<Value> = cs
            .iter()
            .map(|x| {
                let mut x = x.clone();
                if let Some(o) = x.as_object_mut() {
                    o.remove("for");
                }
                x
            })
            .collect();
        let ckey = (key_s, lim.clone());
        if code_last.as_ref() != Some(&ckey) || code_t.elapsed() >= Duration::from_secs(30) {
            let mut msg = json!({"t": "coding", "s": cs});
            if !lim.is_empty() {
                msg["lim"] = Value::Object(lim);
            }
            if !send(msg).await {
                return;
            }
            code_last = Some(ckey);
            code_t = Instant::now();
        }
        if !sh.cfg.get().paused {
            let m = stats.lock().await.sample().await;
            if !send(m).await {
                return;
            }
        }
        let m = media.read().await;
        let mut key = m.clone();
        if let Some(o) = key.as_object_mut() {
            o.remove("pos");
        }
        if media_last.as_ref() != Some(&key) || media_t.elapsed() >= Duration::from_secs(5) {
            if !send(m).await {
                return;
            }
            media_last = Some(key);
            media_t = Instant::now();
        }
        tokio::time::sleep(Duration::from_secs(1)).await;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn incoming_messages() {
        assert_eq!(parse_incoming(r#"{"t":"media_cmd","cmd":"next"}"#), Some(Incoming::MediaCmd("next".into())));
        assert_eq!(parse_incoming(r#"{"t":"busy","pc":"OTHER"}"#), Some(Incoming::Busy("OTHER".into())));
        assert_eq!(parse_incoming(r#"{"t":"proto","need":3,"have":2}"#), Some(Incoming::Proto(3)));
        assert_eq!(parse_incoming(r#"{"t":"auth"}"#), Some(Incoming::Auth));
        assert_eq!(parse_incoming(r#"{"t":"busy","t":"media_cmd","cmd":"next"}"#), None, "repeated key");
        assert_eq!(parse_incoming(r#"["t","busy"]"#), None);
        assert_eq!(parse_incoming("not json"), None);
        assert_eq!(parse_incoming(r#"{"t":"stats"}"#), Some(Incoming::Other));
    }

    #[test]
    fn hello_shape() {
        let h = hello("tok", Some("http://192.168.1.10:8765".into()), true);
        assert_eq!(h["t"], "hello");
        assert_eq!(h["proto"], 2);
        assert_eq!(h["token"], "tok");
        assert_eq!(h["relay"], "http://192.168.1.10:8765");
        assert_eq!(h["take"], true);
        let h = hello("", None, false);
        assert!(h.get("relay").is_none() && h.get("take").is_none());
    }
}
