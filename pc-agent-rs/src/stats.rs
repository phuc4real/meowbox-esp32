//! PC stats for the speaker's stats tile: CPU, RAM, disk, network from sysinfo; GPU load and temperatures from
//! nvidia-smi and/or LibreHardwareMonitor's web server (LHM wins when both give a value).

use crate::json::round;
use serde_json::{Map, Value, json};
use std::path::PathBuf;
use std::time::{Duration, Instant};
use sysinfo::{CpuRefreshKind, Disks, MemoryRefreshKind, Networks, RefreshKind, System};

/// sysinfo parts are Options: a failed probe just leaves those fields at 0.
pub struct Stats {
    sys: Option<System>,
    nets: Option<Networks>,
    disks: Option<Disks>,
    disk_mount: String,
    sent: u64,
    recv: u64,
    t: Instant,
    lhm_retry: Option<Instant>,
    nvsmi: Option<PathBuf>,
    http: reqwest::Client,
}

fn net_totals(nets: &Option<Networks>) -> (u64, u64) {
    nets.iter()
        .flat_map(|n| n.iter())
        .fold((0, 0), |(s, r), (_, d)| (s + d.total_transmitted(), r + d.total_received()))
}

/// First `name.exe` on PATH (like shutil.which).
pub fn which(name: &str) -> Option<PathBuf> {
    let path = std::env::var_os("PATH")?;
    std::env::split_paths(&path).map(|d| d.join(format!("{name}.exe"))).find(|p| p.is_file())
}

impl Default for Stats {
    fn default() -> Self {
        Self::new()
    }
}

impl Stats {
    pub fn new() -> Self {
        let sys = System::new_with_specifics(
            RefreshKind::nothing()
                .with_cpu(CpuRefreshKind::nothing().with_usage())
                .with_memory(MemoryRefreshKind::nothing().with_ram()),
        )
        .ok();
        let nets = Networks::new_with_refreshed_list().ok();
        let (sent, recv) = net_totals(&nets);
        let drive = std::env::var("SystemDrive").unwrap_or_else(|_| "C:".into());
        Self {
            sys,
            nets,
            disks: Disks::new_with_refreshed_list().ok(),
            disk_mount: format!("{drive}\\"),
            sent,
            recv,
            t: Instant::now(),
            lhm_retry: None,
            nvsmi: which("nvidia-smi"),
            http: crate::discovery::lan_client(),
        }
    }

    /// LibreHardwareMonitor's web server (Options > Remote Web Server, port 8085); after a failure, wait 30 s.
    async fn lhm(&mut self) -> Map<String, Value> {
        if self.lhm_retry.is_some_and(|t| Instant::now() < t) {
            return Map::new();
        }
        let r = async {
            let r = self
                .http
                .get("http://localhost:8085/data.json")
                .timeout(Duration::from_millis(500))
                .send()
                .await
                .ok()?;
            let v: Value = serde_json::from_slice(&r.bytes().await.ok()?).ok()?;
            Some(parse_lhm(&v))
        }
        .await;
        match r {
            Some(m) => m,
            None => {
                self.lhm_retry = Some(Instant::now() + Duration::from_secs(30));
                Map::new()
            }
        }
    }

