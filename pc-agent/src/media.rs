//! Now playing (Windows Global System Media Transport Controls) for the speaker's media tile, and the speaker's
//! media buttons back: through GSMTC, falling back to media keys; volume on the default output device.
//!
//! The link reads the media state when Windows says it changed (`watch` subscribes to the current session's events),
//! plus every few seconds as a resend; when subscribing fails it polls every second instead.

use serde_json::{Value, json};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};
use windows::Foundation::TypedEventHandler;
use windows::Media::Control::{
    GlobalSystemMediaTransportControlsSession as Session, GlobalSystemMediaTransportControlsSessionManager as Manager,
};
use windows::Win32::Media::Audio::Endpoints::IAudioEndpointVolume;
use windows::Win32::Media::Audio::{IMMDeviceEnumerator, MMDeviceEnumerator, eMultimedia, eRender};
use windows::Win32::System::Com::{CLSCTX_ALL, CoCreateInstance};
use windows::Win32::UI::Input::KeyboardAndMouse::{KEYEVENTF_KEYUP, keybd_event};

const VK_PLAY_PAUSE: u8 = 0xB3;
const VK_NEXT: u8 = 0xB0;
const VK_PREV: u8 = 0xB1;
const VK_VOL_UP: u8 = 0xAF;
const VK_VOL_DOWN: u8 = 0xAE;

#[derive(Default)]
pub struct Media {
    mgr: tokio::sync::Mutex<Retrying<Manager>>,
    subs: tokio::sync::Mutex<Subs>,
    /// Woken by every event (and kept as a permit if nobody waits yet).
    changed: Arc<tokio::sync::Notify>,
    /// Set by CurrentSessionChanged: subscribe to the new session.
    session_moved: Arc<AtomicBool>,
}

/// Event registrations; removed when replaced or dropped.
#[derive(Default)]
struct Subs {
    mgr: Option<(Manager, i64)>,
    session: Option<(Session, [i64; 3])>,
}

impl Subs {
    fn drop_session(&mut self) {
        if let Some((s, [a, b, c])) = self.session.take() {
            let _ = s.RemoveMediaPropertiesChanged(a);
            let _ = s.RemovePlaybackInfoChanged(b);
            let _ = s.RemoveTimelinePropertiesChanged(c);
        }
    }
}

impl Drop for Subs {
    fn drop(&mut self) {
        self.drop_session();
        if let Some((m, t)) = self.mgr.take() {
            let _ = m.RemoveCurrentSessionChanged(t);
        }
    }
}

/// When to send a `media` message: when it changed (the position aside, the speaker moves it on by itself), else
/// every RESEND.
#[derive(Default)]
pub struct MediaGate {
    last: Option<(Value, Instant)>,
}

pub const RESEND: Duration = Duration::from_secs(5);

impl MediaGate {
    pub fn should_send(&mut self, msg: &Value, now: Instant) -> bool {
        let mut key = msg.clone();
        if let Some(o) = key.as_object_mut() {
            o.remove("pos");
        }
        let send = match &self.last {
            Some((k, t)) => *k != key || now.duration_since(*t) >= RESEND,
            None => true,
        };
        if send {
            self.last = Some((key, now));
        }
        send
    }

    /// Time left before the next resend.
    pub fn until_resend(&self, now: Instant) -> Duration {
        self.last.as_ref().map_or(Duration::ZERO, |(_, t)| RESEND.saturating_sub(now.duration_since(*t)))
    }
}

/// How long a failed GSMTC start is remembered before the next try (at logon the service may not be up yet).
const RETRY_AFTER: Duration = Duration::from_secs(30);

/// A value that is made once, but tried again at most every RETRY_AFTER while making it fails.
pub struct Retrying<T> {
    val: Option<T>,
    failed_at: Option<Instant>,
}

impl<T> Default for Retrying<T> {
    fn default() -> Self {
        Self { val: None, failed_at: None }
    }
}

