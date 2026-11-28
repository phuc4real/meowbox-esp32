//! YouTube relay: the speaker asks http://<pc>:8765/yt/stream?url=..., yt-dlp downloads the AAC audio and the relay
//! sends it as ADTS (m4a container stripped, no re-encoding, no ffmpeg). Live streams need ffmpeg (yt-dlp will not
//! download live HLS without it): the agent downloads an LGPL build the first time, and the AAC is only copied.
//!
//! Only the connected speaker may use it (docs/firmware_v0.2.0.md, 2.3): any other client gets 403, so a PC on a
//! shared Wi-Fi cannot be used by others to run yt-dlp/ffmpeg on arbitrary URLs. /yt/ping answers the speaker's
//! relay_test.

pub mod job;
pub mod mp4adts;
pub mod yt;

use crate::tools::{ffmpeg, ytdlp};
use axum::Router;
use axum::body::{Body, Bytes};
use axum::extract::{ConnectInfo, Query, State};
use axum::http::{HeaderName, HeaderValue, StatusCode};
use axum::response::{IntoResponse, Response};
use axum::routing::get;
use job::Job;
use mp4adts::Mp4ToAdts;
use serde_json::{Value, json};
use std::collections::{HashMap, HashSet};
use std::net::SocketAddr;
use std::path::PathBuf;
use std::process::Stdio;
use std::sync::Arc;
use std::time::Duration;
use tokio::io::AsyncReadExt;
use tokio::process::{ChildStdout, Command};
use tokio::sync::mpsc;

pub type Toast = Arc<dyn Fn(String) + Send + Sync>;

#[derive(Clone)]
pub struct Ctx {
    /// IPs that may use the relay: the connected speaker, plus one being tested by the connect wizard.
    pub allowed: Arc<dyn Fn() -> HashSet<String> + Send + Sync>,
    /// Short messages for the tray ("downloading ffmpeg").
    pub toast: Toast,
    cache: Arc<yt::InfoCache>,
}

impl Ctx {
    pub fn new(allowed: Arc<dyn Fn() -> HashSet<String> + Send + Sync>, toast: Toast) -> Self {
        Self { allowed, toast, cache: Arc::default() }
    }
}

/// An HTTP error with a short text body (what aiohttp's HTTPBadRequest(text=...) etc. sent).
#[derive(Debug)]
pub struct Fail(pub StatusCode, pub String);

impl IntoResponse for Fail {
    fn into_response(self) -> Response {
        (self.0, self.1).into_response()
    }
}

fn fail(code: StatusCode, text: impl Into<String>) -> Fail {
    Fail(code, text.into())
}

type Q = Query<HashMap<String, String>>;

fn guard(ctx: &Ctx, addr: SocketAddr, path: &str) -> Result<(), Fail> {
    let ip = addr.ip().to_string();
    if (ctx.allowed)().contains(&ip) {
        return Ok(());
    }
    tracing::warn!("relay: refused {ip} {path} (not the connected speaker)");
    Err(fail(StatusCode::FORBIDDEN, "MeowBox relay: only the connected speaker may use it"))
}

fn checked_url(q: &Q) -> Result<String, Fail> {
    let url = q.get("url").cloned().unwrap_or_default();
    if yt::check_url(&url) { Ok(url) } else { Err(fail(StatusCode::BAD_REQUEST, "url must be a YouTube http(s) link")) }
}

fn ytdlp_exe() -> Result<PathBuf, Fail> {
    ytdlp::exe().ok_or_else(|| {
        fail(
            StatusCode::SERVICE_UNAVAILABLE,
            "yt-dlp is not installed yet (the agent is downloading it), try again soon",
        )
    })
}

fn command(exe: &std::path::Path) -> Command {
    let mut c = Command::new(exe);
    c.stdin(Stdio::null()).creation_flags(crate::win::CREATE_NO_WINDOW).kill_on_drop(true);
    c
}

/// yt-dlp -J (cached 30 min).
async fn ytjson(ctx: &Ctx, url: &str, single: bool) -> Result<Value, Fail> {
    if !yt::check_url(url) {
        return Err(fail(StatusCode::BAD_REQUEST, "url must be a YouTube http(s) link"));
    }
    if let Some(v) = ctx.cache.get(url, single) {
        return Ok(v);
    }
    let exe = ytdlp_exe()?;
    let _in_use = ytdlp::InUse::new();
    let mut c = command(&exe);
    c.args(yt::info_args(url, single)).stdout(Stdio::piped()).stderr(Stdio::piped());
    let out = match tokio::time::timeout(Duration::from_secs(90), c.output()).await {
        Ok(Ok(o)) => o,
        Ok(Err(e)) => return Err(fail(StatusCode::BAD_GATEWAY, format!("cannot run yt-dlp: {e}"))),
        Err(_) => return Err(fail(StatusCode::GATEWAY_TIMEOUT, "yt-dlp took too long")),
    };
    if !out.status.success() {
        let err = String::from_utf8_lossy(&out.stderr);
        let err = err.trim();
        let tail: String = err.chars().rev().take(500).collect::<Vec<_>>().into_iter().rev().collect();
        tracing::warn!("yt-dlp failed for {url}: {tail}");
        return Err(fail(StatusCode::BAD_GATEWAY, "yt-dlp could not read this link (details in the agent log)"));
    }
    let v: Value = serde_json::from_slice(&out.stdout)
        .map_err(|_| fail(StatusCode::BAD_GATEWAY, "yt-dlp gave no readable answer"))?;
    ctx.cache.put(url, single, v.clone());
    Ok(v)
}

