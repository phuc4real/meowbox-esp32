//! UI strings (Vietnamese, as in the Python agent). One place, so an English UI can come later.

pub const APP: &str = "MeowBox Agent";
pub const TOAST_TITLE: &str = "MeowBox";

pub const M_TAKE_OVER: &str = "Giành quyền nối loa";
pub const M_OPEN_WEB: &str = "Mở trang cài đặt MeowBox";
pub const M_TEST_NOTIFY: &str = "Gửi thông báo thử";
pub const M_PAUSE: &str = "Tạm dừng gửi số liệu";
pub const M_CODING: &str = "Meow Coding (Claude Code)";
pub const M_CONNECT: &str = "Kết nối loa…";
pub const M_SETTINGS: &str = "Cài đặt…";
pub const M_RECONNECT: &str = "Nối lại loa ngay";
pub const M_AUTOSTART: &str = "Chạy cùng Windows";
pub const M_LOG_DIR: &str = "Mở thư mục log";
pub const M_GITHUB: &str = "GitHub ↗";
pub const M_QUIT: &str = "Thoát";

pub fn m_new_version(v: &str) -> String {
    format!("Có bản {v} ↗")
}

pub fn m_about(v: &str) -> String {
    format!("Giới thiệu (v{v})")
}

pub const HOW_LAST: &str = "IP lần trước";
pub const HOW_MANUAL: &str = "IP nhập tay";
pub const CONNECTED: &str = "đã kết nối";

pub const PAGE_CONNECT: &str = "Kết nối loa";
pub const PAGE_SETTINGS: &str = "Cài đặt";
pub const CLOSE: &str = "Đóng";

pub const TEST_NOTIFY_TEXT: &str = "Thông báo thử từ máy tính";
pub const T_NO_SPEAKER: &str = "Chưa nối loa hoặc chưa có token: mở 'Kết nối loa…'";

pub fn t_speaker_error(code: u16) -> String {
    format!("Loa trả lỗi {code} (token sai?)")
}

pub fn t_send_failed(e: &dyn std::fmt::Display) -> String {
    format!("Không gửi được: {e}")
}

pub const T_CODING_OFF: &str = "Mèo thôi canh Claude Code rồi (đã gỡ hook)";
pub const T_CODING_ON: &str = "Mèo vào vị trí! Mở phiên Claude Code mới để mèo code cùng sen";

pub fn t_coding_failed(e: &dyn std::fmt::Display) -> String {
    format!("Không sửa được ~/.claude/settings.json: {e}")
}

/// "Kết nối loa" wizard (connect_dialog.py).
pub mod connect {
    pub const WIFI_HELP: &str = "https://github.com/phuc4real/meowbox-esp32#readme";
    pub const STEPS: [&str; 3] = ["1 IP loa", "2 Token", "3 Kiểm tra"];
    pub const IP: &str = "Nhập IP hiện trên màn hình loa (tile Hệ thống), vd 192.168.1.50:";
    pub const IP_TIP: &str = "Agent không tự dò loa trong mạng. Loa đổi IP thì nhập lại ở đây; nên đặt IP cố định cho \
                              loa trên router (DHCP reservation).";
    pub const TRY: &str = "Thử";
    pub fn ok_ip(name: &str, ver: &str) -> String {
        format!("✓ {name} (firmware {ver})")
    }
    pub fn error(key: &str) -> &'static str {
        match key {
            "not_meowbox" => "IP này không phải MeowBox (hoặc loa đang chạy firmware cũ).",
            "bad_address" => "IP không hợp lệ.",
            "other_network" => "IP này không cùng mạng với máy tính.",
            _ => "Không nối được tới IP này.",
        }
    }
    pub fn reasons(wifi: &str) -> String {
        format!(
            "Lý do hay gặp:\n• Máy tính và loa không cùng mạng Wi-Fi. Máy tính đang ở: {wifi}\n\
             • Mạng khách / quán cà phê chặn các máy nói chuyện với nhau (client isolation): không dùng được.\n\
             • Loa chưa có Wi-Fi: vào Wi-Fi cài đặt MeowBox-XXXX của loa bằng điện thoại."
        )
    }
    pub const NOT_WIFI: &str = "(không phải Wi-Fi / không rõ)";
    pub const WIFI_HELP_BTN: &str = "Hướng dẫn cài Wi-Fi cho loa";
    pub const NEXT: &str = "Tiếp ›";
    pub const BACK: &str = "‹ Quay lại";
    pub const FINISH: &str = "Xong";
    pub const TOKEN_INTRO: &str = "Loa chỉ nhận máy tính có token đúng: số liệu máy, điều khiển nhạc, YouTube, thông \
                                   báo, đọc chữ (say) và Wi-Fi cài đặt đều cần nó. Token ở trang Thông báo trên loa.";
    pub const TOKEN_NEED: &str = "Nhập token rồi bấm Kiểm tra trước khi qua bước sau.";
    pub const OPEN_NOTIFY: &str = "Mở trang Thông báo trên loa";
    pub const TOKEN: &str = "Token:";
    pub const CHECK: &str = "Kiểm tra";
    pub const TOKEN_OK: &str = "✓ Token đúng";
    pub const TOKEN_BAD: &str = "Token sai";
    pub fn token_err(e: &str) -> String {
        format!("Không kiểm tra được ({e})")
    }
    pub fn relay_intro(port: u16) -> String {
        format!("Kiểm tra loa có gọi được YouTube relay trên máy này không (cổng {port}).")
    }
    pub const RELAY_PORT_BUSY: &str = "Cổng relay đang bị app khác dùng. Chọn cổng khác rồi khởi động lại agent:";
    pub const RELAY_RUN: &str = "Kiểm tra relay";
    pub const RELAY_WAIT: &str = "Đang chờ loa kết nối…";
    pub fn relay_ok(ms: &str) -> String {
        format!("✓ Loa gọi được relay ({ms} ms). YouTube sẽ phát được.")
    }
    pub fn relay_fail(err: &str) -> String {
        format!("Loa không gọi được relay ({err}).")
    }
    pub const RELAY_NEED_TOKEN: &str = "Cần token (bước 2) để kiểm tra relay.";
    pub const PUBLIC: &str = "Windows đang xếp mạng này là Public, tường lửa chặn relay.";
    pub const ALLOW: &str = "Cho phép trên mạng này (cần quyền admin)";
    pub const ALLOWED: &str = "Đã gửi yêu cầu thêm rule tường lửa. Bấm Kiểm tra relay lại.";
    pub fn saved_port(port: u16) -> String {
        format!("Đã lưu cổng {port}. Thoát và mở lại MeowBox Agent để dùng cổng mới.")
    }
}

