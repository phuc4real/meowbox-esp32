//! "Kết nối" (B2): one page, three rows checked in order, each by itself: 1 the speaker's IP (`/api/ping`), 2 the
//! notify token (`/api/notify/history`), 3 the YouTube relay (the speaker calls back, `/api/pc/relay_test`). The
//! numbers stay because the rows really are a sequence. A row turns ✓ when it passes; the first one that fails
//! opens in place with the reason and the fix, and the cursor goes to it. A passing IP or token is saved at once and
//! the core reconnects with it.

use super::strings::{f, s};
use super::task::{Task, blocking, poll};
use super::theme::Look;
use super::widgets::{self, Mark};
use crate::discovery::{PingError, Speaker};
use crate::link::CoreHandle;
use eframe::egui::{self, RichText, Ui};
use std::time::Duration;

const WIFI_HELP: &str = "https://github.com/phuc4real/meowbox-esp32#readme";

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Row {
    Ip,
    Token,
    Relay,
}

#[derive(Debug, Clone)]
enum Check<T> {
    Idle,
    Running,
    Ok(T),
    Fail(String),
}

/// (host checked, result, why it failed)
type IpCheck = (String, Result<Speaker, PingError>, String);
/// (token checked, HTTP status or error)
type TokenCheck = (String, Result<u16, String>);
/// Ok(ms) or Err((reason, network category)); Err(NO_LINK) when the speaker never took our hello.
type RelayCheck = Result<String, (String, String)>;
const NO_LINK: &str = "no_link";

pub struct Connect {
    ip: String,
    ip_check: Check<Speaker>,
    ip_task: Option<Task<IpCheck>>,
    wifi: Option<String>,
    wifi_task: Option<Task<String>>,

    token: String,
    tok_check: Check<()>,
    tok_task: Option<Task<TokenCheck>>,

    relay_check: Check<String>,
    relay_task: Option<Task<RelayCheck>>,
    relay_public: bool,
    relay_note: Option<String>,
    new_port: String,
    port_note: Option<String>,

    started: bool,
    focus: Option<Row>,
}

pub enum Act {
    TestNotify,
}

impl Connect {
    pub fn new(core: &CoreHandle) -> Self {
        let cfg = core.cfg().get();
        Self {
            ip: cfg.known_host().to_owned(),
            ip_check: Check::Idle,
            ip_task: None,
            wifi: None,
            wifi_task: None,
            token: cfg.token.clone(),
            tok_check: Check::Idle,
            tok_task: None,
            relay_check: Check::Idle,
            relay_task: None,
            relay_public: false,
            relay_note: None,
            new_port: cfg.relay_port.saturating_add(1).to_string(),
            port_note: None,
            started: false,
            focus: None,
        }
    }

    /// Leaving the window: the relay serves only the connected speaker again.
    pub fn close(&self, core: &CoreHandle) {
        core.clear_allowed();
    }

    /// Put the cursor in a row (the mirror's "Enter IP" / "Enter token").
    pub fn focus(&mut self, row: Row) {
        self.focus = Some(row);
    }

    fn speaker(&self) -> Option<&Speaker> {
        match &self.ip_check {
            Check::Ok(sp) => Some(sp),
            _ => None,
        }
    }

    // -- 1: IP
    fn check_ip(&mut self, ui: &Ui, core: &CoreHandle) {
        let host = self.ip.trim().to_owned();
        if host.is_empty() {
            self.focus = Some(Row::Ip);
            return;
        }
        self.ip_check = Check::Running;
        self.tok_check = Check::Idle;
        self.relay_check = Check::Idle;
        self.wifi = None;
        self.ip_task = Some(Task::spawn(core, ui.ctx(), async move {
            let r = crate::discovery::ping_detail(&host).await;
            let mut why = String::new();
            if let Err(e) = &r {
                why = if *e == PingError::NoAnswer { crate::discovery::explain_failure(&host) } else { e.key() }.into();
            }
            (host, r, why)
        }));
    }

    fn ip_done(&mut self, ui: &Ui, core: &CoreHandle, host: String, r: Result<Speaker, PingError>, why: String) {
        if self.ip.trim() != host {
            return; // edited while checking
        }
        match r {
            Ok(sp) => {
                let cfg = core.cfg().get();
                if cfg.speaker != sp.name || cfg.host != sp.host || cfg.last_host != sp.host {
                    let sp2 = sp.clone();
                    core.cfg().update(|c| {
                        c.speaker = sp2.name.clone();
                        c.host = sp2.host.clone();
                        c.last_host = sp2.host.clone();
                    });
                    core.reconnect_now(false);
                }
                self.ip_check = Check::Ok(sp);
                if self.token.trim().is_empty() {
                    self.focus = Some(Row::Token);
                } else {
                    self.check_token(ui, core);
                }
            }
            Err(_) => {
                let t = s();
                let reason = match why.as_str() {
                    "not_meowbox" => t.c_err_not_meowbox,
                    "bad_address" => t.c_err_bad_address,
                    "other_network" => t.c_err_other_network,
                    _ => t.c_err_no_answer,
                };
                self.ip_check = Check::Fail(reason.into());
                self.focus = Some(Row::Ip);
                // the Wi-Fi name needs netsh / the network list: off the UI thread
                self.wifi_task = Some(Task::spawn(core, ui.ctx(), async {
                    blocking(crate::win::wifi_name).await.filter(|w| !w.is_empty()).unwrap_or_default()
                }));
            }
        }
    }