impl<T> Retrying<T> {
    pub async fn get<F: Future<Output = Option<T>>>(&mut self, now: Instant, make: impl FnOnce() -> F) -> Option<&T> {
        if self.val.is_none() && self.failed_at.is_none_or(|t| now.duration_since(t) >= RETRY_AFTER) {
            self.val = make().await;
            self.failed_at = if self.val.is_none() { Some(now) } else { None };
        }
        self.val.as_ref()
    }
}

/// Titles are cut to what the speaker can show (it keeps 96 / 64 bytes) with room to spare: the speaker drops a
/// `/ws/pc` frame over 4 KB, and a page title can be any length.
const MEDIA_TEXT_MAX: usize = 200;

fn clip(s: &str) -> String {
    s.chars().take(MEDIA_TEXT_MAX).collect()
}

fn empty() -> Value {
    json!({"t": "media", "app": "", "title": "", "artist": "", "state": "stopped", "pos": 0, "len": 0})
}

/// Windows FILETIME now (100 ns since 1601), the unit of WinRT DateTime.
fn filetime_now() -> i64 {
    // SAFETY: fills a plain struct.
    let ft = unsafe { windows::Win32::System::SystemInformation::GetSystemTimeAsFileTime() };
    ((ft.dwHighDateTime as i64) << 32) | ft.dwLowDateTime as i64
}

/// AUMID → short app name: "Spotify.exe" → "Spotify", "Microsoft.ZuneMusic_8wekyb3d8bbwe!Microsoft.ZuneMusic" →
/// "Microsoft.ZuneMusic".
pub fn app_name(aumid: &str) -> &str {
    let last = aumid.rsplit('!').next().unwrap_or(aumid);
    last.strip_suffix(".exe").unwrap_or(last)
}

impl Media {
    async fn manager(&self) -> Option<Manager> {
        let mut mgr = self.mgr.lock().await;
        mgr.get(Instant::now(), || async {
            match Manager::RequestAsync() {
                Ok(op) => op.await.inspect_err(|e| tracing::debug!("media session manager: {e}")).ok(),
                Err(e) => {
                    tracing::debug!("media session manager: {e}");
                    None
                }
            }
        })
        .await
        .cloned()
    }

    async fn session(&self) -> Option<Session> {
        self.manager().await?.GetCurrentSession().ok()
    }

    /// Make sure we hear about changes of the current session. False when that is not possible (GSMTC not up):
    /// the caller then polls.
    pub async fn watch(&self) -> bool {
        let mut subs = self.subs.lock().await;
        if subs.mgr.is_none() {
            let Some(mgr) = self.manager().await else { return false };
            let (n, moved) = (self.changed.clone(), self.session_moved.clone());
            let h = TypedEventHandler::new(move |_, _| {
                moved.store(true, Ordering::SeqCst);
                n.notify_one();
                Ok(())
            });
            match mgr.CurrentSessionChanged(&h) {
                Ok(t) => subs.mgr = Some((mgr, t)),
                Err(e) => {
                    tracing::debug!("media: no session events ({e})");
                    return false;
                }
            }
            self.session_moved.store(true, Ordering::SeqCst);
        }
        if self.session_moved.swap(false, Ordering::SeqCst) {
            subs.drop_session();
            let cur = subs.mgr.as_ref().and_then(|(m, _)| m.GetCurrentSession().ok());
            if let Some(s) = cur {
                match self.subscribe(&s) {
                    Ok(tokens) => subs.session = Some((s, tokens)),
                    Err(e) => {
                        tracing::debug!("media: no events from this session ({e})");
                        self.session_moved.store(true, Ordering::SeqCst); // try again next time
                        return false;
                    }
                }
            }
        }
        true
    }