/// (full video info, (playlist index, playlist count)).
async fn resolve_video(ctx: &Ctx, url: &str, index: Option<usize>) -> Result<(Value, Option<(usize, usize)>), Fail> {
    let info = ytjson(ctx, url, index.is_none()).await?;
    if info.get("_type").and_then(Value::as_str) != Some("playlist") {
        return Ok((info, None));
    }
    let entries = info.get("entries").and_then(Value::as_array).cloned().unwrap_or_default();
    let i = index.unwrap_or(0);
    if i >= entries.len() {
        return Err(fail(StatusCode::NOT_FOUND, format!("index out of range (0..{})", entries.len() as i64 - 1)));
    }
    let v = ytjson(ctx, &yt::entry_url(&entries[i]), true).await?;
    Ok((v, Some((i, entries.len()))))
}

async fn yt_info(State(ctx): State<Ctx>, ConnectInfo(addr): ConnectInfo<SocketAddr>, q: Q) -> Result<Response, Fail> {
    guard(&ctx, addr, "/yt/info")?;
    let url = checked_url(&q)?;
    let info = ytjson(&ctx, &url, false).await?;
    let pick = |a: &str, b: &str| info.get(a).filter(|v| !v.is_null() && v != &&json!("")).or(info.get(b)).cloned();
    let typ = info.get("_type").cloned().unwrap_or(json!("video"));
    let mut res = json!({
        "type": typ, "title": pick("fulltitle", "title"), "duration": info.get("duration"),
        "is_live": info.get("is_live").and_then(Value::as_bool).unwrap_or(false), "uploader": pick("uploader", "channel"),
    });
    if typ == "playlist" {
        let entries: Vec<Value> = info
            .get("entries")
            .and_then(Value::as_array)
            .into_iter()
            .flatten()
            .enumerate()
            .map(|(i, e)| json!({"index": i, "title": e.get("title"), "duration": e.get("duration"), "url": yt::entry_url(e)}))
            .collect();
        res["entries"] = Value::Array(entries);
    }
    Ok(axum::Json(res).into_response())
}

async fn yt_ping(State(ctx): State<Ctx>, ConnectInfo(addr): ConnectInfo<SocketAddr>) -> Result<Response, Fail> {
    guard(&ctx, addr, "/yt/ping")?;
    Ok(axum::Json(json!({"ok": true})).into_response())
}

/// ffmpeg for a live stream: the one in PATH / already downloaded, else start the one-time download and wait a few
/// seconds (the speaker reconnects by itself, so a slow download just takes a few of its retries).
async fn need_ffmpeg(ctx: &Ctx) -> Option<PathBuf> {
    if let Some(p) = ffmpeg::path() {
        return Some(p);
    }
    if !ffmpeg::status().busy {
        (ctx.toast)("Đang tải ffmpeg cho YouTube live (một lần, khoảng 75 MB)…".into());
        let toast = ctx.toast.clone();
        ffmpeg::install_background(move || {
            toast(if ffmpeg::path().is_some() {
                "Đã tải xong ffmpeg: YouTube live phát được.".into()
            } else {
                format!("Không tải được ffmpeg: {}", ffmpeg::status().error)
            })
        });
    }
    for _ in 0..12 {
        tokio::time::sleep(Duration::from_millis(500)).await;
        if let Some(p) = ffmpeg::path() {
            return Some(p);
        }
    }
    None
}

/// Deletes the info JSON when the stream ends.
struct TempFile(PathBuf);

impl Drop for TempFile {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.0);
    }
}

fn temp_info(info: &Value) -> std::io::Result<TempFile> {
    let n: u64 = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_or(0, |d| d.as_nanos() as u64);
    let p = std::env::temp_dir().join(format!("meowbox-{}-{n:x}.info.json", std::process::id()));
    std::fs::write(&p, serde_json::to_vec(info)?)?;
    Ok(TempFile(p))
}

enum Convert {
    Copy,
    Adts(Box<Mp4ToAdts>),
}

