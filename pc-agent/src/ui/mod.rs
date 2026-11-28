//! Tray app (MeowBoxAgent.exe). The main thread runs the Win32 message loop that owns the tray icon and, when asked,
//! the one agent window (eframe, returns when closed); the core runs on its own tokio threads and wakes the main
//! thread through the tray's Waker.
//!
//! The window (docs/enhance_agent.md, part B): a mirror of the speaker's Computer screen on top, then two tabs,
//! Kết nối (three self-checking rows) and Cài đặt (one list), in the speaker's theme, light or dark as Windows is.

pub mod connect;
pub mod icon;
pub mod mirror;
pub mod notice;
pub mod settings;
pub mod strings;
pub mod task;
pub mod theme;
#[rustfmt::skip]
pub mod themes_gen;
pub mod tray;
pub mod widgets;

use crate::config::Config;
use crate::link::CoreHandle;
use crate::state::{AgentState, LinkError, ToastKind};
use eframe::egui;
use std::collections::VecDeque;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};
use strings::{f, s};
use tray::{MenuItem, Tray, TrayEvent};

/// Tray status line and icon state: what is going on and what to do, in at most ~45 characters (B3). The icon's
/// eyes and dot repeat it; the words carry it. `ever_connected`: the speaker was connected earlier in this run, so a
/// failed search means it was lost rather than never found.
pub fn status_line(st: &AgentState, cfg: &Config, ever_connected: bool) -> (String, icon::State) {
    let t = s();
    if st.connected {
        return (f(t.st_connected, &[&st.name]), icon::State::Connected);
    }
    let want = [st.name.as_str(), cfg.speaker.as_str()].into_iter().find(|x| !x.is_empty()).unwrap_or("MeowBox");
    let bad = |text: String| (text, icon::State::Error);
    match st.error {
        LinkError::Busy => return bad(f(t.st_busy, &[&want, &st.busy_with])),
        LinkError::Token => return bad(t.st_token.into()),
        LinkError::ProtoAgent => return bad(t.st_proto_agent.into()),
        LinkError::ProtoSpeaker => return bad(t.st_proto_speaker.into()),
        LinkError::Port => return bad(f(t.st_port, &[&cfg.relay_port])),
        LinkError::NotFound if st.searching.is_empty() => {
            if cfg.last_host.is_empty() && cfg.host.is_empty() {
                return bad(t.st_no_ip.into());
            }
            let n = st.retry_in;
            return bad(match (ever_connected, n > 0) {
                (true, true) => f(t.st_lost_retry, &[&n]),
                (true, false) => t.st_lost.into(),
                (false, true) => f(t.st_not_found_retry, &[&n]),
                (false, false) => t.st_not_found.into(),
            });
        }
        _ => {}
    }
    (t.st_searching.into(), icon::State::Connecting)
}

/// Tray icon size (px) and state.
type IconKey = (u32, icon::State);

