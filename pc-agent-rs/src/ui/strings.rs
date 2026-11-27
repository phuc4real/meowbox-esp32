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
