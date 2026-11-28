//! GPU load and temperature straight from Windows, no app needed, for any vendor:
//! - load: the "GPU Engine" performance counters Task Manager uses (per process and engine); the busiest engine
//!   is the GPU's load, as Task Manager shows it;
//! - temperature: what the display driver reports through D3DKMT (ADAPTERPERFDATA), where Task Manager gets the GPU
//!   temperature of a dedicated card. Integrated GPUs usually report none (0), then it stays unknown.

use std::collections::HashMap;
use windows::Wdk::Graphics::Direct3D::{
    D3DKMT_ADAPTER_PERFDATA, D3DKMT_ADAPTERINFO, D3DKMT_CLOSEADAPTER, D3DKMT_ENUMADAPTERS2, D3DKMT_QUERYADAPTERINFO,
    D3DKMTCloseAdapter, D3DKMTEnumAdapters2, D3DKMTQueryAdapterInfo, KMTQAITYPE_ADAPTERPERFDATA,
};
use windows::Win32::System::Performance::{
    PDH_FMT_COUNTERVALUE_ITEM_W, PDH_FMT_DOUBLE, PDH_HCOUNTER, PDH_HQUERY, PDH_MORE_DATA, PdhAddEnglishCounterW,
    PdhCloseQuery, PdhCollectQueryData, PdhGetFormattedCounterArrayW, PdhOpenQueryW,
};
use windows::core::w;

/// An open PDH query on `\GPU Engine(*)\Utilization Percentage` (a rate: needs two collections).
pub struct GpuLoad {
    query: PDH_HQUERY,
    counter: PDH_HCOUNTER,
}

// SAFETY: PDH handles may be used from any thread; every call that uses them takes `&mut self`, so a shared `&GpuLoad`
// can do nothing with them (and Stats is behind a mutex anyway).
unsafe impl Send for GpuLoad {}
unsafe impl Sync for GpuLoad {}

impl GpuLoad {
    /// None when the counters are missing (old Windows, no WDDM 2 GPU, counters disabled).
    pub fn open() -> Option<Self> {
        // SAFETY: out pointers are valid; the query is closed on failure and in Drop.
        unsafe {
            let mut query = PDH_HQUERY::default();
            if PdhOpenQueryW(None, 0, &mut query) != 0 {
                return None;
            }
            let mut counter = PDH_HCOUNTER::default();
            if PdhAddEnglishCounterW(query, w!("\\GPU Engine(*)\\Utilization Percentage"), 0, &mut counter) != 0 {
                PdhCloseQuery(query);
                return None;
            }
            PdhCollectQueryData(query); // first sample: rates need a second one
            Some(Self { query, counter })
        }
    }

    /// Load in % since the previous call (0..100), None if no engine reported.
    pub fn sample(&mut self) -> Option<f64> {
        // SAFETY: the buffer is sized by the first call and holds `count` items; names point into it.
        unsafe {
            if PdhCollectQueryData(self.query) != 0 {
                return None;
            }
            let (mut size, mut count) = (0u32, 0u32);
            let r = PdhGetFormattedCounterArrayW(self.counter, PDH_FMT_DOUBLE, &mut size, &mut count, None);
            if r != PDH_MORE_DATA || size == 0 {
                return None;
            }
            let mut buf = vec![0u64; (size as usize).div_ceil(8)];
            let items = buf.as_mut_ptr() as *mut PDH_FMT_COUNTERVALUE_ITEM_W;
            if PdhGetFormattedCounterArrayW(self.counter, PDH_FMT_DOUBLE, &mut size, &mut count, Some(items)) != 0 {
                return None;
            }
            let items = std::slice::from_raw_parts(items, count as usize);
            let named = items.iter().filter(|it| it.FmtValue.CStatus == 0).filter_map(|it| {
                let name = it.szName.to_string().ok()?;
                Some((name, it.FmtValue.Anonymous.doubleValue))
            });
            busiest_engine(named)
        }
    }
}

impl Drop for GpuLoad {
    fn drop(&mut self) {
        // SAFETY: our own query.
        unsafe {
            PdhCloseQuery(self.query);
        }
    }
}

/// Instances look like `pid_1234_luid_0x00000000_0x0000D1A4_phys_0_eng_3_engtype_3D`: add up the processes on each
/// engine (`luid…_eng_3`), the busiest engine is the load. None when there is no instance at all.
pub fn busiest_engine(items: impl Iterator<Item = (String, f64)>) -> Option<f64> {
    let mut per_engine: HashMap<String, f64> = HashMap::new();
    for (name, v) in items {
        let Some(at) = name.find("luid_") else { continue };
        let engine = name[at..].split("_engtype").next().unwrap_or(&name[at..]).to_owned();
        *per_engine.entry(engine).or_default() += v.max(0.0);
    }
    per_engine.into_values().reduce(f64::max).map(|v| v.min(100.0))
}