/// The key to draw when the icon must change, None when the one in the tray still fits.
fn new_icon_key(current: Option<IconKey>, size: u32, state: icon::State) -> Option<IconKey> {
    let key = (size, state);
    (current != Some(key)).then_some(key)
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Page {
    Connect,
    Settings,
}

mod id {
    pub const TAKE_OVER: u32 = 1;
    pub const OPEN_WINDOW: u32 = 2;
    pub const OPEN_WEB: u32 = 3;
    pub const PAUSE: u32 = 4;
    pub const CODING: u32 = 5;
    pub const NEW_VERSION: u32 = 6;
    pub const QUIT: u32 = 7;
}

/// A toast waiting for the main thread: from the core (throttled by kind) or answering the user's action (`None`,
/// always shown).
type Queued = (Option<ToastKind>, String);

/// Dev overrides for screenshots (`MeowBoxAgent.exe --dev --theme zen --dark`).
#[derive(Debug, Clone, Default)]
pub struct DevLook {
    pub theme: Option<String>,
    pub dark: Option<bool>,
}

pub struct TrayApp {
    pub core: CoreHandle,
    tray: Tray,
    /// Toasts queued by other threads (shown on the next wake).
    toasts: Arc<Mutex<VecDeque<Queued>>>,
    /// Link changes that make a toast, and the 10-minute limit per kind (B6).
    watch: notice::Watch,
    throttle: notice::Throttle,
    last_line: String,
    /// Size and state of the icon now in the tray: it is drawn again only when one of them changes.
    icon_key: Option<IconKey>,
    /// A page the user asked for; opened by the outer loop, or switched to if the window is already open.
    open_request: Option<Page>,
    /// Something to do in the window once it shows (a mirror button or a menu item asked for it).
    then: Option<mirror::Act>,
    /// The window is open: a left click on the icon brings it to the front instead.
    dialog_open: bool,
    focus_request: bool,
    quit: bool,
    dev_look: DevLook,
    /// Dev check: open and close the window this many times by itself.
    selftest_reopen: u32,
    pub dialogs_opened: u32,
}

impl TrayApp {
    pub fn new(core: CoreHandle) -> anyhow::Result<Self> {
        strings::set_lang(&core.cfg().get().lang);
        let size = tray::tray_icon_size();
        let st = icon::State::Connecting;
        let tray = Tray::new(tray::make_icon(&icon::tray_rgba(st, size), size), s().app)?;
        let waker = tray.waker();
        core.on_state_change(move || waker.wake());
        let toasts: Arc<Mutex<VecDeque<Queued>>> = Arc::default();
        let (q, w) = (toasts.clone(), tray.waker());
        core.set_toast(move |kind, text| {
            q.lock().unwrap_or_else(|e| e.into_inner()).push_back((Some(kind), text));
            w.wake();
        });
        let mut app = Self {
            core,
            tray,
            toasts,
            watch: notice::Watch::default(),
            throttle: notice::Throttle::default(),
            last_line: String::new(),
            icon_key: Some((size, st)),
            open_request: None,
            then: None,
            dialog_open: false,
            focus_request: false,
            quit: false,
            dev_look: DevLook::default(),
            selftest_reopen: 0,
            dialogs_opened: 0,
        };
        app.refresh();
        if app.cfg().speaker.is_empty() {
            app.open_request = Some(Page::Connect); // first run: the window opens by itself
        }
        Ok(app)
    }

    pub fn cfg(&self) -> Config {
        self.core.cfg().get()
    }

    pub fn state(&self) -> AgentState {
        self.core.state().get()
    }

    pub fn set_dev_look(&mut self, dev: DevLook) {
        self.dev_look = dev;
    }

    /// A toast from any thread, answering the user's action (never throttled).
    fn toaster(&self) -> impl Fn(String) + Send + Sync + 'static {
        let (q, w) = (self.toasts.clone(), self.tray.waker());
        move |text| {
            q.lock().unwrap_or_else(|e| e.into_inner()).push_back((None, text));
            w.wake();
        }
    }

    /// The status line as the tray shows it now.
    pub fn status(&self) -> String {
        status_line(&self.state(), &self.cfg(), self.watch.ever_connected).0
    }

    /// Left click / "Open window": the window, or bring it to the front if it is open.
    fn open_window(&mut self) {
        if self.dialog_open {
            self.focus_request = true;
        } else {
            self.open_request = Some(Page::Connect);
        }
    }

    /// A toast for a link change or from the core, at most one per kind every 10 minutes; each new agent version
    /// is announced once, also across restarts.
    fn toast_kind(&mut self, kind: notice::Kind, text: String) {
        if let notice::Kind::Core(ToastKind::NewVersion(v)) = &kind {
            if self.cfg().toasted_version == *v {
                return;
            }
            let v = v.clone();
            self.core.cfg().update(|c| c.toasted_version = v);
        }
        if self.throttle.allow(&kind, Instant::now()) {
            tracing::info!("tray toast {kind:?}: {text}");
            self.tray.notify(&text, s().toast_title);
        } else {
            tracing::debug!("tray toast {kind:?} held back (one per 10 minutes)");
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

    /// Icon and tooltip follow the state; link changes that matter make a toast.
    fn refresh(&mut self) {
        let st = self.state();
        let cfg = self.cfg();
        if let Some(kind) = self.watch.update(&st, Instant::now()) {
            let t = s();
            let name = if st.name.is_empty() { "MeowBox" } else { &st.name };
            let text = match &kind {
                notice::Kind::Lost => f(t.t_lost, &[&name]),
                notice::Kind::Token => t.t_token.into(),
                notice::Kind::TakenOver => f(t.t_taken_over, &[&name, &st.busy_with]),
                notice::Kind::Core(_) => String::new(),
            };
            self.toast_kind(kind, text);
        }
        let (line, state) = status_line(&st, &cfg, self.watch.ever_connected);
        let size = tray::tray_icon_size();
        if line == self.last_line && self.icon_key == Some((size, state)) {
            return;
        }
        let tip = if st.ip().is_empty() { line.clone() } else { format!("{line} · {}", st.ip()) };
        // the line changes every second while a retry counts down; the icon does not
        if let Some(key) = new_icon_key(self.icon_key, size, state) {
            self.tray.set(tray::make_icon(&icon::tray_rgba(state, size), size), &tip);
            self.icon_key = Some(key);
        } else {
            self.tray.set_tip(&tip);
        }
        self.last_line = line;
    }

    /// The short menu of B3; everything else is in the window.
    fn menu(&self) -> Vec<MenuItem> {
        let t = s();
        let st = self.state();
        let cfg = self.cfg();
        let mut m = vec![MenuItem::label(self.status())];
        if st.error == LinkError::Busy {
            m.push(MenuItem::item(id::TAKE_OVER, t.m_take_over));
        }
        m.extend([
            MenuItem::Separator,
            MenuItem::default(id::OPEN_WINDOW, t.m_open_window),
            MenuItem::item(id::OPEN_WEB, t.m_open_web),
            MenuItem::Separator,
            MenuItem::check(id::PAUSE, t.m_pause, cfg.paused),
            MenuItem::check(id::CODING, t.m_coding, crate::coding::hooks::hooks_installed()),
            MenuItem::Separator,
        ]);
        if !st.new_version.is_empty() {
            m.extend([MenuItem::item(id::NEW_VERSION, f(t.m_new_version, &[&st.new_version])), MenuItem::Separator]);
        }
        m.push(MenuItem::item(id::QUIT, t.m_quit));
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
            id::OPEN_WINDOW => self.open_window(),
            id::OPEN_WEB => self.open_web(),
            id::TAKE_OVER => self.core.reconnect_now(true),
            id::PAUSE => self.core.cfg().update(|c| c.paused = !c.paused),
            id::CODING => self.toggle_coding(),
            // the Settings page has the version and the install button
            id::NEW_VERSION => {
                self.open_request = Some(Page::Settings);
                self.then = Some(mirror::Act::SeeUpdate);
            }
            id::QUIT => {
                tracing::info!("quit");
                self.quit = true;
                self.core.stop();
            }
            other => tracing::debug!("menu item {other}: unknown"),
        }
    }

    /// Add / remove MeowBox's hooks and status line in ~/.claude/settings.json (new Claude Code sessions pick them up).
    fn toggle_coding(&self) {
        let t = s();
        let r = if crate::coding::hooks::hooks_installed() {
            crate::coding::hooks::uninstall().map(|_| t.t_coding_off)
        } else {
            crate::coding::hooks::install(self.cfg().coding_port).map(|_| t.t_coding_on)
        };
        match r {
            Ok(msg) => self.tray.notify(msg, t.toast_title),
            Err(e) => {
                tracing::warn!("Claude Code settings: {e}");
                self.tray.notify(&f(t.t_coding_failed, &[&e]), t.toast_title);
            }
        }
    }

    /// "Gửi thông báo thử": a test notification through the speaker's API; the toast says how it went, with the
    /// button's words.
    fn test_notify(&self) {
        let t = s();
        let (host, token) = (self.state().host, self.cfg().token);
        let toast = self.toaster();
        if host.is_empty() || token.is_empty() {
            toast(t.t_no_speaker.into());
            return;
        }
        self.core.spawn(async move {
            let body = serde_json::json!({"title": t.test_title, "text": t.test_text, "sound": "chime"});
            match crate::speaker_api::notify(&host, &token, &body).await {
                Ok((code, _)) if code >= 400 => toast(f(t.t_speaker_error, &[&code])),
                Ok(_) => toast(t.t_test_sent.into()),
                Err(e) => toast(f(t.t_send_failed, &[&e])),
            }
        });
    }

    /// Handle everything queued by the tray window. Safe to call from the window's frame: it never opens a window
    /// itself, it only records `open_request`.
    fn process(&mut self) {
        while let Some(ev) = tray::pop() {
            tracing::debug!("tray event {ev:?}");
            match ev {
                TrayEvent::Click => self.open_window(),
                TrayEvent::Menu(x, y) => {
                    let items = self.menu();
                    if let Some(cmd) = self.tray.popup(&items, x, y) {
                        self.command(cmd);
                    }
                }
                TrayEvent::Wake => {
                    self.refresh();
                    let q: Vec<Queued> = self.toasts.lock().unwrap_or_else(|e| e.into_inner()).drain(..).collect();
                    for (kind, text) in q {
                        match kind {
                            Some(k) => self.toast_kind(notice::Kind::Core(k), text),
                            None => self.tray.notify(&text, s().toast_title),
                        }
                    }
                }
                TrayEvent::TaskbarCreated => self.tray.add(),
            }
        }
    }

    /// The main loop: messages, queued tray events, and the window when asked. Returns on Quit.
    pub fn run(&mut self) {
        loop {
            if let Some(page) = self.open_request.take() {
                self.run_dialog(page);
                self.process(); // events that arrived while the window was closing
            }
            if self.quit || !tray::pump_one() {
                tracing::debug!("tray loop ends (quit {})", self.quit);
                break;
            }
            self.process();
            if self.quit {
                break;
            }
        }
    }

    fn look(&self) -> theme::Look {
        let st = self.state();
        let id = self.dev_look.theme.clone().unwrap_or(st.theme);
        theme::Look::new(&id, self.dev_look.dark.unwrap_or_else(theme::windows_dark))
    }

    fn run_dialog(&mut self, page: Page) {
        tracing::debug!("window open ({page:?})");
        self.dialogs_opened += 1;
        self.dialog_open = true;
        let icon = egui::IconData { rgba: icon::cat_rgba(32, None), width: 32, height: 32 };
        let saved = self.cfg().window.filter(|w| on_screen(w[0], w[1]));
        let mut vp = egui::ViewportBuilder::default()
            .with_title(s().app)
            .with_inner_size(saved.map_or([560.0, 660.0], |w| [w[2] as f32, w[3] as f32]))
            .with_min_inner_size([480.0, 480.0])
            .with_resizable(true)
            .with_maximize_button(false)
            .with_icon(icon);
        if let Some(w) = saved {
            vp = vp.with_position([w[0] as f32, w[1] as f32]);
        }
        let options = eframe::NativeOptions {
            viewport: vp,
            centered: saved.is_none(),
            run_and_return: true,
            ..Default::default()
        };
        let auto_close = self.selftest_reopen > 0;
        let look = self.look();
        self.core.refresh_theme();
        let app: &mut TrayApp = &mut *self;
        let r = eframe::run_native(
            s().app,
            options,
            Box::new(move |cc| {
                cc.egui_ctx.set_fonts(theme::fonts(look.def));
                style(&cc.egui_ctx);
                theme::apply(&cc.egui_ctx, &look);
                tray::set_dialog_ctx(Some(cc.egui_ctx.clone()));
                let (connect, settings) = (connect::Connect::new(&app.core), settings::Settings::new(&app.core));
                let now = Instant::now();
                Ok(Box::new(Dialog {
                    app,
                    page,
                    connect,
                    settings,
                    look,
                    look_at: now,
                    theme_at: now,
                    rect: None,
                    frames: 0,
                    auto_close,
                }))
            }),
        );
        tray::set_dialog_ctx(None);
        self.dialog_open = false;
        self.focus_request = false;
        self.then = None;
        tracing::debug!("window closed, back in the tray loop");
        crate::win::trim_memory();
        if let Err(e) = r {
            tracing::error!("window failed: {e}");
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

/// A saved window position (points) is used only if a monitor is still there (a laptop off its dock).
fn on_screen(x: i32, y: i32) -> bool {
    use windows::Win32::Foundation::POINT;
    use windows::Win32::Graphics::Gdi::{
        GetDC, GetDeviceCaps, LOGPIXELSX, MONITOR_DEFAULTTONULL, MonitorFromPoint, ReleaseDC,
    };
    // SAFETY: plain GDI queries on the screen DC.
    unsafe {
        let dc = GetDC(None);
        let dpi = GetDeviceCaps(Some(dc), LOGPIXELSX).max(96);
        ReleaseDC(None, dc);
        let px = |v: i32| v.saturating_mul(dpi) / 96;
        // the title bar's left part must be on a monitor
        !MonitorFromPoint(POINT { x: px(x) + 40, y: px(y) + 10 }, MONITOR_DEFAULTTONULL).is_invalid()
    }
}

/// Windows-like sizes: 14 px Segoe UI, roomier buttons and fields.
fn style(ctx: &egui::Context) {
    ctx.all_styles_mut(|st| {
        use egui::{FontFamily::Proportional, FontId, TextStyle};
        st.text_styles = [
            (TextStyle::Heading, FontId::new(20.0, theme::display_family())),
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

/// Content column width at most (B2).
const MAX_W: f32 = 520.0;

/// The one agent window. Borrows the tray app for as long as it is open (eframe's run_and_return).
struct Dialog<'a> {
    app: &'a mut TrayApp,
    page: Page,
    connect: connect::Connect,
    settings: settings::Settings,
    look: theme::Look,
    /// Last time the theme / Windows dark mode were looked at, and the speaker asked for its theme.
    look_at: Instant,
    theme_at: Instant,
    /// Window position and size (points) as last seen, saved when it closes.
    rect: Option<[i32; 4]>,
    frames: u32,
    auto_close: bool,
}

impl Dialog<'_> {
    fn close(&mut self, ctx: &egui::Context) {
        self.save_rect();
        self.connect.close(&self.app.core);
        ctx.send_viewport_cmd(egui::ViewportCommand::Close);
    }

    fn save_rect(&self) {
        if let Some(r) = self.rect {
            self.app.core.cfg().update(|c| c.window = Some(r));
        }
    }

    /// Follow the speaker's theme and Windows' dark mode while the window is open.
    fn follow_look(&mut self, ctx: &egui::Context) {
        let now = Instant::now();
        if now.duration_since(self.theme_at) >= Duration::from_secs(30) {
            self.theme_at = now;
            self.app.core.refresh_theme();
        }
        if now.duration_since(self.look_at) < Duration::from_secs(2) {
            return;
        }
        self.look_at = now;
        let want = self.app.look();
        if want.def.id != self.look.def.id {
            ctx.set_fonts(theme::fonts(want.def));
        }
        if want.def.id != self.look.def.id || want.dark != self.look.dark {
            theme::apply(ctx, &want);
            self.look = want;
        }
    }

    fn mirror_act(&mut self, ui: &egui::Ui, a: mirror::Act) {
        let core = &self.app.core;
        match a {
            mirror::Act::EnterIp => {
                self.page = Page::Connect;
                self.connect.focus(connect::Row::Ip);
            }
            mirror::Act::EnterToken => {
                self.page = Page::Connect;
                self.connect.focus(connect::Row::Token);
            }
            mirror::Act::Retry => core.reconnect_now(false),
            mirror::Act::TakeOver => core.reconnect_now(true),
            mirror::Act::ChangePort => {
                self.page = Page::Settings;
                self.settings.focus_port = true;
            }
            mirror::Act::SeeUpdate => {
                self.page = Page::Settings;
                self.settings.check_agent(ui, core);
            }
            mirror::Act::OpenWeb => self.app.open_web(),
            mirror::Act::Resume => core.cfg().update(|c| c.paused = false),
        }
    }
}

impl eframe::App for Dialog<'_> {
    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        let ctx = ui.ctx().clone();
        // tray events keep working while the window is open
        self.app.process();
        if let Some(p) = self.app.open_request.take() {
            self.page = p;
            self.app.focus_request = true;
        }
        if std::mem::take(&mut self.app.focus_request) {
            ctx.send_viewport_cmd(egui::ViewportCommand::Minimized(false));
            ctx.send_viewport_cmd(egui::ViewportCommand::Focus);
        }
        if let Some(a) = self.app.then.take() {
            self.mirror_act(ui, a);
        }
        if self.app.quit {
            self.close(&ctx);
        }
        self.frames += 1;
        if self.auto_close && self.frames > 3 {
            self.close(&ctx);
        }
        ctx.input(|i| {
            let vp = i.viewport();
            if let (Some(o), Some(inner)) = (vp.outer_rect, vp.inner_rect) {
                let r = [o.min.x, o.min.y, inner.width(), inner.height()].map(|v| v.round() as i32);
                self.rect = Some(r);
            }
        });
        if ctx.input(|i| i.viewport().close_requested()) {
            self.save_rect();
            self.connect.close(&self.app.core);
        }
        self.follow_look(&ctx);
        // the mirror shows what the core sends every second
        ctx.request_repaint_after(Duration::from_secs(1));

        let look = self.look;
        let t = s();
        let panel = egui::Frame::NONE.fill(look.c(|p| p.bg)).inner_margin(egui::Margin::same(16));
        egui::CentralPanel::default().frame(panel).show(ui, |ui| {
            let r = ui.available_rect_before_wrap();
            let w = r.width().min(MAX_W);
            let column =
                egui::Rect::from_min_size(egui::pos2(r.center().x - w / 2.0, r.top()), egui::vec2(w, r.height()));
            ui.scope_builder(egui::UiBuilder::new().max_rect(column), |ui| {
                let (st, cfg, m) = (self.app.state(), self.app.cfg(), self.app.core.mirror());
                if let Some(a) = mirror::show(ui, &look, &st, &cfg, &m, self.app.watch.ever_connected) {
                    self.mirror_act(ui, a);
                }
                ui.add_space(10.0);
                ui.horizontal(|ui| {
                    for (p, label) in [(Page::Connect, t.tab_connect), (Page::Settings, t.tab_settings)] {
                        if widgets::tab(ui, &look, self.page == p, label).clicked() {
                            self.page = p;
                        }
                    }
                });
                ui.add_space(10.0);
                egui::ScrollArea::vertical().auto_shrink([false, false]).show(ui, |ui| match self.page {
                    Page::Connect => {
                        if let Some(connect::Act::TestNotify) = self.connect.ui(ui, &look, &self.app.core) {
                            self.app.test_notify();
                        }
                    }
                    Page::Settings => {
                        if let Some(settings::Act::Quit) = self.settings.ui(ui, &look, &self.app.core) {
                            self.app.quit = true;
                            self.app.core.stop();
                        }
                    }
                });
            });
        });
        if self.app.quit {
            self.close(&ctx);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn status_lines_say_what_to_do() {
        let cfg = Config::default();
        let mut st = AgentState::default();
        let line = |st: &AgentState, cfg: &Config, ever| status_line(st, cfg, ever).0;
        assert_eq!(line(&st, &cfg, false), "Đang tìm loa…");
        st.error = LinkError::NotFound;
        assert_eq!(line(&st, &cfg, false), "Chưa có IP loa: mở cửa sổ để nhập");
        let mut cfg2 = Config::default();
        cfg2.last_host = "192.168.1.50".into();
        st.retry_in = 8;
        assert_eq!(line(&st, &cfg2, false), "Không thấy loa, thử lại sau 8 s");
        assert_eq!(line(&st, &cfg2, true), "Mất kết nối với loa, thử lại sau 8 s");
        st.searching = "start".into();
        assert_eq!(line(&st, &cfg2, true), "Đang tìm loa…");
        st = AgentState { error: LinkError::Busy, busy_with: "DESKTOP-ABC".into(), ..Default::default() };
        cfg2.speaker = "MeowBox-1A2B".into();
        assert_eq!(status_line(&st, &cfg2, true), ("MeowBox-1A2B đang nối với DESKTOP-ABC".into(), icon::State::Error));
        st.error = LinkError::Token;
        assert_eq!(line(&st, &cfg2, true), "Loa không nhận token");
        st = AgentState { connected: true, name: "MeowBox-1A2B".into(), host: "192.168.1.50".into(), ..st };
        assert_eq!(status_line(&st, &cfg2, true), ("Đã nối MeowBox-1A2B".into(), icon::State::Connected));
    }

    /// The retry countdown changes the line every second; the icon is drawn only when its state or size changes.
    #[test]
    fn countdown_does_not_redraw_the_icon() {
        let mut cfg = Config::default();
        cfg.last_host = "192.168.1.50".into();
        let st = |n| AgentState { error: LinkError::NotFound, retry_in: n, ..Default::default() };
        let (l8, c8) = status_line(&st(8), &cfg, true);
        let (l7, c7) = status_line(&st(7), &cfg, true);
        assert_ne!(l8, l7);
        let key = new_icon_key(Some((16, icon::State::Connecting)), 16, c8).expect("connecting to error draws once");
        assert_eq!(new_icon_key(Some(key), 16, c7), None, "same state: tooltip only");
        assert!(new_icon_key(Some(key), 24, c7).is_some(), "DPI change draws again");
        assert!(new_icon_key(None, 16, c7).is_some());
    }
}