    // -- 2: token
    fn check_token(&mut self, ui: &Ui, core: &CoreHandle) {
        let Some(sp) = self.speaker() else {
            self.focus = Some(Row::Ip);
            return;
        };
        let (host, tok) = (sp.host.clone(), self.token.trim().to_owned());
        if tok.is_empty() {
            self.focus = Some(Row::Token);
            return;
        }
        self.tok_check = Check::Running;
        self.relay_check = Check::Idle;
        self.tok_task = Some(Task::spawn(core, ui.ctx(), async move {
            let r = crate::speaker_api::request(&host, "/api/notify/history", &tok, None, Duration::from_secs(5))
                .await
                .map(|(code, _)| code)
                .map_err(|e| e.to_string());
            (tok, r)
        }));
    }

    fn token_done(&mut self, ui: &Ui, core: &CoreHandle, (tok, r): TokenCheck) {
        if self.token.trim() != tok {
            return; // edited while checking: that one is checked again on Enter / Kiểm tra
        }
        let t = s();
        match r {
            Ok(200) => {
                if core.cfg().get().token != tok {
                    core.cfg().update(|c| c.token = tok);
                    core.reconnect_now(false); // the hello carries the token
                }
                self.tok_check = Check::Ok(());
                self.check_relay(ui, core);
            }
            Ok(401) => self.tok_check = Check::Fail(t.c_token_bad.into()),
            Ok(code) => self.tok_check = Check::Fail(f(t.c_token_err, &[&format!("HTTP {code}")])),
            Err(e) => self.tok_check = Check::Fail(f(t.c_token_err, &[&e])),
        }
        if matches!(self.tok_check, Check::Fail(_)) {
            self.focus = Some(Row::Token);
        }
    }

    // -- 3: relay
    fn check_relay(&mut self, ui: &Ui, core: &CoreHandle) {
        let Some(sp) = self.speaker().cloned() else { return };
        let tok = self.token.trim().to_owned();
        if !matches!(self.tok_check, Check::Ok(_)) {
            return;
        }
        self.relay_public = false;
        self.relay_note = None;
        if core.state().get().relay_port == 0 {
            let port = core.cfg().get().relay_port;
            self.relay_check = Check::Fail(f(s().c_port_busy, &[&port]));
            return;
        }
        core.allow_ip(sp.ip());
        self.relay_check = Check::Running;
        let state = core.state().clone();
        self.relay_task = Some(Task::spawn(core, ui.ctx(), async move {
            // the speaker learns the relay address from our hello
            for _ in 0..20 {
                let st = state.get();
                if st.connected && st.name == sp.name {
                    break;
                }
                tokio::time::sleep(Duration::from_millis(500)).await;
            }
            let st = state.get();
            if !(st.connected && st.name == sp.name) {
                // the speaker would test the relay of the PC it is connected to (maybe another one)
                return Err((NO_LINK.into(), String::new()));
            }
            let body = Some((b"{}".to_vec(), "application/json"));
            let fail = |e: String| (e, String::new());
            let (code, text) =
                crate::speaker_api::request(&sp.host, "/api/pc/relay_test", &tok, body, Duration::from_secs(8))
                    .await
                    .map_err(|e| fail(e.to_string()))?;
            if code != 200 {
                return Err(fail(format!("HTTP {code}")));
            }
            let j: serde_json::Value = serde_json::from_str(&text).map_err(|e| fail(e.to_string()))?;
            if j.get("ok").and_then(|v| v.as_bool()).unwrap_or(false) {
                return Ok(j.get("ms").map(|v| v.to_string()).unwrap_or_else(|| "?".into()));
            }
            let err = j.get("err").and_then(|v| v.as_str()).unwrap_or("?").to_owned();
            let ip = sp.ip().to_owned();
            let cat = blocking(move || crate::win::network_category(&crate::win::local_ip_towards(&ip)))
                .await
                .unwrap_or_default();
            Err((err, cat))
        }));
    }