    async fn nvidia(&self) -> Map<String, Value> {
        let Some(exe) = &self.nvsmi else { return Map::new() };
        let mut cmd = tokio::process::Command::new(exe);
        cmd.args(["--query-gpu=utilization.gpu,temperature.gpu", "--format=csv,noheader,nounits"])
            .stdin(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .kill_on_drop(true)
            .creation_flags(crate::win::CREATE_NO_WINDOW);
        let Ok(Ok(out)) = tokio::time::timeout(Duration::from_secs(3), cmd.output()).await else {
            return Map::new();
        };
        let text = String::from_utf8_lossy(&out.stdout);
        let mut vals = text.lines().next().unwrap_or("").split(',').map(|x| x.trim().parse::<f64>());
        match (vals.next(), vals.next()) {
            (Some(Ok(load)), Some(Ok(temp))) => {
                let mut m = Map::new();
                m.insert("gpu".into(), json!(load));
                m.insert("gpu_temp".into(), json!(temp));
                m
            }
            _ => Map::new(),
        }
    }

    /// One `stats` message; rates are per second since the previous call.
    pub async fn sample(&mut self) -> Value {
        let (mut cpu, mut ram) = (0.0, 0.0);
        if let Some(sys) = self.sys.as_mut() {
            sys.refresh_cpu_usage();
            sys.refresh_memory_specifics(MemoryRefreshKind::nothing().with_ram());
            cpu = sys.global_cpu_usage() as f64;
            let total = sys.total_memory().max(1) as f64;
            ram = (total - sys.available_memory() as f64) * 100.0 / total;
        }
        if let Some(n) = self.nets.as_mut() {
            n.refresh(true);
        }
        if let Some(d) = self.disks.as_mut() {
            d.refresh(true);
        }
        let now = Instant::now();
        let dt = now.duration_since(self.t).as_secs_f64().max(1e-3);
        let (sent, recv) = net_totals(&self.nets);
        let disk = self
            .disks
            .iter()
            .flat_map(|d| d.iter())
            .find(|d| d.mount_point().to_string_lossy().eq_ignore_ascii_case(&self.disk_mount))
            .map(|d| {
                let t = d.total_space().max(1) as f64;
                (t - d.available_space() as f64) * 100.0 / t
            })
            .unwrap_or(0.0);
        let mut msg = Map::new();
        msg.insert("t".into(), json!("stats"));
        msg.insert("cpu".into(), json!(round(cpu)));
        msg.insert("ram".into(), json!(round(ram)));
        msg.insert("gpu".into(), Value::Null);
        msg.insert("cpu_temp".into(), Value::Null);
        msg.insert("gpu_temp".into(), Value::Null);
        msg.insert("net_up".into(), json!(round(sent.saturating_sub(self.sent) as f64 / 1024.0 / dt)));
        msg.insert("net_down".into(), json!(round(recv.saturating_sub(self.recv) as f64 / 1024.0 / dt)));
        msg.insert("disk".into(), json!(round(disk)));
        (self.sent, self.recv, self.t) = (sent, recv, now);
        let nv = self.nvidia().await;
        let lhm = self.lhm().await;
        for extra in [nv, lhm] {
            for (k, v) in extra {
                if let Some(x) = v.as_f64() {
                    msg.insert(k, json!(round(x)));
                }
            }
        }
        Value::Object(msg)
    }
}

/// LibreHardwareMonitor data.json → cpu_temp, gpu_temp, gpu load (null where not found), like the Python agent.
pub fn parse_lhm(root: &Value) -> Map<String, Value> {
    // (hardware kind, group, sensor name, value)
    let mut sensors: Vec<(Option<&str>, Option<&str>, String, f64)> = Vec::new();

    fn number_prefix(s: &str) -> Option<f64> {
        let end = s
            .char_indices()
            .take_while(|&(i, c)| c.is_ascii_digit() || c == '.' || c == ',' || (i == 0 && c == '-'))
            .last()
            .map(|(i, c)| i + c.len_utf8())?;
        s[..end].replace(',', ".").parse().ok()
    }

    fn walk<'a>(
        node: &'a Value,
        mut hw: Option<&'a str>,
        mut group: Option<&'a str>,
        out: &mut Vec<(Option<&'a str>, Option<&'a str>, String, f64)>,
    ) {
        let img = node.get("ImageURL").and_then(Value::as_str).unwrap_or("");
        let text = node.get("Text").and_then(Value::as_str).unwrap_or("");
        let kids = node.get("Children").and_then(Value::as_array).map(Vec::as_slice).unwrap_or(&[]);
        let value = node.get("Value").and_then(Value::as_str).unwrap_or("");
        if kids.is_empty() && !value.is_empty() {
            if let Some(v) = number_prefix(value) {
                out.push((hw, group, text.to_owned(), v));
            }
            return;
        }
        if img.contains("temperature") {
            group = Some("temp");
        } else if img.contains("load") {
            group = Some("load");
        } else if img.contains("cpu") {
            hw = Some("cpu");
        } else if ["nvidia", "ati", "amd", "gpu", "intel"].iter().any(|k| img.contains(k)) {
            hw = Some("gpu");
        }
        for k in kids {
            walk(k, hw, group, out);
        }
    }