impl Convert {
    fn feed(&mut self, chunk: &[u8]) -> Result<Vec<u8>, mp4adts::Unsupported> {
        match self {
            Self::Copy => Ok(chunk.to_vec()),
            Self::Adts(m) => m.feed(chunk),
        }
    }
}

/// What lives as long as one stream: the processes (job), the info file, the yt-dlp folder lock.
struct Stream {
    _job: Job,
    _info: TempFile,
    _in_use: ytdlp::InUse,
    _children: Vec<tokio::process::Child>,
    src: ChildStdout,
    convert: Convert,
    /// One read buffer for the whole stream.
    buf: Vec<u8>,
    title: String,
    how: String,
    sent: u64,
}

impl Stream {
    /// Next converted bytes; Ok(None) at the end.
    async fn next(&mut self) -> Result<Option<Vec<u8>>, mp4adts::Unsupported> {
        loop {
            let n = self.src.read(&mut self.buf).await.unwrap_or(0);
            if n == 0 {
                return Ok(None);
            }
            let out = self.convert.feed(&self.buf[..n])?;
            if !out.is_empty() {
                return Ok(Some(out));
            }
        }
    }
}

impl Drop for Stream {
    fn drop(&mut self) {
        tracing::info!("relay stop: {} ({}), {} KB sent", self.title, self.how, self.sent / 1024);
    }
}

fn header_value(v: &str) -> HeaderValue {
    // titles go out as raw UTF-8 bytes; control characters are not allowed in a header
    HeaderValue::from_bytes(v.as_bytes()).unwrap_or_else(|_| {
        let clean: String = v.chars().filter(|c| !c.is_control()).collect();
        HeaderValue::from_bytes(clean.as_bytes()).unwrap_or(HeaderValue::from_static(""))
    })
}

