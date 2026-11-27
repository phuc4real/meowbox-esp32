//! Network facts for the connect wizard: which local IP talks to the speaker, that adapter's network category
//! (Public / Private) from the Network List Manager, the Wi-Fi name, and the firewall rule for the relay.

use std::net::{Ipv4Addr, UdpSocket};
use std::os::windows::process::CommandExt;
use windows::Win32::NetworkManagement::IpHelper::{
    GAA_FLAG_SKIP_ANYCAST, GAA_FLAG_SKIP_DNS_SERVER, GAA_FLAG_SKIP_MULTICAST, GetAdaptersAddresses,
    IP_ADAPTER_ADDRESSES_LH,
};
use windows::Win32::Networking::NetworkListManager::{INetworkConnection, INetworkListManager, NetworkListManager};
use windows::Win32::Networking::WinSock::{AF_INET, SOCKADDR_IN};
use windows::Win32::System::Com::{CLSCTX_ALL, CoCreateInstance};

const IF_TYPE_IEEE80211: u32 = 71;

/// The local IP this PC uses to reach `ip` ("" if none): a UDP "connect" sends nothing, it only picks the route.
pub fn local_ip_towards(ip: &str) -> String {
    let ip = ip.split(':').next().unwrap_or("");
    UdpSocket::bind("0.0.0.0:0")
        .and_then(|s| s.connect((ip, 80)).map(|_| s))
        .and_then(|s| s.local_addr())
        .map(|a| a.ip().to_string())
        .unwrap_or_default()
}

struct Adapter {
    /// "{GUID}" as in the registry and the Network List Manager.
    guid: String,
    alias: String,
    wifi: bool,
    ipv4: Vec<Ipv4Addr>,
}

fn adapters() -> Vec<Adapter> {
    let flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    let mut size: u32 = 16 * 1024;
    let mut buf: Vec<u64> = Vec::new(); // u64 for alignment
    for _ in 0..3 {
        buf.resize((size as usize).div_ceil(8), 0);
        // SAFETY: buf holds `size` bytes; the API writes a linked list inside it.
        let r =
            unsafe { GetAdaptersAddresses(AF_INET.0 as u32, flags, None, Some(buf.as_mut_ptr() as *mut _), &mut size) };
        if r == 0 {
            break;
        }
        if r != 111 {
            // not ERROR_BUFFER_OVERFLOW
            return Vec::new();
        }
    }
    let mut out = Vec::new();
    let mut p = buf.as_ptr() as *const IP_ADAPTER_ADDRESSES_LH;
    // SAFETY: walking the list the API filled; every pointer points into `buf` or is null.
    unsafe {
        while !p.is_null() {
            let a = &*p;
            let mut ipv4 = Vec::new();
            let mut u = a.FirstUnicastAddress;
            while !u.is_null() {
                let sa = (*u).Address.lpSockaddr;
                if !sa.is_null() && (*sa).sa_family == AF_INET {
                    let sin = &*(sa as *const SOCKADDR_IN);
                    ipv4.push(Ipv4Addr::from(u32::from_be(sin.sin_addr.S_un.S_addr)));
                }
                u = (*u).Next;
            }
            let alias = a.FriendlyName.to_string().unwrap_or_default();
            out.push(Adapter {
                guid: a.AdapterName.to_string().unwrap_or_default(),
                wifi: a.IfType == IF_TYPE_IEEE80211,
                alias,
                ipv4,
            });
            p = a.Next;
        }
    }
    out
}