    fn relay_done(&mut self, core: &CoreHandle, r: RelayCheck) {
        let t = s();
        match r {
            Ok(ms) => {
                core.state().update(|s| s.relay_ok = Some(true));
                self.relay_check = Check::Ok(f(t.c_relay_ok, &[&ms]));
            }
            Err((err, _)) if err == NO_LINK => self.relay_check = Check::Fail(t.c_relay_no_link.into()),
            Err((err, cat)) => {
                core.state().update(|s| s.relay_ok = Some(false));
                self.relay_public = cat == "Public";
                self.relay_check = Check::Fail(f(t.c_relay_fail, &[&err]));
            }
        }
    }

    pub fn ui(&mut self, ui: &mut Ui, look: &Look, core: &CoreHandle) -> Option<Act> {
        let t = s();
        let mut act = None;
        if !self.started {
            self.started = true;
            if self.ip.trim().is_empty() {
                self.focus = Some(Row::Ip);
            } else {
                self.check_ip(ui, core);
            }
        }
        let mut done_ip = None;
        poll(&mut self.ip_task, |r| done_ip = Some(r));
        if let Some((host, r, why)) = done_ip {
            self.ip_done(ui, core, host, r, why);
        }
        poll(&mut self.wifi_task, |w| self.wifi = Some(w));
        let mut done_tok = None;
        poll(&mut self.tok_task, |r| done_tok = Some(r));
        if let Some(r) = done_tok {
            self.token_done(ui, core, r);
        }
        let mut done_relay = None;
        poll(&mut self.relay_task, |r| done_relay = Some(r));
        if let Some(r) = done_relay {
            self.relay_done(core, r);
        }
        let focus = self.focus.take();

        // 1 IP
        let (ip_mark, ip_fail) =
            mark_of(&self.ip_check, |sp| f(t.c_ip_ok, &[&sp.name, &if sp.ver.is_empty() { "?" } else { &sp.ver }]));
        let mut run_ip = false;
        row(ui, look, "1", t.c_ip, |ui, label| {
            let field = egui::TextEdit::singleline(&mut self.ip).desired_width(150.0).hint_text("192.168.1.50");
            let r = ui.add(field).labelled_by(label);
            if focus == Some(Row::Ip) {
                r.request_focus();
            }
            if r.changed() {
                self.ip_check = Check::Idle;
                self.tok_check = Check::Idle;
                self.relay_check = Check::Idle;
            }
            run_ip = enter(ui, &r);
            run_ip |= check_button(ui, look, &self.ip_check);
            widgets::mark(ui, look, &ip_mark);
        });
        if run_ip {
            self.check_ip(ui, core);
        }
        if let Some(reason) = ip_fail {
            expanded(ui, |ui| {
                widgets::problem(ui, look, &reason);
                let wifi = match self.wifi.as_deref() {
                    None => "…",
                    Some("") => t.c_not_wifi,
                    Some(w) => w,
                };
                widgets::note(ui, look, &f(t.c_reasons, &[&wifi]));
                if widgets::button(ui, look, t.c_wifi_help).clicked() {
                    crate::win::open(WIFI_HELP);
                }
            });
        } else if matches!(self.ip_check, Check::Idle) && self.ip.trim().is_empty() {
            expanded(ui, |ui| widgets::note(ui, look, t.c_ip_hint));
        }

        // 2 token
        let have_ip = self.speaker().is_some();
        let (tok_mark, tok_fail) = if have_ip {
            mark_of(&self.tok_check, |_| t.c_token_ok.to_owned())
        } else {
            (Mark::Dim(t.c_need_ip.into()), None)
        };
        let mut run_tok = false;
        row(ui, look, "2", t.c_token, |ui, label| {
            let field = egui::TextEdit::singleline(&mut self.token).desired_width(150.0).password(true);
            let r = ui.add_enabled(have_ip, field).labelled_by(label);
            if focus == Some(Row::Token) {
                r.request_focus();
            }
            if r.changed() {
                self.tok_check = Check::Idle;
                self.relay_check = Check::Idle;
            }
            run_tok = enter(ui, &r);
            if have_ip {
                run_tok |= check_button(ui, look, &self.tok_check);
            }
            widgets::mark(ui, look, &tok_mark);
        });
        if run_tok {
            self.check_token(ui, core);
        }
        if let Some(reason) = tok_fail {
            expanded(ui, |ui| {
                widgets::problem(ui, look, &reason);
                if let Some(sp) = self.speaker()
                    && widgets::button(ui, look, t.c_open_notify).clicked()
                {
                    crate::win::open(&format!("http://{}/#/notify", sp.host));
                }
            });
        } else if have_ip && matches!(self.tok_check, Check::Idle) && self.token.trim().is_empty() {
            let host = self.speaker().map(|sp| sp.host.clone()).unwrap_or_default();
            expanded(ui, |ui| {
                widgets::note(ui, look, t.why_token);
                if widgets::button(ui, look, t.c_open_notify).clicked() {
                    crate::win::open(&format!("http://{host}/#/notify"));
                }
            });
        }

        // 3 relay
        let have_tok = matches!(self.tok_check, Check::Ok(_));
        let (relay_mark, relay_fail) = if !have_tok {
            (Mark::Dim(t.c_need_token.into()), None)
        } else if matches!(self.relay_check, Check::Running) {
            (Mark::Wait(t.c_relay_wait.into()), None)
        } else {
            mark_of(&self.relay_check, Clone::clone)
        };
        let mut run_relay = false;
        row(ui, look, "3", t.c_relay, |ui, _| {
            if have_tok {
                run_relay = check_button(ui, look, &self.relay_check);
            }
            widgets::mark(ui, look, &relay_mark);
        });
        if run_relay {
            self.check_relay(ui, core);
        }
        if let Some(reason) = relay_fail {
            expanded(ui, |ui| {
                widgets::problem(ui, look, &reason);
                if core.state().get().relay_port == 0 {
                    ui.horizontal(|ui| {
                        ui.add(egui::TextEdit::singleline(&mut self.new_port).desired_width(70.0));
                        if widgets::button(ui, look, t.c_port_save).clicked() {
                            match self.new_port.trim().parse::<u16>() {
                                Ok(p) if p > 1024 => {
                                    core.cfg().update(|c| c.relay_port = p);
                                    self.port_note = Some(f(t.c_port_saved, &[&p]));
                                }
                                _ => self.port_note = Some(t.s_port_range.into()),
                            }
                        }
                    });
                }
                if self.relay_public {
                    widgets::note(ui, look, t.c_relay_public);
                    if widgets::button(ui, look, t.c_allow).clicked() {
                        crate::win::allow_relay_on_this_network(core.cfg().get().relay_port, "Public");
                        self.relay_note = Some(t.c_allowed.into());
                        self.relay_public = false;
                    }
                }
                for n in [&self.relay_note, &self.port_note].into_iter().flatten() {
                    widgets::note(ui, look, n);
                }
            });
        }

        ui.add_space(14.0);
        ui.horizontal(|ui| {
            widgets::right(ui, |ui| {
                let connected = core.state().get().connected;
                if widgets::button_enabled(ui, look, t.c_test, connected).clicked() {
                    act = Some(Act::TestNotify);
                }
                if widgets::button(ui, look, t.c_reconnect).clicked() {
                    core.reconnect_now(false);
                }
            });
        });
        act
    }
}

