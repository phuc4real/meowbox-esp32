//! Now playing (Windows Global System Media Transport Controls) for the speaker's media tile, and the speaker's
//! media buttons back: through GSMTC, falling back to media keys; volume on the default output device.

use serde_json::{Value, json};
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
    mgr: tokio::sync::OnceCell<Option<Manager>>,
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
    async fn session(&self) -> Option<Session> {
        let mgr = self
            .mgr
            .get_or_init(|| async {
                match Manager::RequestAsync() {
                    Ok(op) => op.await.ok(),
                    Err(_) => None,
                }
            })
            .await
            .as_ref()?;
        mgr.GetCurrentSession().ok()
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
            "t": "media", "app": app_name(&aumid), "title": props.Title()?.to_string(),
            "artist": props.Artist()?.to_string(), "state": state,
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

    #[tokio::test]
    async fn read_gives_a_media_message() {
        let m = Media::default().read().await;
        assert_eq!(m["t"], "media");
        for k in ["app", "title", "artist", "state", "pos", "len"] {
            assert!(m.get(k).is_some(), "{k}");
        }
    }
}
