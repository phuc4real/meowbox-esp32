//! Window, tray and toast strings in Vietnamese and English (docs/enhance_agent.md, B4). Both tables are the same
//! struct, so a key missing in one does not compile; `tests` checks they take the same `{}` arguments. The command
//! line keeps its own (English) messages.
//!
//! Voice: plain verbs, sentence case, no filler. Errors say what happened and how to fix it, with no apology and no
//! joke; the cat speaks only where it is the subject (Meow Coding, the test notification). A button and the toast
//! it gives use the same words.

use std::sync::atomic::{AtomicBool, Ordering};

macro_rules! table {
    ($($k:ident),* $(,)?) => {
        pub struct S { $(pub $k: &'static str,)* }
        impl S {
            #[cfg(test)]
            pub fn entries(&self) -> Vec<(&'static str, &'static str)> {
                vec![$((stringify!($k), self.$k)),*]
            }
        }
    };
}

table! {
    // tray menu
    m_take_over, m_open_window, m_open_web, m_pause, m_coding, m_new_version, m_quit,
    // tray status line (≤ ~45 characters)
    st_connected, st_searching, st_no_ip, st_lost, st_lost_retry, st_not_found, st_not_found_retry, st_token, st_busy,
    st_proto_agent, st_proto_speaker, st_port,
    // toasts
    toast_title, t_lost, t_token, t_taken_over, t_test_sent, t_no_speaker, t_speaker_error, t_send_failed,
    t_coding_on, t_coding_off, t_coding_failed, t_new_agent, t_ffmpeg_start, t_ffmpeg_done, t_ffmpeg_failed,
    test_title, test_text,
    // window
    app, tab_connect, tab_settings,
    // speaker mirror
    mi_connected, mi_searching, mi_off, mi_nothing_playing, mi_media_paused, mi_temp, mi_paused, mi_resume,
    why_no_ip, why_searching, why_not_found, why_lost, why_token, why_busy, why_port, why_proto_agent,
    why_proto_speaker, do_enter_ip, do_retry, do_enter_token, do_take_over, do_change_port, do_see_update,
    do_open_web,
    // connect page
    c_ip, c_token, c_relay, c_check, c_checking, c_need_ip, c_need_token, c_ip_hint, c_ip_ok,
    c_err_not_meowbox, c_err_bad_address, c_err_other_network, c_err_no_answer, c_reasons, c_not_wifi, c_wifi_help,
    c_token_ok, c_token_bad, c_token_err, c_open_notify, c_relay_ok, c_relay_fail, c_relay_no_link, c_relay_wait, c_relay_public,
    c_allow, c_allowed, c_port_busy, c_port_save, c_port_saved, c_test, c_reconnect,
    // settings page
    s_agent, s_version, s_check, s_checking, s_no_release, s_has, s_latest, s_install, s_downloading,
    s_agent_failed, s_ytdlp, s_ytdlp_missing, s_update, s_cannot_check, s_ytdlp_failed, s_ytdlp_installed,
    s_ffmpeg, s_ff_download, s_ff_redownload, s_ff_path, s_ff_own, s_ff_none, s_ff_last_error, s_auto_update,
    s_auto_update_box, s_autostart, s_autostart_box, s_port, s_save, s_port_range, s_port_saved, s_lang, s_lang_auto,
    s_log_dir, s_github, s_about, s_mb,
}

pub static VI: S = S {
    m_take_over: "Giành quyền nối loa",
    m_open_window: "Mở cửa sổ MeowBox Agent",
    m_open_web: "Mở trang loa",
    m_pause: "Tạm dừng gửi số liệu",
    m_coding: "Meow Coding (Claude Code)",
    m_new_version: "Có bản {}",
    m_quit: "Thoát",

    st_connected: "Đã nối {}",
    st_searching: "Đang tìm loa…",
    st_no_ip: "Chưa có IP loa: mở cửa sổ để nhập",
    st_lost: "Mất kết nối với loa",
    st_lost_retry: "Mất kết nối với loa, thử lại sau {} s",
    st_not_found: "Không thấy loa",
    st_not_found_retry: "Không thấy loa, thử lại sau {} s",
    st_token: "Loa không nhận token",
    st_busy: "{} đang nối với {}",
    st_proto_agent: "Loa cần MeowBox Agent mới hơn",
    st_proto_speaker: "Loa cần firmware mới hơn",
    st_port: "Cổng relay {} đang bị app khác dùng",

    toast_title: "MeowBox",
    t_lost: "Mất kết nối với {} hơn 2 phút. Agent vẫn tự thử lại.",
    t_token: "Loa không nhận token này. Lấy token ở trang Thông báo của loa.",
    t_taken_over: "{} đã nối với {}. Bấm Giành quyền nối loa trong menu để lấy lại.",
    t_test_sent: "Đã gửi thông báo thử",
    t_no_speaker: "Chưa nối được loa. Mở cửa sổ MeowBox Agent để kiểm tra.",
    t_speaker_error: "Không gửi được thông báo thử: loa trả lỗi {}.",
    t_send_failed: "Không gửi được thông báo thử: {}",
    t_coding_on: "Đã bật Meow Coding. Mèo vào vị trí! Mở phiên Claude Code mới để mèo canh cùng bạn.",
    t_coding_off: "Đã tắt Meow Coding. Mèo nghỉ canh Claude Code.",
    t_coding_failed: "Không sửa được ~/.claude/settings.json: {}",
    t_new_agent: "Có MeowBox Agent {}. Mở Cài đặt để cập nhật.",
    t_ffmpeg_start: "Đang tải ffmpeg cho YouTube live (một lần, khoảng 75 MB)…",
    t_ffmpeg_done: "Đã tải xong ffmpeg. YouTube live phát được rồi.",
    t_ffmpeg_failed: "Không tải được ffmpeg: {}. Thử lại ở Cài đặt.",
    test_title: "MeowBox Agent",
    test_text: "Meo! Máy tính gửi lời chào.",

    app: "MeowBox Agent",
    tab_connect: "Kết nối",
    tab_settings: "Cài đặt",

    mi_connected: "đã nối",
    mi_searching: "đang tìm",
    mi_off: "chưa nối",
    mi_nothing_playing: "Không có gì đang phát",
    mi_media_paused: "(đang dừng)",
    mi_temp: "Nhiệt độ GPU",
    mi_paused: "Đang tạm dừng gửi số liệu.",
    mi_resume: "Gửi tiếp",
    why_no_ip: "Chưa có IP loa. Nhập IP hiện trên tile Hệ thống của loa.",
    why_searching: "Đang tìm loa ở {}…",
    why_not_found: "Không thấy loa ở {}. Kiểm tra loa đang bật và cùng mạng Wi-Fi với máy này.",
    why_lost: "Mất kết nối với loa. Agent tự thử lại sau {} s.",
    why_token: "Loa không nhận token này. Lấy token ở trang Thông báo của loa.",
    why_busy: "{} đang nối với {}. Mỗi loa chỉ nhận một máy tính.",
    why_port: "Cổng relay {} đang bị app khác dùng. Đổi cổng rồi mở lại agent.",
    why_proto_agent: "Loa cần MeowBox Agent mới hơn bản này.",
    why_proto_speaker: "Loa chạy firmware cũ. Cập nhật firmware ở trang loa.",
    do_enter_ip: "Nhập IP",
    do_retry: "Thử lại ngay",
    do_enter_token: "Nhập token",
    do_take_over: "Giành quyền nối loa",
    do_change_port: "Đổi cổng",
    do_see_update: "Xem bản mới",
    do_open_web: "Mở trang loa",

    c_ip: "IP loa",
    c_token: "Token",
    c_relay: "YouTube relay",
    c_check: "Kiểm tra",
    c_checking: "đang kiểm tra…",
    c_need_ip: "cần IP loa trước",
    c_need_token: "cần token trước",
    c_ip_hint: "IP hiện trên tile Hệ thống của loa, vd 192.168.1.50. Nên đặt IP cố định cho loa trên router.",
    c_ip_ok: "{} (firmware {})",
    c_err_not_meowbox: "IP này không phải MeowBox, hoặc loa chạy firmware cũ.",
    c_err_bad_address: "IP không hợp lệ. Nhập dạng 192.168.1.50.",
    c_err_other_network: "IP này không cùng mạng với máy tính.",
    c_err_no_answer: "Không có loa nào trả lời ở IP này.",
    c_reasons: "Hay gặp:\n• Máy tính và loa không cùng mạng Wi-Fi. Máy tính đang ở: {}\n• Mạng khách hoặc mạng quán cà phê chặn các máy nói chuyện với nhau: không dùng được.\n• Loa chưa có Wi-Fi: dùng điện thoại vào Wi-Fi cài đặt MeowBox-XXXX của loa.",
    c_not_wifi: "(không phải Wi-Fi hoặc không rõ)",
    c_wifi_help: "Hướng dẫn cài Wi-Fi cho loa",
    c_token_ok: "loa nhận token",
    c_token_bad: "Loa không nhận token này. Lấy token ở trang Thông báo của loa.",
    c_token_err: "Không kiểm tra được token: {}",
    c_open_notify: "Mở trang Thông báo",
    c_relay_ok: "loa gọi được ({} ms)",
    c_relay_fail: "Loa không gọi được relay trên máy này ({}).",
    c_relay_no_link: "Loa chưa nối với máy này nên chưa kiểm tra được relay. Xem dòng trên cùng của cửa sổ.",
    c_relay_wait: "đang chờ loa nối…",
    c_relay_public: "Windows xếp mạng này là Public nên tường lửa chặn relay.",
    c_allow: "Cho phép trên mạng này (cần quyền admin)",
    c_allowed: "Đã gửi yêu cầu thêm rule tường lửa. Bấm Kiểm tra lại.",
    c_port_busy: "Cổng relay {} đang bị app khác dùng. Chọn cổng khác:",
    c_port_save: "Lưu cổng",
    c_port_saved: "Đã lưu cổng {}. Thoát rồi mở lại MeowBox Agent để dùng cổng mới.",
    c_test: "Gửi thông báo thử",
    c_reconnect: "Nối lại loa ngay",

    s_agent: "MeowBox Agent",
    s_version: "bản {}",
    s_check: "Kiểm tra",
    s_checking: "đang kiểm tra…",
    s_no_release: "không kiểm tra được",
    s_has: "có bản {}",
    s_latest: "bản mới nhất",
    s_install: "Tải và cài",
    s_downloading: "Đang tải…",
    s_agent_failed: "Không tải được: {}. Mở GitHub để tải tay.",
    s_ytdlp: "yt-dlp",
    s_ytdlp_missing: "Chưa cài. YouTube cần nó (~24 MB).",
    s_update: "Cập nhật",
    s_cannot_check: "không kiểm tra được",
    s_ytdlp_failed: "Không cập nhật được: {}",
    s_ytdlp_installed: "Đã cài bản {}",
    s_ffmpeg: "ffmpeg",
    s_ff_download: "Tải ngay",
    s_ff_redownload: "Tải lại",
    s_ff_path: "bản trong PATH ({})",
    s_ff_own: "bản {} (agent đã tải)",
    s_ff_none: "Chưa có. Chỉ YouTube live cần nó; agent tự tải lần đầu (~75 MB).",
    s_ff_last_error: "Lần trước tải lỗi: {}",
    s_auto_update: "Kiểm tra bản mới",
    s_auto_update_box: "Mỗi ngày (agent và yt-dlp)",
    s_autostart: "Chạy cùng Windows",
    s_autostart_box: "Bật",
    s_port: "Cổng YouTube relay",
    s_save: "Lưu",
    s_port_range: "Cổng phải từ 1025 đến 65535.",
    s_port_saved: "Đã lưu. Thoát rồi mở lại MeowBox Agent để dùng cổng mới.",
    s_lang: "Ngôn ngữ",
    s_lang_auto: "Theo Windows",
    s_log_dir: "Mở thư mục log",
    s_github: "GitHub ↗",
    s_about: "Giới thiệu (v{})",
    s_mb: "{} / {} MB",
};

pub static EN: S = S {
    m_take_over: "Take over the speaker",
    m_open_window: "Open MeowBox Agent",
    m_open_web: "Open the speaker's page",
    m_pause: "Pause sending stats",
    m_coding: "Meow Coding (Claude Code)",
    m_new_version: "Version {} is out",
    m_quit: "Quit",

    st_connected: "Connected to {}",
    st_searching: "Looking for the speaker…",
    st_no_ip: "No speaker IP yet: open the window to enter it",
    st_lost: "Lost the speaker",
    st_lost_retry: "Lost the speaker, retrying in {} s",
    st_not_found: "Speaker not found",
    st_not_found_retry: "Speaker not found, retrying in {} s",
    st_token: "The speaker refused the token",
    st_busy: "{} is connected to {}",
    st_proto_agent: "The speaker needs a newer agent",
    st_proto_speaker: "The speaker needs newer firmware",
    st_port: "Relay port {} is used by another app",

    toast_title: "MeowBox",
    t_lost: "Lost {} for more than 2 minutes. The agent keeps retrying.",
    t_token: "The speaker refused this token. Get the token from the speaker's Notifications page.",
    t_taken_over: "{} is now connected to {}. Choose Take over the speaker in the menu to get it back.",
    t_test_sent: "Test notification sent",
    t_no_speaker: "Not connected to the speaker. Open MeowBox Agent to check.",
    t_speaker_error: "Could not send the test notification: the speaker answered {}.",
    t_send_failed: "Could not send the test notification: {}",
    t_coding_on: "Meow Coding is on. The cat is on duty! Start a new Claude Code session to see it on the speaker.",
    t_coding_off: "Meow Coding is off. The cat is off duty.",
    t_coding_failed: "Could not change ~/.claude/settings.json: {}",
    t_new_agent: "MeowBox Agent {} is out. Open Settings to update.",
    t_ffmpeg_start: "Downloading ffmpeg for YouTube live (once, about 75 MB)…",
    t_ffmpeg_done: "ffmpeg downloaded. YouTube live plays now.",
    t_ffmpeg_failed: "Could not download ffmpeg: {}. Try again in Settings.",
    test_title: "MeowBox Agent",
    test_text: "Meow! Hello from your PC.",

    app: "MeowBox Agent",
    tab_connect: "Connect",
    tab_settings: "Settings",

    mi_connected: "connected",
    mi_searching: "searching",
    mi_off: "not connected",
    mi_nothing_playing: "Nothing playing",
    mi_media_paused: "(paused)",
    mi_temp: "GPU temp",
    mi_paused: "Sending stats is paused.",
    mi_resume: "Resume",
    why_no_ip: "No speaker IP yet. Enter the IP shown on the speaker's System tile.",
    why_searching: "Looking for the speaker at {}…",
    why_not_found: "No speaker at {}. Check that it is on and on the same Wi-Fi as this PC.",
    why_lost: "Lost the speaker. The agent retries in {} s.",
    why_token: "The speaker refused this token. Get the token from the speaker's Notifications page.",
    why_busy: "{} is connected to {}. A speaker takes one PC at a time.",
    why_port: "Relay port {} is used by another app. Change the port, then restart the agent.",
    why_proto_agent: "The speaker needs a newer MeowBox Agent than this one.",
    why_proto_speaker: "The speaker runs old firmware. Update it on the speaker's page.",
    do_enter_ip: "Enter IP",
    do_retry: "Retry now",
    do_enter_token: "Enter token",
    do_take_over: "Take over the speaker",
    do_change_port: "Change port",
    do_see_update: "See the new version",
    do_open_web: "Open the speaker's page",

    c_ip: "Speaker IP",
    c_token: "Token",
    c_relay: "YouTube relay",
    c_check: "Check",
    c_checking: "checking…",
    c_need_ip: "needs the speaker IP first",
    c_need_token: "needs the token first",
    c_ip_hint: "The IP on the speaker's System tile, e.g. 192.168.1.50. Best give the speaker a fixed IP on the router.",
    c_ip_ok: "{} (firmware {})",
    c_err_not_meowbox: "This IP is not a MeowBox, or the speaker runs old firmware.",
    c_err_bad_address: "Not a valid IP. Enter it like 192.168.1.50.",
    c_err_other_network: "This IP is not on the same network as this PC.",
    c_err_no_answer: "No speaker answers at this IP.",
    c_reasons: "Common causes:\n• The PC and the speaker are on different Wi-Fi networks. This PC is on: {}\n• Guest and café networks stop devices from talking to each other: they cannot work.\n• The speaker has no Wi-Fi yet: join its MeowBox-XXXX setup Wi-Fi with a phone.",
    c_not_wifi: "(not Wi-Fi, or unknown)",
    c_wifi_help: "How to set up the speaker's Wi-Fi",
    c_token_ok: "accepted",
    c_token_bad: "The speaker refused this token. Get the token from the speaker's Notifications page.",
    c_token_err: "Could not check the token: {}",
    c_open_notify: "Open the Notifications page",
    c_relay_ok: "the speaker reaches it ({} ms)",
    c_relay_fail: "The speaker cannot reach the relay on this PC ({}).",
    c_relay_no_link: "The speaker is not connected to this PC, so the relay cannot be checked. See the top of the window.",
    c_relay_wait: "waiting for the speaker…",
    c_relay_public: "Windows calls this network Public, so the firewall blocks the relay.",
    c_allow: "Allow on this network (needs admin)",
    c_allowed: "Asked Windows to add the firewall rule. Click Check again.",
    c_port_busy: "Relay port {} is used by another app. Pick another port:",
    c_port_save: "Save port",
    c_port_saved: "Saved port {}. Quit and start MeowBox Agent again to use it.",
    c_test: "Send a test notification",
    c_reconnect: "Reconnect now",

    s_agent: "MeowBox Agent",
    s_version: "version {}",
    s_check: "Check",
    s_checking: "checking…",
    s_no_release: "could not check",
    s_has: "{} is out",
    s_latest: "up to date",
    s_install: "Download and install",
    s_downloading: "Downloading…",
    s_agent_failed: "Download failed: {}. Get it from GitHub instead.",
    s_ytdlp: "yt-dlp",
    s_ytdlp_missing: "Not installed. YouTube needs it (~24 MB).",
    s_update: "Update",
    s_cannot_check: "could not check",
    s_ytdlp_failed: "Update failed: {}",
    s_ytdlp_installed: "Installed {}",
    s_ffmpeg: "ffmpeg",
    s_ff_download: "Download now",
    s_ff_redownload: "Download again",
    s_ff_path: "the one on PATH ({})",
    s_ff_own: "version {} (downloaded by the agent)",
    s_ff_none: "Not there. Only YouTube live needs it; the agent downloads it the first time (~75 MB).",
    s_ff_last_error: "Last download failed: {}",
    s_auto_update: "Check for updates",
    s_auto_update_box: "Daily (agent and yt-dlp)",
    s_autostart: "Run at Windows start",
    s_autostart_box: "On",
    s_port: "YouTube relay port",
    s_save: "Save",
    s_port_range: "The port must be 1025 to 65535.",
    s_port_saved: "Saved. Quit and start MeowBox Agent again to use the new port.",
    s_lang: "Language",
    s_lang_auto: "Same as Windows",
    s_log_dir: "Open the log folder",
    s_github: "GitHub ↗",
    s_about: "About (v{})",
    s_mb: "{} / {} MB",
};

static ENGLISH: AtomicBool = AtomicBool::new(false);

/// The table for the language in use.
pub fn s() -> &'static S {
    if ENGLISH.load(Ordering::Relaxed) { &EN } else { &VI }
}

/// "auto" (Windows' display language: Vietnamese → vi, anything else → en), "vi" or "en".
pub fn set_lang(setting: &str) {
    let en = match setting {
        "vi" => false,
        "en" => true,
        _ => !windows_is_vietnamese(),
    };
    ENGLISH.store(en, Ordering::Relaxed);
}

fn windows_is_vietnamese() -> bool {
    // SAFETY: plain query.
    let id = unsafe { windows::Win32::Globalization::GetUserDefaultUILanguage() };
    id & 0x3FF == 0x2A // LANG_VIETNAMESE
}

/// A template with its `{}` filled in order.
pub fn f(t: &str, args: &[&dyn std::fmt::Display]) -> String {
    let mut out = String::with_capacity(t.len() + 16);
    let mut it = args.iter();
    let mut rest = t;
    while let Some(i) = rest.find("{}") {
        out.push_str(&rest[..i]);
        if let Some(a) = it.next() {
            out.push_str(&a.to_string());
        }
        rest = &rest[i + 2..];
    }
    out.push_str(rest);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn both_languages_take_the_same_arguments() {
        for ((k, vi), (_, en)) in VI.entries().into_iter().zip(EN.entries()) {
            assert!(!vi.is_empty() && !en.is_empty(), "{k} is empty");
            assert_eq!(vi.matches("{}").count(), en.matches("{}").count(), "{k}: {vi:?} vs {en:?}");
        }
    }

    #[test]
    fn templates_fill_in_order() {
        assert_eq!(f(VI.st_busy, &[&"MeowBox-1A2B", &"DESKTOP-ABC"]), "MeowBox-1A2B đang nối với DESKTOP-ABC");
        assert_eq!(f("{} / {} MB", &[&3, &75]), "3 / 75 MB");
        assert_eq!(f("no args", &[]), "no args");
    }

    /// The status line fits the tray menu (B3): about 45 characters, names aside.
    #[test]
    fn status_lines_are_short() {
        for t in [&VI, &EN] {
            for (k, v) in t.entries().into_iter().filter(|(k, _)| k.starts_with("st_")) {
                let n = f(v, &[&"300", &"300"]).chars().count();
                assert!(n <= 47, "{k}: {v:?} is {n} characters");
            }
        }
    }
}