fn mark_of<T>(c: &Check<T>, ok: impl FnOnce(&T) -> String) -> (Mark, Option<String>) {
    let t = s();
    match c {
        Check::Idle => (Mark::Dim(String::new()), None),
        Check::Running => (Mark::Wait(t.c_checking.into()), None),
        Check::Ok(v) => (Mark::Good(ok(v)), None),
        Check::Fail(e) => (Mark::Bad(String::new()), Some(e.clone())),
    }
}

/// "1  IP loa  [field] [Kiểm tra] ✓ …"
/// `body` gets the label's id, so a field can be "labelled by" it for screen readers.
fn row(ui: &mut Ui, look: &Look, n: &str, label: &str, body: impl FnOnce(&mut Ui, egui::Id)) {
    ui.horizontal(|ui| {
        ui.set_min_height(32.0);
        ui.label(RichText::new(n).color(look.dim()).size(15.0));
        ui.add_space(4.0);
        let label_id = ui.allocate_ui_with_layout(
            egui::vec2(widgets::LABEL_W - 24.0, 28.0),
            egui::Layout::left_to_right(egui::Align::Center),
            |ui| {
                ui.set_width(widgets::LABEL_W - 24.0);
                ui.label(RichText::new(label).color(look.text())).id
            },
        );
        body(ui, label_id.inner);
    });
}

/// The opened part under a row that failed: indented to the field.
fn expanded(ui: &mut Ui, body: impl FnOnce(&mut Ui)) {
    ui.horizontal(|ui| {
        ui.add_space(widgets::LABEL_W);
        ui.vertical(|ui| {
            ui.set_max_width(ui.available_width());
            body(ui);
        });
    });
    ui.add_space(6.0);
}

fn enter(ui: &Ui, r: &egui::Response) -> bool {
    r.lost_focus() && ui.input(|i| i.key_pressed(egui::Key::Enter))
}

/// "Kiểm tra" while the row has not passed (a row that passed shows only its ✓; Enter in its field checks again).
fn check_button<T>(ui: &mut Ui, look: &Look, c: &Check<T>) -> bool {
    match c {
        Check::Ok(_) => false,
        _ => widgets::button_enabled(ui, look, s().c_check, !matches!(c, Check::Running)).clicked(),
    }
}