/// yt-dlp downloads (it handles YouTube's throttling, retries and link refresh) and writes to stdout; the relay only
/// changes the container. Video: m4a → ADTS, no ffmpeg. Live: yt-dlp + ffmpeg copy the AAC into MPEG-TS (ffmpeg
/// downloaded on first use). A video without AAC (rare): ffmpeg encodes AAC.
async fn yt_stream(State(ctx): State<Ctx>, ConnectInfo(addr): ConnectInfo<SocketAddr>, q: Q) -> Result<Response, Fail> {
    guard(&ctx, addr, "/yt/stream")?;
    let url = checked_url(&q)?;
    let index =
        q.get("index").filter(|s| !s.is_empty() && s.bytes().all(|b| b.is_ascii_digit())).and_then(|s| s.parse().ok());
    let (info, idx) = resolve_video(&ctx, &url, index).await?;
    let title = yt::title(&info);
    let live = info.get("is_live").and_then(Value::as_bool).unwrap_or(false);
    let fmt = if live { yt::pick_hls(&info) } else { yt::pick_aac(&info) };
    let ff = if live || fmt.is_none() {
        Some(need_ffmpeg(&ctx).await.ok_or_else(|| {
            fail(StatusCode::SERVICE_UNAVAILABLE, "downloading ffmpeg (first live stream), the speaker will retry")
        })?)
    } else {
        None
    };
    let exe = ytdlp_exe()?;
    let info_file = temp_info(&info).map_err(|e| fail(StatusCode::INTERNAL_SERVER_ERROR, e.to_string()))?;
    let job = Job::new().map_err(|e| fail(StatusCode::INTERNAL_SERVER_ERROR, e.to_string()))?;
    let in_use = ytdlp::InUse::new();
    let fmt_id = |f: &Value| match f.get("format_id") {
        Some(Value::String(s)) => s.clone(),
        Some(v) => v.to_string(),
        None => String::new(),
    };
    let load = info_file.0.to_string_lossy().into_owned();
    let spawn_err = |e: std::io::Error| fail(StatusCode::BAD_GATEWAY, format!("cannot run yt-dlp: {e}"));

    let mut yt = command(&exe);
    yt.args(yt::BASE_ARGS).stderr(Stdio::null());
    let (how, ctype, br, mut children, src, convert);
    if live {
        let id = fmt.as_ref().map(fmt_id).unwrap_or_else(|| "bestaudio/best".into());
        how =
            format!("live itag {}, AAC copied into MPEG-TS", fmt.as_ref().map(fmt_id).unwrap_or_else(|| "best".into()));
        let ff = ff.clone().unwrap_or_default();
        yt.arg("--ffmpeg-location")
            .arg(&ff)
            .args(["--load-info-json", &load, "-f", &id, "-o", "-"])
            .stdout(Stdio::piped());
        let mut c = yt.spawn().map_err(spawn_err)?;
        let _ = job.add(&c);
        src = c.stdout.take().expect("piped");
        children = vec![c];
        (ctype, br, convert) = ("video/mp2t", None, Convert::Copy);
    } else if let Some(f) = &fmt {
        let rate =
            crate::json::round(f.get("abr").and_then(Value::as_f64).filter(|a| *a > 0.0).unwrap_or(128.0)) as u32;
        how = format!("AAC itag {}, {rate} kbps", fmt_id(f));
        yt.args(["--load-info-json", &load, "-f", &fmt_id(f), "-o", "-"]).stdout(Stdio::piped());
        let mut c = yt.spawn().map_err(spawn_err)?;
        let _ = job.add(&c);
        src = c.stdout.take().expect("piped");
        children = vec![c];
        (ctype, br, convert) = ("audio/aac", Some(rate), Convert::Adts(Box::default()));
    } else {
        how = "no AAC: ffmpeg -> AAC 128k".into();
        yt.args(["--load-info-json", &load, "-f", "bestaudio/best", "-o", "-"]).stdout(Stdio::piped());
        let mut c = yt.spawn().map_err(spawn_err)?;
        let _ = job.add(&c);
        let pipe: Stdio = c.stdout.take().expect("piped").try_into().map_err(spawn_err)?;
        let mut enc = command(ff.as_deref().unwrap_or(std::path::Path::new("ffmpeg")));
        enc.args([
            "-hide_banner",
            "-loglevel",
            "error",
            "-i",
            "pipe:0",
            "-vn",
            "-c:a",
            "aac",
            "-b:a",
            "128k",
            "-ar",
            "44100",
        ])
        .args(["-ac", "2", "-f", "adts", "pipe:1"])
        .stdin(pipe)
        .stdout(Stdio::piped())
        .stderr(Stdio::null());
        let mut e = enc.spawn().map_err(|e| fail(StatusCode::BAD_GATEWAY, format!("cannot run ffmpeg: {e}")))?;
        let _ = job.add(&e);
        src = e.stdout.take().expect("piped");
        children = vec![c, e];
        (ctype, br, convert) = ("audio/aac", Some(128), Convert::Copy);
    }
    tracing::info!("relay start: {title} ({how}, {})", addr.ip());
    let headers = yt::headers(&info, &title, idx, ctype, br);
    let mut st = Stream {
        _job: job,
        _info: info_file,
        _in_use: in_use,
        _children: std::mem::take(&mut children),
        src,
        convert,
        buf: vec![0u8; 16384],
        title,
        how,
        sent: 0,
    };

    // headers go out with the first audio, so a failure before that is still an HTTP error
    let first = match st.next().await {
        Ok(Some(b)) => b,
        Ok(None) => return Err(fail(StatusCode::BAD_GATEWAY, "no audio from yt-dlp")),
        Err(e) => {
            tracing::warn!("relay: {e}");
            return Err(fail(StatusCode::BAD_GATEWAY, format!("cannot convert this video: {e}")));
        }
    };
    st.sent += first.len() as u64;
    let (tx, rx) = mpsc::channel::<Bytes>(8);
    let _ = tx.send(Bytes::from(first)).await;
    tokio::spawn(async move {
        loop {
            match st.next().await {
                Ok(Some(b)) => {
                    let n = b.len() as u64;
                    if tx.send(Bytes::from(b)).await.is_err() {
                        tracing::info!("relay client gone");
                        break;
                    }
                    st.sent += n;
                }
                Ok(None) => break,
                Err(e) => {
                    tracing::warn!("relay: {e}");
                    break;
                }
            }
        }
        drop(st); // kills what is left, logs "relay stop"
    });
    let body =
        futures_util::stream::unfold(
            rx,
            |mut rx| async move { rx.recv().await.map(|b| (Ok::<_, std::io::Error>(b), rx)) },
        );
    let mut resp = Response::new(Body::from_stream(body));
    for (k, v) in headers {
        if let Ok(name) = HeaderName::from_bytes(k.as_bytes()) {
            resp.headers_mut().insert(name, header_value(&v));
        }
    }
    Ok(resp)
}

pub fn router(ctx: Ctx) -> Router {
    Router::new()
        .route("/yt/info", get(yt_info))
        .route("/yt/stream", get(yt_stream))
        .route("/yt/ping", get(yt_ping))
        .with_state(ctx)
}

/// Bind 0.0.0.0:port and serve in the background. Err when the port is taken.
pub async fn start(port: u16, ctx: Ctx) -> std::io::Result<SocketAddr> {
    let listener = tokio::net::TcpListener::bind(("0.0.0.0", port)).await?;
    let addr = listener.local_addr()?;
    tokio::spawn(async move {
        let app = router(ctx).into_make_service_with_connect_info::<SocketAddr>();
        if let Err(e) = axum::serve(listener, app).await {
            tracing::error!("relay stopped: {e}");
        }
    });
    tracing::info!("YouTube relay on http://0.0.0.0:{port}/yt/stream?url=...");
    Ok(addr)
}