/// (adapter "{GUID}", network name, category) of each connected network.
fn nlm_connections() -> Vec<(String, String, &'static str)> {
    super::com_init();
    let r: windows::core::Result<Vec<_>> = (|| {
        // SAFETY: COM calls on an initialised thread; interfaces are released when dropped.
        unsafe {
            let nlm: INetworkListManager = CoCreateInstance(&NetworkListManager, None, CLSCTX_ALL)?;
            let conns = nlm.GetNetworkConnections()?;
            let mut out = Vec::new();
            loop {
                let mut one: [Option<INetworkConnection>; 1] = [None];
                let mut fetched = 0u32;
                if conns.Next(&mut one, Some(&mut fetched)).is_err() || fetched == 0 {
                    break;
                }
                let Some(conn) = one[0].take() else { break };
                let guid = format!("{{{:?}}}", conn.GetAdapterId()?);
                let net = conn.GetNetwork()?;
                let cat = match net.GetCategory()?.0 {
                    0 => "Public",
                    1 => "Private",
                    2 => "DomainAuthenticated",
                    _ => "",
                };
                out.push((guid, net.GetName()?.to_string(), cat));
            }
            Ok(out)
        }
    })();
    r.unwrap_or_else(|e| {
        tracing::debug!("network list query failed: {e}");
        Vec::new()
    })
}

/// Public | Private | DomainAuthenticated for the adapter that has `local_ip` ("" if unknown).
pub fn network_category(local_ip: &str) -> String {
    let Ok(ip) = local_ip.parse::<Ipv4Addr>() else { return String::new() };
    let guids: Vec<String> =
        adapters().into_iter().filter(|a| a.ipv4.contains(&ip)).map(|a| a.guid.to_uppercase()).collect();
    nlm_connections()
        .into_iter()
        .find(|(g, _, _)| guids.contains(&g.to_uppercase()))
        .map(|(_, _, c)| c.to_owned())
        .unwrap_or_default()
}

/// Name of the Wi-Fi this PC is on. netsh needs the Location permission on Windows 11, so fall back to the network
/// profile name of the Wi-Fi adapter (Windows names the profile after the SSID).
pub fn wifi_name() -> String {
    if let Ok(out) = std::process::Command::new("netsh")
        .args(["wlan", "show", "interfaces"])
        .creation_flags(super::CREATE_NO_WINDOW)
        .output()
    {
        let text = String::from_utf8_lossy(&out.stdout);
        for line in text.lines() {
            if let Some((k, v)) = line.split_once(':')
                && k.trim() == "SSID"
                && !v.trim().is_empty()
            {
                return v.trim().to_owned();
            }
        }
    }
    let wifi: Vec<String> =
        adapters().into_iter().filter(|a| a.wifi || is_wifi_alias(&a.alias)).map(|a| a.guid.to_uppercase()).collect();
    nlm_connections()
        .into_iter()
        .find(|(g, _, _)| wifi.contains(&g.to_uppercase()))
        .map(|(_, n, _)| n)
        .unwrap_or_default()
}

fn is_wifi_alias(alias: &str) -> bool {
    let a = alias.to_lowercase();
    a.contains("wi-fi") || a.contains("wifi") || a.contains("wlan") || a.contains("wireless")
}

/// Firewall rule for the relay on one profile (Public/Private), via an elevated netsh (UAC prompt). Only offered
/// when relay_test failed and that profile has no rule (docs/firmware_v0.2.0.md, 2.2 step 3).
pub fn allow_relay_on_this_network(port: u16, profile: &str) -> bool {
    let exe = super::tray_exe();
    let args = format!(
        "advfirewall firewall add rule name=\"MeowBox Agent relay ({profile})\" dir=in action=allow protocol=TCP \
         localport={port} profile={} program=\"{}\"",
        profile.to_lowercase(),
        exe.display()
    );
    super::run_elevated("netsh.exe", &args)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn local_ip_and_category_of_this_pc() {
        let ip = local_ip_towards("8.8.8.8");
        if ip.is_empty() {
            return; // offline machine
        }
        assert!(ip.parse::<Ipv4Addr>().is_ok());
        let cat = network_category(&ip);
        assert!(["Public", "Private", "DomainAuthenticated", ""].contains(&cat.as_str()), "{cat}");
        assert!(adapters().iter().any(|a| a.ipv4.iter().any(|x| x.to_string() == ip)));
    }

    #[test]
    fn wifi_aliases() {
        assert!(is_wifi_alias("Wi-Fi 2"));
        assert!(is_wifi_alias("WLAN"));
        assert!(!is_wifi_alias("Ethernet"));
    }
}
