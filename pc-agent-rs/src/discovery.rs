//! Reach the MeowBox speaker at the IP the user typed (docs/firmware_v0.2.0.md, 2.2).
//!
//! No discovery on the LAN (no mDNS, no UDP broadcast): those get flagged or filtered on managed networks and PCs.
//! The user types the IP shown on the speaker's System screen; `find_speaker` checks the typed IP and the last
//! working one with GET /api/ping.

use crate::config::Config;
use serde_json::Value;
use std::net::IpAddr;
use std::time::Duration;

#[derive(Debug, Clone, PartialEq)]
pub struct Speaker {
    pub name: String,
    /// "ip" or "ip:port".
    pub host: String,
    pub ver: String,
    pub proto: i64,
    /// ping | last | manual
    pub how: String,
}

impl Speaker {
    pub fn ip(&self) -> &str {
        self.host.split(':').next().unwrap_or("")
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PingError {
    BadAddress,
    /// Something answers HTTP there, but it is not a MeowBox (or an old firmware).
    NotMeowbox,
    NoAnswer,
}

impl PingError {
    pub fn key(self) -> &'static str {
        match self {
            Self::BadAddress => "bad_address",
            Self::NotMeowbox => "not_meowbox",
            Self::NoAnswer => "no_answer",
        }
    }
}

/// HTTP client for the speaker: LAN only, so never through a proxy.
pub fn lan_client() -> reqwest::Client {
    reqwest::Client::builder().no_proxy().build().expect("http client")
}

/// GET http://host/api/ping: is it a MeowBox, and which one?
pub async fn ping_detail_with(host: &str, timeout: Duration) -> Result<Speaker, PingError> {
    let ip = host.split(':').next().unwrap_or("");
    if ip.parse::<IpAddr>().is_err() {
        return Err(PingError::BadAddress);
    }
    let r = lan_client()
        .get(format!("http://{host}/api/ping"))
        .timeout(timeout)
        .send()
        .await
        .map_err(|_| PingError::NoAnswer)?;
    if !r.status().is_success() {
        return Err(PingError::NotMeowbox);
    }
    let body = r.bytes().await.map_err(|_| PingError::NoAnswer)?;
    let j: Value = serde_json::from_slice(&body).map_err(|_| PingError::NotMeowbox)?;
    let name = j.get("name").and_then(Value::as_str).unwrap_or("");
    if !name.starts_with("MeowBox") {
        return Err(PingError::NotMeowbox);
    }
    let proto = match j.get("proto") {
        Some(Value::Number(n)) => n.as_i64().unwrap_or(0),
        Some(Value::String(s)) => s.trim().parse().unwrap_or(0),
        _ => 0,
    };
    Ok(Speaker {
        name: name.to_owned(),
        host: host.to_owned(),
        ver: j.get("ver").and_then(Value::as_str).unwrap_or("").to_owned(),
        proto,
        how: "ping".into(),
    })
}

/// Like `ping` but says why it failed.
pub async fn ping_detail(host: &str) -> Result<Speaker, PingError> {
    ping_detail_with(host, Duration::from_millis(2500)).await
}

pub async fn ping(host: &str) -> Option<Speaker> {
    ping_detail_with(host, Duration::from_secs(2)).await.ok()
}

/// The speaker at the typed IP, else at the last working one, if it answers (and has the name in `speaker`, if
/// set). The typed IP goes first so a new one wins over a stale last_host. `report(step)` says which IP is tried.
pub async fn find_speaker(cfg: &Config, report: impl Fn(&str)) -> Option<Speaker> {
    let want = cfg.speaker.as_str();
    let mut tried: Vec<&str> = Vec::new();
    for (h, how) in [(cfg.host.as_str(), "manual"), (cfg.last_host.as_str(), "last")] {
        if h.is_empty() || tried.contains(&h) {
            continue;
        }
        tried.push(h);
        report(&format!("ip {h}"));
        if let Some(mut sp) = ping(h).await
            && (want.is_empty() || sp.name == want)
        {
            sp.how = how.into();
            return Some(sp);
        }
    }
    None
}

/// IPv4 networks of this PC's interfaces (no loopback / link-local), as (address, prefix length).
pub fn local_networks() -> Vec<(std::net::Ipv4Addr, u8)> {
    let Ok(nets) = sysinfo::Networks::new_with_refreshed_list() else { return Vec::new() };
    let mut out = Vec::new();
    for data in nets.values() {
        for n in data.ip_networks() {
            if let IpAddr::V4(a) = n.addr
                && !a.is_loopback()
                && !a.is_link_local()
            {
                out.push((a, n.prefix));
            }
        }
    }
    out
}

fn in_network(ip: std::net::Ipv4Addr, net: std::net::Ipv4Addr, prefix: u8) -> bool {
    let mask = if prefix == 0 { 0 } else { u32::MAX << (32 - prefix.min(32)) };
    (u32::from(ip) & mask) == (u32::from(net) & mask)
}

pub fn same_subnet(host: &str) -> bool {
    let Ok(ip) = host.split(':').next().unwrap_or("").parse::<std::net::Ipv4Addr>() else {
        return false;
    };
    local_networks().into_iter().any(|(net, prefix)| in_network(ip, net, prefix))
}

/// Best guess why a typed IP does not answer: other_network | no_answer.
pub fn explain_failure(host: &str) -> &'static str {
    if !host.is_empty() && !same_subnet(host) { "other_network" } else { "no_answer" }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn subnet_math() {
        let a = |s: &str| s.parse::<std::net::Ipv4Addr>().unwrap();
        assert!(in_network(a("192.168.1.50"), a("192.168.1.7"), 24));
        assert!(!in_network(a("192.168.2.50"), a("192.168.1.7"), 24));
        assert!(in_network(a("10.1.2.3"), a("10.9.9.9"), 8));
    }

    #[tokio::test]
    async fn bad_address_is_refused_without_network() {
        assert_eq!(ping_detail("not-an-ip").await, Err(PingError::BadAddress));
        assert_eq!(ping_detail("").await, Err(PingError::BadAddress));
    }
}