    fn subscribe(&self, s: &Session) -> windows::core::Result<[i64; 3]> {
        let wake = || {
            let n = self.changed.clone();
            move || {
                n.notify_one();
                Ok(())
            }
        };
        let (a, b, c) = (wake(), wake(), wake());
        let ta = s.MediaPropertiesChanged(&TypedEventHandler::new(move |_, _| a()))?;
        let tb = match s.PlaybackInfoChanged(&TypedEventHandler::new(move |_, _| b())) {
            Ok(t) => t,
            Err(e) => {
                let _ = s.RemoveMediaPropertiesChanged(ta);
                return Err(e);
            }
        };
        match s.TimelinePropertiesChanged(&TypedEventHandler::new(move |_, _| c())) {
            Ok(tc) => Ok([ta, tb, tc]),
            Err(e) => {
                let _ = s.RemoveMediaPropertiesChanged(ta);
                let _ = s.RemovePlaybackInfoChanged(tb);
                Err(e)
            }
        }
    }

    /// Wait until Windows reports a media change, at most `max`.
    pub async fn changed(&self, max: Duration) {
        let _ = tokio::time::timeout(max, self.changed.notified()).await;
    }

    async fn read_inner(&self) -> windows::core::Result<Option<Value>> {
        let Some(s) = self.session().await else { return Ok(None) };
        let props = s.TryGetMediaPropertiesAsync()?.await?;
        let tl = s.GetTimelineProperties()?;
        let state = match s.GetPlaybackInfo()?.PlaybackStatus()?.0 {
            4 => "playing",
            5 => "paused",
            _ => "stopped",
        };
        let mut pos = tl.Position()?.Duration as f64 / 1e7;
        let len = tl.EndTime()?.Duration as f64 / 1e7;
        let updated = tl.LastUpdatedTime()?.UniversalTime;
        if state == "playing" && updated > 0 {
            // position is a snapshot taken at `updated`
            pos += (filetime_now() - updated) as f64 / 1e7;
        }
        let pos = if len > 0.0 { pos.min(len) } else { pos };
        let aumid = s.SourceAppUserModelId()?.to_string();
        Ok(Some(json!({
            "t": "media", "app": app_name(&aumid), "title": clip(&props.Title()?.to_string()),
            "artist": clip(&props.Artist()?.to_string()), "state": state,
            "pos": crate::json::round(pos), "len": crate::json::round(len),
        })))
    }

    /// One `media` message (an empty one when nothing plays or GSMTC fails).
    pub async fn read(&self) -> Value {
        match self.read_inner().await {
            Ok(Some(v)) => v,
            Ok(None) => empty(),
            Err(e) => {
                tracing::debug!("media read failed: {e}");
                empty()
            }
        }
    }

    /// A media_cmd from the speaker: play_pause | next | prev | vol_up | vol_down.
    pub async fn command(&self, cmd: &str) {
        let vk = match cmd {
            "play_pause" => VK_PLAY_PAUSE,
            "next" => VK_NEXT,
            "prev" => VK_PREV,
            "vol_up" => VK_VOL_UP,
            "vol_down" => VK_VOL_DOWN,
            _ => {
                tracing::warn!("unknown media_cmd {cmd:?}");
                return;
            }
        };
        tracing::info!("media_cmd {cmd}");
        if cmd.starts_with("vol_") {
            set_volume(if cmd == "vol_up" { 0.05 } else { -0.05 });
            return;
        }
        if let Some(s) = self.session().await {
            let op = match cmd {
                "play_pause" => s.TryTogglePlayPauseAsync(),
                "next" => s.TrySkipNextAsync(),
                _ => s.TrySkipPreviousAsync(),
            };
            match op {
                Ok(op) => match op.await {
                    Ok(true) => return,
                    Ok(false) => tracing::debug!("media api refused {cmd}, using media key"),
                    Err(e) => tracing::debug!("media api failed ({e}), using media key"),
                },
                Err(e) => tracing::debug!("media api failed ({e}), using media key"),
            }
        }
        press_key(vk);
    }
}