/// The display adapters opened once for temperature reads. They are enumerated again when a query fails (driver
/// update, eGPU unplugged) and every REENUMERATE reads, so an adapter added later is seen too.
#[derive(Default)]
pub struct GpuTemp {
    adapters: Vec<u32>,
    reads: u32,
}

const REENUMERATE: u32 = 60;

impl GpuTemp {
    fn close(&mut self) {
        for &h in &self.adapters {
            let close = D3DKMT_CLOSEADAPTER { hAdapter: h };
            // SAFETY: a handle we opened through D3DKMTEnumAdapters2 and have not closed yet.
            let _ = unsafe { D3DKMTCloseAdapter(&close) };
        }
        self.adapters.clear();
    }

    fn open(&mut self) {
        self.close();
        // SAFETY: the first call gets the count, the second fills a buffer of that size.
        unsafe {
            let mut e = D3DKMT_ENUMADAPTERS2::default();
            if D3DKMTEnumAdapters2(&mut e).is_err() || e.NumAdapters == 0 {
                return;
            }
            let mut infos = vec![D3DKMT_ADAPTERINFO::default(); e.NumAdapters as usize];
            e.pAdapters = infos.as_mut_ptr();
            if D3DKMTEnumAdapters2(&mut e).is_err() {
                return;
            }
            self.adapters = infos[..e.NumAdapters as usize].iter().map(|i| i.hAdapter).collect();
        }
    }

    /// Highest temperature (°C) any display adapter reports, None when none does (integrated GPUs, old drivers).
    pub fn read(&mut self) -> Option<f64> {
        if self.adapters.is_empty() || self.reads >= REENUMERATE {
            self.open();
            self.reads = 0;
        }
        self.reads += 1;
        let mut best: Option<f64> = None;
        let mut failed = false;
        for &h in &self.adapters {
            let mut perf = D3DKMT_ADAPTER_PERFDATA::default();
            let mut q = D3DKMT_QUERYADAPTERINFO {
                hAdapter: h,
                Type: KMTQAITYPE_ADAPTERPERFDATA,
                pPrivateDriverData: &mut perf as *mut _ as *mut _,
                PrivateDriverDataSize: size_of::<D3DKMT_ADAPTER_PERFDATA>() as u32,
            };
            // SAFETY: q points at a live perf struct of the size given.
            if unsafe { D3DKMTQueryAdapterInfo(&mut q) }.is_err() {
                failed = true;
                continue;
            }
            // tenths of a degree; 0 = not reported
            if perf.Temperature > 0 && perf.Temperature < 1500 {
                let t = perf.Temperature as f64 / 10.0;
                best = Some(best.map_or(t, |b: f64| b.max(t)));
            }
        }
        if failed {
            self.reads = REENUMERATE; // enumerate again next time
        }
        best
    }
}

impl Drop for GpuTemp {
    fn drop(&mut self) {
        self.close();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn busiest_engine_adds_processes_per_engine() {
        let items = [
            ("pid_10_luid_0x0_0xD1A4_phys_0_eng_0_engtype_3D", 30.0),
            ("pid_11_luid_0x0_0xD1A4_phys_0_eng_0_engtype_3D", 25.0), // same engine: 55
            ("pid_10_luid_0x0_0xD1A4_phys_0_eng_3_engtype_VideoDecode", 40.0),
            ("pid_12_luid_0x0_0xBEEF_phys_0_eng_0_engtype_3D", 20.0), // another GPU
            ("junk", 99.0),
        ];
        let v = busiest_engine(items.iter().map(|(n, v)| (n.to_string(), *v)));
        assert_eq!(v, Some(55.0));
        assert_eq!(busiest_engine(std::iter::empty()), None);
        let over = [
            ("pid_1_luid_0x0_0x1_phys_0_eng_0_engtype_3D", 80.0),
            ("pid_2_luid_0x0_0x1_phys_0_eng_0_engtype_3D", 70.0),
        ];
        assert_eq!(busiest_engine(over.iter().map(|(n, v)| (n.to_string(), *v))), Some(100.0));
    }

    /// On this PC: the counters exist and give a value in range; the temperature call does not fail (it may be None).
    #[test]
    fn reads_this_pc() {
        let Some(mut g) = GpuLoad::open() else { return };
        std::thread::sleep(std::time::Duration::from_millis(300));
        if let Some(v) = g.sample() {
            assert!((0.0..=100.0).contains(&v), "{v}");
        }
        let mut temp = GpuTemp::default();
        for _ in 0..3 {
            if let Some(t) = temp.read() {
                assert!((1.0..150.0).contains(&t), "{t}");
            }
        }
    }
}
