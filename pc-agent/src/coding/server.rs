//! The local HTTP endpoint for Claude Code's hooks (/hook) and status line (/status), on 127.0.0.1 only.

use super::Tracker;
use super::hooks::HOOK_HEADER;
use axum::Router;
use axum::body::Bytes;
use axum::extract::{ConnectInfo, DefaultBodyLimit, State};
use axum::http::{HeaderMap, StatusCode};
use axum::response::{IntoResponse, Response};
use axum::routing::post;
use serde_json::{Value, json};
use std::net::SocketAddr;
use std::sync::Arc;

#[derive(Clone)]
struct Ctx {
    tracker: Arc<Tracker>,
    key: Arc<String>,
}

fn ct_eq(a: &[u8], b: &[u8]) -> bool {
    a.len() == b.len() && a.iter().zip(b).fold(0u8, |acc, (x, y)| acc | (x ^ y)) == 0
}

/// Loopback, our key, JSON, and no Origin (browsers always send one on a cross-site POST).
fn from_our_hook(addr: SocketAddr, h: &HeaderMap, key: &str) -> bool {
    let ctype = h.get("content-type").and_then(|v| v.to_str().ok()).unwrap_or("");
    let mime = ctype.split(';').next().unwrap_or("").trim();
    addr.ip().is_loopback()
        && !h.contains_key("origin")
        && mime.eq_ignore_ascii_case("application/json")
        && ct_eq(h.get(HOOK_HEADER).map(|v| v.as_bytes()).unwrap_or(b""), key.as_bytes())
}

async fn hook(State(c): State<Ctx>, ConnectInfo(addr): ConnectInfo<SocketAddr>, h: HeaderMap, body: Bytes) -> Response {
    if !from_our_hook(addr, &h, &c.key) {
        return StatusCode::FORBIDDEN.into_response();
    }
    let Ok(ev) = serde_json::from_slice::<Value>(&body) else { return StatusCode::BAD_REQUEST.into_response() };
    if ev.is_object() {
        c.tracker.event(&ev);
    }
    axum::Json(json!({})).into_response() // empty answer: the hook changes nothing in Claude Code
}

async fn status(
    State(c): State<Ctx>,
    ConnectInfo(addr): ConnectInfo<SocketAddr>,
    h: HeaderMap,
    body: Bytes,
) -> Response {
    if !from_our_hook(addr, &h, &c.key) {
        return StatusCode::FORBIDDEN.into_response();
    }
    let body: &[u8] = if body.is_empty() { b"{}" } else { &body };
    let Ok(ev) = serde_json::from_slice::<Value>(body) else { return StatusCode::BAD_REQUEST.into_response() };
    let ev = if ev.is_object() { ev } else { json!({}) };
    c.tracker.status(&ev).into_response()
}

pub fn router(tracker: Arc<Tracker>, key: String) -> Router {
    Router::new()
        .route("/hook", post(hook))
        .route("/status", post(status))
        .layer(DefaultBodyLimit::max(4 * 1024 * 1024)) // PostToolUse carries the tool output
        .with_state(Ctx { tracker, key: Arc::new(key) })
}

/// Bind 127.0.0.1:port and serve in the background. Err when the port is taken.
pub async fn start(tracker: Arc<Tracker>, port: u16, key: String) -> std::io::Result<SocketAddr> {
    let listener = tokio::net::TcpListener::bind(("127.0.0.1", port)).await?;
    let addr = listener.local_addr()?;
    tokio::spawn(async move {
        let app = router(tracker, key).into_make_service_with_connect_info::<SocketAddr>();
        if let Err(e) = axum::serve(listener, app).await {
            tracing::error!("Meow Coding server stopped: {e}");
        }
    });
    tracing::info!("Meow Coding: Claude Code hooks on http://127.0.0.1:{}/hook", addr.port());
    Ok(addr)
}