/// Default output device volume ± delta (0..1), falling back to the volume keys.
pub fn set_volume(delta: f32) {
    let r: windows::core::Result<()> = (|| {
        crate::win::com_init();
        // SAFETY: COM calls on an initialised thread; interfaces are released when dropped.
        unsafe {
            let en: IMMDeviceEnumerator = CoCreateInstance(&MMDeviceEnumerator, None, CLSCTX_ALL)?;
            let dev = en.GetDefaultAudioEndpoint(eRender, eMultimedia)?;
            let ep: IAudioEndpointVolume = dev.Activate(CLSCTX_ALL, None)?;
            let v = (ep.GetMasterVolumeLevelScalar()? + delta).clamp(0.0, 1.0);
            ep.SetMasterVolumeLevelScalar(v, std::ptr::null())
        }
    })();
    if let Err(e) = r {
        tracing::debug!("endpoint volume failed ({e}), using volume key");
        press_key(if delta > 0.0 { VK_VOL_UP } else { VK_VOL_DOWN });
    }
}

pub fn press_key(vk: u8) {
    // SAFETY: synthesises one key press and release.
    unsafe {
        keybd_event(vk, 0, Default::default(), 0);
        keybd_event(vk, 0, KEYEVENTF_KEYUP, 0);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn app_names() {
        assert_eq!(app_name("Spotify.exe"), "Spotify");
        assert_eq!(app_name("Microsoft.ZuneMusic_8wekyb3d8bbwe!Microsoft.ZuneMusic"), "Microsoft.ZuneMusic");
        assert_eq!(app_name("chrome"), "chrome");
    }

    /// A failed start is tried again after RETRY_AFTER, not before; a success is kept.
    #[tokio::test]
    async fn failed_start_is_retried_later() {
        let mut r = Retrying::<u32>::default();
        let t0 = Instant::now();
        let calls = std::cell::Cell::new(0);
        let make = |v: Option<u32>| {
            calls.set(calls.get() + 1);
            async move { v }
        };
        assert_eq!(r.get(t0, || make(None)).await, None);
        assert_eq!(r.get(t0 + Duration::from_secs(5), || make(Some(7))).await, None, "too soon");
        assert_eq!(calls.get(), 1);
        assert_eq!(r.get(t0 + RETRY_AFTER, || make(Some(7))).await, Some(&7));
        assert_eq!(r.get(t0 + RETRY_AFTER * 5, || make(Some(9))).await, Some(&7), "kept");
        assert_eq!(calls.get(), 2);
    }

    #[test]
    fn media_is_sent_when_it_changes_or_every_resend() {
        let mut g = MediaGate::default();
        let t0 = Instant::now();
        let m = |title: &str, pos: i64| json!({"t": "media", "title": title, "state": "playing", "pos": pos});
        assert!(g.should_send(&m("a", 0), t0), "first");
        assert!(!g.should_send(&m("a", 1), t0 + Duration::from_secs(1)), "only the position moved");
        assert_eq!(g.until_resend(t0 + Duration::from_secs(1)), RESEND - Duration::from_secs(1));
        assert!(g.should_send(&m("b", 1), t0 + Duration::from_secs(2)), "new track at once");
        assert!(!g.should_send(&m("b", 3), t0 + Duration::from_secs(6)));
        assert!(g.should_send(&m("b", 5), t0 + Duration::from_secs(2) + RESEND), "resend");
    }

    /// On this PC: watching works or says it cannot, and a change wait returns within its limit.
    #[tokio::test]
    async fn watch_and_wait() {
        let m = Media::default();
        let _ = m.watch().await;
        let t = Instant::now();
        m.changed(Duration::from_millis(200)).await;
        assert!(t.elapsed() < Duration::from_secs(2));
    }

    #[test]
    fn long_titles_are_cut() {
        let long = "Bài hát ".repeat(100);
        assert_eq!(clip(&long).chars().count(), MEDIA_TEXT_MAX);
        assert_eq!(clip("short"), "short");
    }

    #[tokio::test]
    async fn read_gives_a_media_message() {
        let m = Media::default().read().await;
        assert_eq!(m["t"], "media");
        for k in ["app", "title", "artist", "state", "pos", "len"] {
            assert!(m.get(k).is_some(), "{k}");
        }
    }
}