/// "Cài đặt" page (settings_dialog.py).
pub mod settings {
    pub const AGENT: &str = "MeowBox Agent";
    pub fn version(v: &str) -> String {
        format!("Phiên bản {v}")
    }
    pub const CHECK: &str = "Kiểm tra";
    pub const CHECKING: &str = "đang kiểm tra…";
    pub const NO_RELEASE: &str = "không kiểm tra được (chưa có bản phát hành?)";
    pub fn has(v: &str) -> String {
        format!("có bản {v}")
    }
    pub const LATEST: &str = "đang là bản mới nhất";
    pub const INSTALL: &str = "Tải & cài";
    pub fn downloading_agent(v: &str) -> String {
        format!("Đang tải bản {v}…")
    }
    pub fn agent_download_failed(e: &str) -> String {
        format!("Không tải được ({e}). Mở trang GitHub để tải tay.")
    }
    pub const YTDLP: &str = "yt-dlp (YouTube)";
    pub const YTDLP_MISSING: &str = "Chưa cài: bấm Cập nhật để tải (~24 MB)";
    pub const UPDATE: &str = "Cập nhật";
    pub const CANNOT_CHECK: &str = "không kiểm tra được";
    pub const DOWNLOADING_YTDLP: &str = "Đang tải yt-dlp…";
    pub fn ytdlp_failed(e: &str) -> String {
        format!("Không cập nhật được: {e}")
    }
    pub fn ytdlp_installed(v: &str) -> String {
        format!("Đã cài yt-dlp {v}")
    }
    pub const FFMPEG: &str = "ffmpeg (chỉ cho YouTube live)";
    pub const FF_DOWNLOAD: &str = "Tải ngay";
    pub const FF_REDOWNLOAD: &str = "Tải bản mới";
    pub fn ff_busy(pct: u32) -> String {
        format!("Đang tải… {pct}%")
    }
    pub fn ff_path(v: &str) -> String {
        format!("Dùng ffmpeg có sẵn trong PATH ({v})")
    }
    pub fn ff_own(v: &str) -> String {
        format!("Phiên bản {v} (bản LGPL agent đã tải)")
    }
    pub const FF_NONE: &str = "Chưa có. Agent tự tải lần đầu phát YouTube live (~75 MB).";
    pub fn ff_last_error(e: &str) -> String {
        format!("Lần trước lỗi: {e}")
    }
    pub const OPTIONS: &str = "Tùy chọn";
    pub const AUTO_UPDATE: &str = "Tự kiểm tra bản mới mỗi ngày (agent, yt-dlp)";
    pub const PORT: &str = "Cổng YouTube relay:";
    pub const SAVE: &str = "Lưu";
    pub const PORT_RANGE: &str = "Cổng phải từ 1025 đến 65535.";
    pub const PORT_SAVED: &str = "Đã lưu. Thoát và mở lại MeowBox Agent để dùng cổng mới.";
}

pub fn t_new_agent(v: &str) -> String {
    format!("Có MeowBox Agent {v}: mở Cài đặt để cập nhật")
}
