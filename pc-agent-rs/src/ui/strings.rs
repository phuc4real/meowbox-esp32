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
