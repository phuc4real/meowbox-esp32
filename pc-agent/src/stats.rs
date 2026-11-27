//! PC stats for the speaker's stats tile: CPU, RAM, disk, network from sysinfo; GPU load and GPU temperature from
//! Windows itself (gpu.rs, any vendor), overridden by nvidia-smi on an NVIDIA card (it ships with the driver). No
//! third-party app: `cpu_temp` is always null, as Windows has no API for it without a kernel driver; the speaker then
//! shows the GPU temperature.

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
    nvsmi: Option<PathBuf>,
    gpu: Option<crate::gpu::GpuLoad>,
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
            nvsmi: which("nvidia-smi"),
            gpu: crate::gpu::GpuLoad::open(),
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
        // Windows' own numbers first, nvidia-smi overrides them
        if let Some(v) = self.gpu.as_mut().and_then(|g| g.sample()) {
            msg.insert("gpu".into(), json!(round(v)));
        }
        if let Some(t) = crate::gpu::temperature() {
            msg.insert("gpu_temp".into(), json!(round(t)));
        }
        for (k, v) in self.nvidia().await {
            if let Some(x) = v.as_f64() {
                msg.insert(k, json!(round(x)));
            }
        }
        Value::Object(msg)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn sample_has_every_field() {
        let mut s = Stats::new();
        s.nvsmi = None;
        tokio::time::sleep(Duration::from_millis(250)).await;
        let m = s.sample().await;
        for k in ["t", "cpu", "ram", "gpu", "cpu_temp", "gpu_temp", "net_up", "net_down", "disk"] {
            assert!(m.get(k).is_some(), "{k}");
        }
        assert!((1..=100).contains(&m["ram"].as_i64().unwrap()));
        assert!((1..=100).contains(&m["disk"].as_i64().unwrap()));
        assert_eq!(m["cpu_temp"], Value::Null, "no source for it");
    }
}
