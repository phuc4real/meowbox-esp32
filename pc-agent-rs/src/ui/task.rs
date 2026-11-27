//! Background work for the window: a future on the core's runtime whose result the window picks up on a later
//! frame (egui redraws when the result arrives).

use crate::link::CoreHandle;
use eframe::egui;
use std::future::Future;
use std::sync::{Arc, Mutex};

pub struct Task<T>(Arc<Mutex<Option<T>>>);

impl<T: Send + 'static> Task<T> {
    pub fn spawn(core: &CoreHandle, ctx: &egui::Context, f: impl Future<Output = T> + Send + 'static) -> Self {
        let slot = Arc::new(Mutex::new(None));
        let (s, c) = (slot.clone(), ctx.clone());
        core.spawn(async move {
            let r = f.await;
            *s.lock().unwrap_or_else(|e| e.into_inner()) = Some(r);
            c.request_repaint();
        });
        Self(slot)
    }

    /// The result, once (None while still running).
    pub fn take(&self) -> Option<T> {
        self.0.lock().unwrap_or_else(|e| e.into_inner()).take()
    }
}

/// Run a blocking function (COM, files, a child process) off the runtime's worker threads.
pub async fn blocking<T: Send + 'static>(f: impl FnOnce() -> T + Send + 'static) -> Option<T> {
    tokio::task::spawn_blocking(move || {
        crate::win::com_init();
        f()
    })
    .await
    .ok()
}

/// Poll a pending task: when it finishes, clear it and hand the result to `done`.
pub fn poll<T: Send + 'static>(slot: &mut Option<Task<T>>, done: impl FnOnce(T)) {
    if let Some(r) = slot.as_ref().and_then(Task::take) {
        *slot = None;
        done(r);
    }
}
