//! Logging: the console, or `%APPDATA%\MeowBox\agent.log` rotated at 1 MB × 3 (agent.log.1 .. .3, like Python's
//! RotatingFileHandler). Line format as the Python agent: `11-27 19:01:02 INFO message`.

use std::fs::{File, OpenOptions};
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use tracing_subscriber::fmt::format::Writer;
use tracing_subscriber::fmt::time::FormatTime;
use tracing_subscriber::fmt::{self, MakeWriter};
use tracing_subscriber::{EnvFilter, prelude::*};

const MAX_BYTES: u64 = 1_000_000;
const BACKUPS: u32 = 3;

struct LocalTime;

impl FormatTime for LocalTime {
    fn format_time(&self, w: &mut Writer<'_>) -> std::fmt::Result {
        // SAFETY: GetLocalTime only fills the returned struct.
        let t = unsafe { windows::Win32::System::SystemInformation::GetLocalTime() };
        write!(w, "{:02}-{:02} {:02}:{:02}:{:02}", t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond)
    }
}

struct Rotating {
    path: PathBuf,
    file: Option<File>,
    size: u64,
}

impl Rotating {
    fn open(path: &Path) -> io::Result<Self> {
        let file = OpenOptions::new().create(true).append(true).open(path)?;
        let size = file.metadata().map(|m| m.len()).unwrap_or(0);
        Ok(Self { path: path.to_owned(), file: Some(file), size })
    }

    fn backup(&self, n: u32) -> PathBuf {
        let mut s = self.path.clone().into_os_string();
        s.push(format!(".{n}"));
        s.into()
    }

    fn rotate(&mut self) -> io::Result<()> {
        self.file = None;
        let _ = std::fs::remove_file(self.backup(BACKUPS));
        for n in (1..BACKUPS).rev() {
            let _ = std::fs::rename(self.backup(n), self.backup(n + 1));
        }
        let _ = std::fs::rename(&self.path, self.backup(1));
        self.file = Some(OpenOptions::new().create(true).append(true).open(&self.path)?);
        self.size = 0;
        Ok(())
    }

    fn write_line(&mut self, buf: &[u8]) -> io::Result<()> {
        if self.size > 0 && self.size + buf.len() as u64 > MAX_BYTES {
            self.rotate()?;
        }
        if let Some(f) = self.file.as_mut() {
            f.write_all(buf)?;
            self.size += buf.len() as u64;
        }
        Ok(())
    }
}

#[derive(Clone)]
struct FileWriter(Arc<Mutex<Rotating>>);

/// One tracing event = one `write` call in fmt (it formats into a buffer first), so rotation never splits a line.
struct Handle(Arc<Mutex<Rotating>>);

impl Write for Handle {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        let mut r = self.0.lock().unwrap_or_else(|e| e.into_inner());
        r.write_line(buf)?;
        Ok(buf.len())
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

impl<'a> MakeWriter<'a> for FileWriter {
    type Writer = Handle;
    fn make_writer(&'a self) -> Handle {
        Handle(self.0.clone())
    }
}

/// Set up logging once. `to_file`: agent.log in `dir` (returns its path); else stderr. Panics are logged too.
pub fn setup(verbose: bool, to_file: bool, dir: &Path) -> Option<PathBuf> {
    let level = if verbose { "debug" } else { "info" };
    // our crate at `level`, dependencies only from warn up (egui/winit are chatty at debug)
    let filter = EnvFilter::new(format!("warn,meowbox_agent={level},meowbox={level},MeowBoxAgent={level}"));
    let base = fmt::layer().with_timer(LocalTime).with_target(false).with_level(true);
    let mut logfile = None;
    let ok = if to_file {
        let path = dir.join("agent.log");
        match std::fs::create_dir_all(dir).and_then(|_| Rotating::open(&path)) {
            Ok(r) => {
                logfile = Some(path);
                let w = FileWriter(Arc::new(Mutex::new(r)));
                tracing_subscriber::registry().with(filter).with(base.with_ansi(false).with_writer(w)).try_init()
            }
            Err(_) => tracing_subscriber::registry().with(filter).with(base.with_writer(io::stderr)).try_init(),
        }
    } else {
        tracing_subscriber::registry().with(filter).with(base.with_writer(io::stderr)).try_init()
    };
    if ok.is_ok() {
        let default = std::panic::take_hook();
        std::panic::set_hook(Box::new(move |info| {
            tracing::error!("panic: {info}");
            default(info);
        }));
    }
    logfile
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rotates_and_keeps_three_backups() {
        let dir = std::env::temp_dir().join(format!("meowbox-log-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("agent.log");
        let mut r = Rotating::open(&path).unwrap();
        let line = vec![b'x'; 300_000];
        for _ in 0..20 {
            r.write_line(&line).unwrap();
        }
        assert!(std::fs::metadata(&path).unwrap().len() <= MAX_BYTES);
        assert!(r.backup(3).exists());
        assert!(!r.backup(4).exists());
        drop(r);
        std::fs::remove_dir_all(&dir).ok();
    }
}
