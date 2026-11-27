//! Tray app (MeowBoxAgent.exe). Threads: the main thread runs the Win32 message loop that owns the tray icon and,
//! when asked, an eframe window (Connect / Settings pages) that returns when closed. The core (phase 1) will run in
//! its own tokio thread and wake the main thread through the tray's Waker.

pub mod connect;
pub mod fonts;
pub mod icon;
pub mod settings;
pub mod strings;
pub mod task;
pub mod tray;

use crate::config::Config;
use crate::link::CoreHandle;
use crate::state::{AgentState, LinkError};
use eframe::egui;
use std::collections::VecDeque;
use std::sync::{Arc, Mutex};
use strings as s;
use tray::{MenuItem, Tray, TrayEvent};

/// Tray status line and dot colour, word for word as `status_line()` in the Python tray.
pub fn status_line(st: &AgentState, cfg: &Config) -> (String, (u8, u8, u8)) {
    let want = [st.name.as_str(), cfg.speaker.as_str()].into_iter().find(|x| !x.is_empty()).unwrap_or("MeowBox");
    let how = match st.how.as_str() {
        "last" => s::HOW_LAST,
        "manual" => s::HOW_MANUAL,
        other => other,
    };
    if st.connected {
        return (format!("● Đã kết nối {} · {} ({how})", st.name, st.ip()), icon::GREEN);
    }
    let red = |t: String| (t, icon::RED);
    match st.error {
        LinkError::Busy => return red(format!("✕ {want} đang nối với {}", st.busy_with)),
        LinkError::Token => return red("✕ Loa cần token đúng: mở 'Kết nối loa…', bước 2".into()),
        LinkError::ProtoAgent => {
            return red("✕ Loa cần MeowBox Agent mới hơn: bấm 'Có bản mới' hoặc vào GitHub".into());
        }
        LinkError::Port => {
            return red(format!("✕ Cổng relay {} đang bị app khác dùng: mở 'Kết nối loa…'", cfg.relay_port));
        }
        _ => {}
    }
    if !st.searching.is_empty() {
        let step = if st.searching == "start" { "…" } else { st.searching.as_str() };
        return (format!("○ Đang nối loa: {step}"), icon::GREY);
    }
    if st.error == LinkError::NotFound {
        let retry = if st.retry_in > 0 { format!(" (thử lại sau {} s)", st.retry_in) } else { String::new() };
        if cfg.last_host.is_empty() && cfg.host.is_empty() {
            return red("✕ Chưa có IP loa: mở 'Kết nối loa…' để nhập".into());
        }
        return red(format!("✕ Không nối được {want}, mở 'Kết nối loa…' để kiểm tra IP{retry}"));
    }
    ("○ Đang nối loa…".into(), icon::GREY)
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Page {
    Connect,
    Settings,
}

mod id {
    pub const TAKE_OVER: u32 = 1;
    pub const OPEN_WEB: u32 = 2;
    pub const TEST_NOTIFY: u32 = 3;
    pub const PAUSE: u32 = 4;
    pub const CODING: u32 = 5;
    pub const CONNECT: u32 = 6;
    pub const SETTINGS: u32 = 7;
    pub const RECONNECT: u32 = 8;
    pub const AUTOSTART: u32 = 9;
    pub const LOG_DIR: u32 = 10;
    pub const NEW_VERSION: u32 = 11;
    pub const GITHUB: u32 = 12;
    pub const ABOUT: u32 = 13;
    pub const QUIT: u32 = 14;
}

pub struct TrayApp {
    pub core: CoreHandle,
    tray: Tray,
    /// Toasts queued by other threads (shown on the next wake).
    toasts: Arc<Mutex<VecDeque<String>>>,
    last_line: String,
    /// A page the user asked for; opened by the outer loop, or switched to if a dialog is already open.
    open_request: Option<Page>,
    quit: bool,
    /// Dev check for the Phase 0 spike: open and close the dialog this many times by itself.
    selftest_reopen: u32,
    pub dialogs_opened: u32,
}

impl TrayApp {
    pub fn new(core: CoreHandle) -> anyhow::Result<Self> {
        let size = tray::tray_icon_size();
        let tray = Tray::new(tray::make_icon(&icon::cat_rgba(size, Some(icon::GREY)), size), s::APP)?;
        let waker = tray.waker();
        core.on_state_change(move || waker.wake());
        let toasts: Arc<Mutex<VecDeque<String>>> = Arc::default();
        let (q, w) = (toasts.clone(), tray.waker());
        core.set_toast(move |text| {
            q.lock().unwrap_or_else(|e| e.into_inner()).push_back(text);
            w.wake();
        });
        let mut app = Self {
            core,
            tray,
            toasts,
            last_line: String::new(),
            open_request: None,
            quit: false,
            selftest_reopen: 0,
            dialogs_opened: 0,
        };
        app.refresh();
        if app.cfg().speaker.is_empty() {
            app.open_request = Some(Page::Connect); // first run: the connect window opens by itself
        }
        Ok(app)
    }

    pub fn cfg(&self) -> Config {
        self.core.cfg().get()
    }

    pub fn state(&self) -> AgentState {
        self.core.state().get()
    }

    /// A toast from any thread.
    fn toaster(&self) -> impl Fn(String) + Send + Sync + 'static {
        let (q, w) = (self.toasts.clone(), self.tray.waker());
        move |text| {
            q.lock().unwrap_or_else(|e| e.into_inner()).push_back(text);
            w.wake();
        }
    }

    pub fn open_page(&mut self, page: Page) {
        self.open_request = Some(page);
    }

    pub fn selftest_reopen(&mut self, n: u32) {
        self.selftest_reopen = n;
        if n > 0 {
            self.open_request = Some(Page::Connect);
        }
    }

    /// Icon dot and tooltip follow the state.
    fn refresh(&mut self) {
        let st = self.state();
        let (line, color) = status_line(&st, &self.cfg());
        if line == self.last_line {
            return;
        }
        let size = tray::tray_icon_size();
        let ip = if st.ip().is_empty() { "—" } else { st.ip() };
        let tail = if st.connected {
            s::CONNECTED.to_owned()
        } else {
            line.trim_start_matches(['●', '○', '✕', ' ']).to_owned()
        };
        let tip = format!("{} · {ip} · {tail}", if st.name.is_empty() { "MeowBox" } else { &st.name });
        self.tray.set(tray::make_icon(&icon::cat_rgba(size, Some(color)), size), &tip);
        self.last_line = line;
    }

    fn menu(&self) -> Vec<MenuItem> {
        let st = self.state();
        let cfg = self.cfg();
        let mut m = vec![MenuItem::label(status_line(&st, &cfg).0)];
        if st.error == LinkError::Busy {
            m.push(MenuItem::item(id::TAKE_OVER, s::M_TAKE_OVER));
        }
        m.extend([
            MenuItem::Separator,
            MenuItem::default(id::OPEN_WEB, s::M_OPEN_WEB),
            MenuItem::item(id::TEST_NOTIFY, s::M_TEST_NOTIFY),
            MenuItem::check(id::PAUSE, s::M_PAUSE, cfg.paused),
            MenuItem::check(id::CODING, s::M_CODING, crate::coding::hooks::hooks_installed()),
            MenuItem::Separator,
            MenuItem::item(id::CONNECT, s::M_CONNECT),
            MenuItem::item(id::SETTINGS, s::M_SETTINGS),
            MenuItem::item(id::RECONNECT, s::M_RECONNECT),
            MenuItem::check(id::AUTOSTART, s::M_AUTOSTART, crate::win::autostart_enabled()),
            MenuItem::item(id::LOG_DIR, s::M_LOG_DIR),
            MenuItem::Separator,
        ]);
        if !st.new_version.is_empty() {
            m.push(MenuItem::item(id::NEW_VERSION, s::m_new_version(&st.new_version)));
        }
        m.extend([
            MenuItem::item(id::GITHUB, s::M_GITHUB),
            MenuItem::item(id::ABOUT, s::m_about(crate::VERSION)),
            MenuItem::item(id::QUIT, s::M_QUIT),
        ]);
        m
    }

    fn open_web(&mut self) {
        let st = self.state();
        let h = if st.host.is_empty() { self.cfg().known_host().to_owned() } else { st.host };
        if h.is_empty() {
            self.open_request = Some(Page::Connect);
        } else {
            crate::win::open(&format!("http://{h}/"));
        }
    }

    fn command(&mut self, cmd: u32) {
        match cmd {
            id::OPEN_WEB => self.open_web(),
            id::TAKE_OVER => self.core.reconnect_now(true),
            id::RECONNECT => self.core.reconnect_now(false),
            id::TEST_NOTIFY => self.test_notify(),
            id::PAUSE => self.core.cfg().update(|c| c.paused = !c.paused),
            id::CODING => self.toggle_coding(),
            id::AUTOSTART => {
                if let Err(e) = crate::win::set_autostart(!crate::win::autostart_enabled()) {
                    tracing::warn!("autostart: {e}");
                }
            }
            id::CONNECT => self.open_request = Some(Page::Connect),
            id::SETTINGS => self.open_request = Some(Page::Settings),
            id::LOG_DIR => crate::win::open(&crate::app_dir().to_string_lossy()),
            id::NEW_VERSION => crate::win::open(&format!("https://github.com/{}/releases", crate::REPO)),
            id::GITHUB => crate::win::open(&format!("https://github.com/{}", crate::REPO)),
            id::ABOUT => crate::win::open(&format!("https://github.com/{}#readme", crate::REPO)),
            id::QUIT => {
                tracing::info!("quit");
                self.quit = true;
                self.core.stop();
            }
            other => tracing::debug!("menu item {other}: not in this build yet"),
        }
    }

    /// Add / remove MeowBox's hooks and status line in ~/.claude/settings.json (new Claude Code sessions pick them up).
    fn toggle_coding(&self) {
        let r = if crate::coding::hooks::hooks_installed() {
            crate::coding::hooks::uninstall().map(|_| s::T_CODING_OFF)
        } else {
            crate::coding::hooks::install(self.cfg().coding_port).map(|_| s::T_CODING_ON)
        };
        match r {
            Ok(msg) => self.tray.notify(msg, s::TOAST_TITLE),
            Err(e) => {
                tracing::warn!("Claude Code settings: {e}");
                self.tray.notify(&s::t_coding_failed(&e), s::TOAST_TITLE);
            }
        }
    }

    /// "Gửi thông báo thử": a test notification through the speaker's API, toast on failure.
    fn test_notify(&self) {
        let (host, token) = (self.state().host, self.cfg().token);
        let toast = self.toaster();
        if host.is_empty() || token.is_empty() {
            toast(s::T_NO_SPEAKER.into());
            return;
        }
        self.core.spawn(async move {
            let body = serde_json::json!({"title": "MeowBox Agent", "text": s::TEST_NOTIFY_TEXT, "sound": "chime"});
            match crate::speaker_api::notify(&host, &token, &body).await {
                Ok((code, _)) if code >= 400 => toast(s::t_speaker_error(code)),
                Ok(_) => {}
                Err(e) => toast(s::t_send_failed(&e)),
            }
        });
    }

    /// Handle everything queued by the tray window. Safe to call from the dialog's frame: it never opens a dialog
    /// itself, it only records `open_request`.
    fn process(&mut self) {
        while let Some(ev) = tray::pop() {
            match ev {
                TrayEvent::Click => self.open_web(),
                TrayEvent::Menu(x, y) => {
                    let items = self.menu();
                    if let Some(cmd) = self.tray.popup(&items, x, y) {
                        self.command(cmd);
                    }
                }
                TrayEvent::Wake => {
                    self.refresh();
                    let q: Vec<String> = self.toasts.lock().unwrap_or_else(|e| e.into_inner()).drain(..).collect();
                    for t in q {
                        self.tray.notify(&t, s::TOAST_TITLE);
                    }
                }
                TrayEvent::TaskbarCreated => self.tray.add(),
            }
        }
    }

    /// The main loop: messages, queued tray events, and dialogs when asked. Returns on Quit.
    pub fn run(&mut self) {
        loop {
            if let Some(page) = self.open_request.take() {
                self.run_dialog(page);
                self.process(); // events that arrived while the dialog was closing
            }
            if self.quit || !tray::pump_one() {
                break;
            }
            self.process();
            if self.quit {
                break;
            }
        }
    }

    fn run_dialog(&mut self, page: Page) {
        self.dialogs_opened += 1;
        let icon = egui::IconData { rgba: icon::cat_rgba(32, None), width: 32, height: 32 };
        let options = eframe::NativeOptions {
            viewport: egui::ViewportBuilder::default()
                .with_title(s::APP)
                .with_inner_size([580.0, 560.0])
                .with_resizable(false)
                .with_maximize_button(false)
                .with_icon(icon),
            centered: true,
            run_and_return: true,
            ..Default::default()
        };
        let auto_close = self.selftest_reopen > 0;
        let app: &mut TrayApp = &mut *self;
        let r = eframe::run_native(
            s::APP,
            options,
            Box::new(move |cc| {
                cc.egui_ctx.set_fonts(fonts::definitions());
                cc.egui_ctx.set_theme(egui::Theme::Light);
                style(&cc.egui_ctx);
                tray::set_dialog_ctx(Some(cc.egui_ctx.clone()));
                let (connect, settings) = (connect::Connect::new(&app.core), settings::Settings::new(&app.core));
                Ok(Box::new(Dialog { app, page, connect, settings, frames: 0, auto_close }))
            }),
        );
        tray::set_dialog_ctx(None);
        crate::win::trim_memory();
        if let Err(e) = r {
            tracing::error!("dialog failed: {e}");
        }
        if self.selftest_reopen > 0 {
            self.selftest_reopen -= 1;
            if self.selftest_reopen > 0 {
                self.open_request =
                    Some(if self.selftest_reopen.is_multiple_of(2) { Page::Connect } else { Page::Settings });
            } else {
                self.quit = true;
            }
        }
    }
}