    walk(root, None, None, &mut sensors);

    let pick = |hw: &str, group: &str, names: &[&str]| -> Value {
        let vals: Vec<_> = sensors.iter().filter(|s| s.0 == Some(hw) && s.1 == Some(group)).collect();
        for n in names {
            if let Some(s) = vals.iter().find(|s| s.2.starts_with(n)) {
                return json!(s.3);
            }
        }
        if group == "temp" {
            vals.iter()
                .map(|s| s.3)
                .fold(None, |m: Option<f64>, v| Some(m.map_or(v, |m| m.max(v))))
                .map_or(Value::Null, |v| json!(v))
        } else {
            Value::Null
        }
    };

    let mut m = Map::new();
    m.insert("cpu_temp".into(), pick("cpu", "temp", &["CPU Package", "Core (Tctl", "Package", "CPU Core"]));
    m.insert("gpu_temp".into(), pick("gpu", "temp", &["GPU Core", "GPU Hot Spot"]));
    m.insert("gpu".into(), pick("gpu", "load", &["GPU Core", "D3D 3D"]));
    m
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lhm_picks_named_sensors() {
        let v = json!({"Text":"Sensor","Children":[{"Text":"DESKTOP","Children":[
            {"Text":"AMD Ryzen 7","ImageURL":"images_icon/cpu.png","Children":[
                {"Text":"Temperatures","ImageURL":"images_icon/temperature.png","Children":[
                    {"Text":"Core (Tctl/Tdie)","Value":"61,5 °C","Children":[]},
                    {"Text":"CCD1","Value":"70.0 °C","Children":[]}]},
                {"Text":"Load","ImageURL":"images_icon/load.png","Children":[
                    {"Text":"CPU Total","Value":"12.0 %","Children":[]}]}]},
            {"Text":"NVIDIA GeForce","ImageURL":"images_icon/nvidia.png","Children":[
                {"Text":"Temperatures","ImageURL":"images_icon/temperature.png","Children":[
                    {"Text":"GPU Hot Spot","Value":"55.0 °C","Children":[]}]},
                {"Text":"Load","ImageURL":"images_icon/load.png","Children":[
                    {"Text":"GPU Core","Value":"33.0 %","Children":[]}]}]}]}]});
        let m = parse_lhm(&v);
        assert_eq!(m["cpu_temp"], json!(61.5));
        assert_eq!(m["gpu_temp"], json!(55.0));
        assert_eq!(m["gpu"], json!(33.0));
    }

    #[test]
    fn lhm_temp_falls_back_to_max_and_load_to_null() {
        let v = json!({"Children":[{"ImageURL":"cpu.png","Children":[{"ImageURL":"temperature.png","Children":[
            {"Text":"A","Value":"40 °C"},{"Text":"B","Value":"48 °C"}]}]}]});
        let m = parse_lhm(&v);
        assert_eq!(m["cpu_temp"], json!(48.0));
        assert_eq!(m["gpu"], Value::Null);
    }

    #[tokio::test]
    async fn sample_has_every_field() {
        let mut s = Stats::new();
        s.nvsmi = None;
        s.lhm_retry = Some(Instant::now() + Duration::from_secs(60));
        tokio::time::sleep(Duration::from_millis(250)).await;
        let m = s.sample().await;
        for k in ["t", "cpu", "ram", "gpu", "cpu_temp", "gpu_temp", "net_up", "net_down", "disk"] {
            assert!(m.get(k).is_some(), "{k}");
        }
        assert!((1..=100).contains(&m["ram"].as_i64().unwrap()));
        assert!((1..=100).contains(&m["disk"].as_i64().unwrap()));
    }
}
