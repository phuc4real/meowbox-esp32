//! HTTP to the speaker: notify, say (audio), wifi-setup, token check, relay test.

use std::time::Duration;

/// GET (body None) or POST to http://host/path with the notify token. Returns (status, body text); an HTTP error
/// status is a normal result, only a network failure is an Err.
pub async fn request(
    host: &str,
    path: &str,
    token: &str,
    body: Option<(Vec<u8>, &str)>,
    timeout: Duration,
) -> reqwest::Result<(u16, String)> {
    let client = crate::discovery::lan_client();
    let url = format!("http://{host}{path}");
    let mut req = match body {
        Some((bytes, ctype)) => client.post(url).header("Content-Type", ctype).body(bytes),
        None => client.get(url).header("Content-Type", "application/json"),
    };
    if !token.is_empty() {
        req = req.bearer_auth(token);
    }
    let r = req.timeout(timeout).send().await?;
    let code = r.status().as_u16();
    let bytes = r.bytes().await?;
    Ok((code, String::from_utf8_lossy(&bytes).into_owned()))
}

/// Same with the Python agent's 15 s timeout.
pub async fn speaker_request(
    host: &str,
    path: &str,
    token: &str,
    body: Option<(Vec<u8>, &str)>,
) -> reqwest::Result<(u16, String)> {
    request(host, path, token, body, Duration::from_secs(15)).await
}

/// POST /api/notify with JSON.
pub async fn notify(host: &str, token: &str, body: &serde_json::Value) -> reqwest::Result<(u16, String)> {
    speaker_request(host, "/api/notify", token, Some((body.to_string().into_bytes(), "application/json"))).await
}