/// Text colours of the Python dialogs: ok #080, error #b00, hints #666.
#[derive(Clone, Copy)]
pub enum Look {
    Good,
    Bad,
    Hint,
    Text,
}

impl Look {
    pub fn color(self) -> egui::Color32 {
        match self {
            Self::Good => egui::Color32::from_rgb(0x00, 0x88, 0x00),
            Self::Bad => egui::Color32::from_rgb(0xbb, 0x00, 0x00),
            Self::Hint => egui::Color32::from_rgb(0x66, 0x66, 0x66),
            Self::Text => egui::Color32::from_rgb(0x55, 0x55, 0x55),
        }
    }
}

pub fn msg(text: &str, look: Look) -> Option<(String, egui::Color32)> {
    Some((text.to_owned(), look.color()))
}

pub fn hint(ui: &mut egui::Ui, text: &str) {
    if !text.is_empty() {
        ui.label(egui::RichText::new(text).color(Look::Hint.color()));
    }
}

/// Windows-like sizes: 14 px Segoe UI, roomier buttons and fields.
fn style(ctx: &egui::Context) {
    ctx.all_styles_mut(|st| {
        use egui::{FontFamily::Proportional, FontId, TextStyle};
        st.text_styles = [
            (TextStyle::Heading, FontId::new(18.0, Proportional)),
            (TextStyle::Body, FontId::new(14.0, Proportional)),
            (TextStyle::Button, FontId::new(14.0, Proportional)),
            (TextStyle::Small, FontId::new(12.0, Proportional)),
            (TextStyle::Monospace, FontId::new(13.0, egui::FontFamily::Monospace)),
        ]
        .into();
        st.spacing.item_spacing = egui::vec2(8.0, 8.0);
        st.spacing.button_padding = egui::vec2(12.0, 5.0);
        st.spacing.interact_size.y = 28.0;
    });
}

/// The one agent window. Borrows the tray app for as long as it is open (eframe's run_and_return).
struct Dialog<'a> {
    app: &'a mut TrayApp,
    page: Page,
    connect: connect::Connect,
    settings: settings::Settings,
    frames: u32,
    auto_close: bool,
}

impl Dialog<'_> {
    fn close(&self, ctx: &egui::Context) {
        self.connect.close(&self.app.core);
        ctx.send_viewport_cmd(egui::ViewportCommand::Close);
    }
}

impl eframe::App for Dialog<'_> {
    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        // tray events keep working while the window is open
        self.app.process();
        if let Some(p) = self.app.open_request.take() {
            self.page = p;
            ui.ctx().send_viewport_cmd(egui::ViewportCommand::Focus);
        }
        if self.app.quit {
            self.close(ui.ctx());
        }
        self.frames += 1;
        if self.auto_close && self.frames > 3 {
            self.close(ui.ctx());
        }
        if ui.ctx().input(|i| i.viewport().close_requested()) {
            self.connect.close(&self.app.core);
        }

        let frame = egui::Frame::central_panel(ui.style()).inner_margin(egui::Margin::symmetric(16, 12));
        egui::CentralPanel::default().frame(frame).show(ui, |ui| {
            ui.horizontal(|ui| {
                ui.selectable_value(&mut self.page, Page::Connect, s::PAGE_CONNECT);
                ui.selectable_value(&mut self.page, Page::Settings, s::PAGE_SETTINGS);
            });
            ui.separator();
            match self.page {
                Page::Connect => {
                    self.connect.ui(ui, &self.app.core);
                    if self.connect.finished {
                        self.close(ui.ctx());
                    }
                }
                Page::Settings => {
                    egui::Panel::bottom("settings_nav").show_separator_line(false).show(ui, |ui| {
                        ui.add_space(6.0);
                        ui.horizontal(|ui| {
                            if ui.button(s::M_LOG_DIR).clicked() {
                                crate::win::open(&crate::app_dir().to_string_lossy());
                            }
                            if ui.button(s::M_GITHUB).clicked() {
                                crate::win::open(&format!("https://github.com/{}", crate::REPO));
                            }
                            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                                if ui.button(s::CLOSE).clicked() {
                                    self.close(ui.ctx());
                                }
                            });
                        });
                    });
                    let core = &self.app.core;
                    let act = egui::CentralPanel::default()
                        .frame(egui::Frame::NONE)
                        .show(ui, |ui| egui::ScrollArea::vertical().show(ui, |ui| self.settings.ui(ui, core)).inner)
                        .inner;
                    if let Some(settings::Action::Quit) = act {
                        self.app.quit = true;
                        self.app.core.stop();
                        self.close(ui.ctx());
                    }
                }
            }
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn status_lines_match_python() {
        let cfg = Config::default();
        let mut st = AgentState::default();
        assert_eq!(status_line(&st, &cfg).0, "○ Đang nối loa…");
        st.error = LinkError::NotFound;
        assert_eq!(status_line(&st, &cfg).0, "✕ Chưa có IP loa: mở 'Kết nối loa…' để nhập");
        let mut cfg2 = Config::default();
        cfg2.last_host = "192.168.1.50".into();
        st.retry_in = 8;
        assert_eq!(
            status_line(&st, &cfg2).0,
            "✕ Không nối được MeowBox, mở 'Kết nối loa…' để kiểm tra IP (thử lại sau 8 s)"
        );
        st = AgentState {
            connected: true,
            name: "MeowBox-1A2B".into(),
            host: "192.168.1.50".into(),
            how: "last".into(),
            ..st
        };
        assert_eq!(
            status_line(&st, &cfg2),
            ("● Đã kết nối MeowBox-1A2B · 192.168.1.50 (IP lần trước)".into(), icon::GREEN)
        );
    }
}
