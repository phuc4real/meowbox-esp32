//! "Kết nối loa" (docs/firmware_v0.2.0.md, 2.2): 1 speaker IP, 2 notify token, 3 YouTube relay check. On Finish
//! it saves speaker/token/last_host and asks the core to reconnect now. Port of connect_dialog.py.

use super::strings::connect as t;
use super::task::{Task, blocking, poll};
use super::{Look, hint, msg};
use crate::discovery::{PingError, Speaker};
use crate::link::CoreHandle;
use eframe::egui::{self, RichText};
use std::time::Duration;

type Msg = Option<(String, egui::Color32)>;
/// (host checked, result, why it failed)
type IpCheck = (String, Result<Speaker, PingError>, String);
/// (ok, ms or error, network category)
type RelayCheck = Result<(bool, String, String), String>;

pub struct Connect {
    pub step: usize,
    ip: String,
    sel: Option<Speaker>,
    ip_msg: Msg,
    reasons: Option<String>,
    ip_task: Option<Task<IpCheck>>,
    wifi_task: Option<Task<String>>,
    tried_on_open: bool,

    token: String,
    token_ok: bool,
    tok_msg: Msg,
    tok_task: Option<Task<Result<u16, String>>>,
    tok_checked_on_open: bool,

    new_port: String,
    port_msg: Msg,
    relay_msg: Msg,
    relay_public: bool,
    relay_task: Option<Task<RelayCheck>>,
    relay_tested_on_open: bool,

    /// Set when Finish was pressed: the window closes.
    pub finished: bool,
}

impl Connect {
    pub fn new(core: &CoreHandle) -> Self {
        let cfg = core.cfg().get();
        Self {
            step: 0,
            ip: cfg.known_host().to_owned(),
            sel: None,
            ip_msg: None,
            reasons: None,
            ip_task: None,
            wifi_task: None,
            tried_on_open: false,
            token: cfg.token.clone(),
            token_ok: false,
            tok_msg: None,
            tok_task: None,
            tok_checked_on_open: false,
            new_port: (cfg.relay_port.saturating_add(1)).to_string(),
            port_msg: None,
            relay_msg: None,
            relay_public: false,
            relay_task: None,
            relay_tested_on_open: false,
            finished: false,
        }
    }

    /// Leaving the window: the relay serves only the connected speaker again.
    pub fn close(&self, core: &CoreHandle) {
        core.clear_allowed();
    }

    fn save(&self, core: &CoreHandle, reconnect: bool) {
        let (sel, token) = (self.sel.clone(), self.token.trim().to_owned());
        core.cfg().update(|c| {
            if let Some(sp) = &sel {
                c.speaker = sp.name.clone();
                c.host = sp.host.clone();
                c.last_host = sp.host.clone();
            }
            c.token = token;
        });
        if reconnect {
            core.reconnect_now(false);
        }
    }

    fn next(&mut self, core: &CoreHandle) {
        match self.step {
            0 => {
                if self.sel.is_none() {
                    return;
                }
                self.save(core, true);
            }
            1 => {
                if !self.token_ok {
                    self.tok_msg = msg(t::TOKEN_NEED, Look::Bad);
                    return;
                }
                self.save(core, true); // the hello carries the token now
            }
            _ => {
                self.save(core, false);
                self.finished = true;
                return;
            }
        }
        self.step += 1;
    }

    pub fn ui(&mut self, ui: &mut egui::Ui, core: &CoreHandle) {
        let head = t::STEPS
            .iter()
            .enumerate()
            .map(|(k, s)| if k == self.step { format!("● {s}") } else { (*s).to_owned() })
            .collect::<Vec<_>>()
            .join("  ›  ");
        ui.label(RichText::new(head).size(15.0));
        ui.add_space(8.0);

        egui::Panel::bottom("connect_nav").show_separator_line(false).show(ui, |ui| {
            ui.add_space(6.0);
            ui.horizontal(|ui| {
                if ui.add_enabled(self.step > 0, egui::Button::new(t::BACK)).clicked() {
                    self.step -= 1;
                }
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    let label = if self.step == 2 { t::FINISH } else { t::NEXT };
                    let enabled = self.step != 0 || self.sel.is_some();
                    if ui.add_enabled(enabled, egui::Button::new(label)).clicked() {
                        self.next(core);
                    }
                });
            });
        });

        egui::CentralPanel::default().frame(egui::Frame::NONE).show(ui, |ui| match self.step {
            0 => self.page_ip(ui, core),
            1 => self.page_token(ui, core),
            _ => self.page_relay(ui, core),
        });
    }

    // -- step 1
    fn try_ip(&mut self, ui: &egui::Ui, core: &CoreHandle) {
        let host = self.ip.trim().to_owned();
        self.ip_msg = msg("…", Look::Hint);
        self.reasons = None;
        self.ip_task = Some(Task::spawn(core, ui.ctx(), async move {
            let r = crate::discovery::ping_detail(&host).await;
            let mut why = String::new();
            if let Err(e) = &r {
                why = if *e == PingError::NoAnswer { crate::discovery::explain_failure(&host) } else { e.key() }.into();
            }
            (host, r, why)
        }));
    }

    fn page_ip(&mut self, ui: &mut egui::Ui, core: &CoreHandle) {
        let mut failed = false;
        poll(&mut self.ip_task, |(host, r, why)| {
            if self.step != 0 || self.ip.trim() != host {
                return; // left the page or edited the IP while checking
            }
            match r {
                Ok(sp) => {
                    self.ip_msg = msg(&t::ok_ip(&sp.name, if sp.ver.is_empty() { "?" } else { &sp.ver }), Look::Good);
                    self.sel = Some(sp);
                    self.reasons = None;
                }
                Err(_) => {
                    self.ip_msg = msg(t::error(&why), Look::Bad);
                    failed = true;
                }
            }
        });
        if failed {
            // the Wi-Fi name needs netsh / the network list: off the UI thread
            self.reasons = Some(t::reasons("…"));
            self.wifi_task = Some(Task::spawn(core, ui.ctx(), async {
                blocking(crate::win::wifi_name).await.filter(|w| !w.is_empty()).unwrap_or_else(|| t::NOT_WIFI.into())
            }));
        }
        poll(&mut self.wifi_task, |w| {
            if self.reasons.is_some() {
                self.reasons = Some(t::reasons(&w));
            }
        });
        ui.label(t::IP);
        ui.horizontal(|ui| {
            let r = ui.add(egui::TextEdit::singleline(&mut self.ip).desired_width(200.0));
            if r.changed() && self.sel.as_ref().is_some_and(|s| s.host != self.ip.trim()) {
                // a different IP than the one checked: check again before Next
                self.sel = None;
                self.ip_msg = None;
            }
            let enter = r.lost_focus() && ui.input(|i| i.key_pressed(egui::Key::Enter));
            if ui.button(t::TRY).clicked() || enter {
                self.try_ip(ui, core);
            }
        });
        if !self.tried_on_open {
            self.tried_on_open = true;
            if !self.ip.trim().is_empty() {
                self.try_ip(ui, core);
            }
        }
        show_msg(ui, &self.ip_msg);
        ui.add_space(6.0);
        hint(ui, t::IP_TIP);
        if let Some(r) = &self.reasons {
            ui.add_space(8.0);
            ui.label(RichText::new(r).color(Look::Text.color()));
            if ui.button(t::WIFI_HELP_BTN).clicked() {
                crate::win::open(t::WIFI_HELP);
            }
        }
    }

    // -- step 2
    fn check_token(&mut self, ui: &egui::Ui, core: &CoreHandle) {
        let (host, tok) = (self.sel.as_ref().map(|s| s.host.clone()).unwrap_or_default(), self.token.trim().to_owned());
        self.tok_msg = msg("…", Look::Hint);
        self.tok_task = Some(Task::spawn(core, ui.ctx(), async move {
            crate::speaker_api::request(&host, "/api/notify/history", &tok, None, Duration::from_secs(5))
                .await
                .map(|(code, _)| code)
                .map_err(|e| e.to_string())
        }));
    }

    fn page_token(&mut self, ui: &mut egui::Ui, core: &CoreHandle) {
        poll(&mut self.tok_task, |r| {
            self.token_ok = r == Ok(200);
            self.tok_msg = match r {
                Ok(200) => msg(t::TOKEN_OK, Look::Good),
                Ok(401) => msg(t::TOKEN_BAD, Look::Bad),
                Ok(code) => msg(&t::token_err(&code.to_string()), Look::Bad),
                Err(e) => msg(&t::token_err(&e), Look::Bad),
            };
        });
        ui.label(t::TOKEN_INTRO);
        ui.add_space(8.0);
        let host = self.sel.as_ref().map(|s| s.host.clone()).unwrap_or_default();
        if ui.button(t::OPEN_NOTIFY).clicked() {
            crate::win::open(&format!("http://{host}/#/notify"));
        }
        ui.add_space(8.0);
        ui.horizontal(|ui| {
            ui.label(t::TOKEN);
            let r = ui.add(egui::TextEdit::singleline(&mut self.token).desired_width(300.0));
            if r.changed() {
                self.token_ok = false;
            }
            let enter = r.lost_focus() && ui.input(|i| i.key_pressed(egui::Key::Enter));
            if ui.button(t::CHECK).clicked() || enter {
                self.check_token(ui, core);
            }
        });
        if !self.tok_checked_on_open {
            self.tok_checked_on_open = true;
            if !self.token.trim().is_empty() {
                // saved earlier: check it right away
                self.check_token(ui, core);
            }
        }
        show_msg(ui, &self.tok_msg);
    }

    // -- step 3
    fn relay_test(&mut self, ui: &egui::Ui, core: &CoreHandle) {
        let tok = self.token.trim().to_owned();
        if tok.is_empty() {
            self.relay_msg = msg(t::RELAY_NEED_TOKEN, Look::Hint);
            return;
        }
        let Some(sp) = self.sel.clone() else { return };
        core.allow_ip(sp.ip());
        self.relay_msg = msg(t::RELAY_WAIT, Look::Hint);
        self.relay_public = false;
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
            let body = Some((b"{}".to_vec(), "application/json"));
            let (code, text) =
                crate::speaker_api::request(&sp.host, "/api/pc/relay_test", &tok, body, Duration::from_secs(8))
                    .await
                    .map_err(|e| e.to_string())?;
            if code != 200 {
                return Err(format!("HTTP Error {code}"));
            }
            let j: serde_json::Value = serde_json::from_str(&text).map_err(|e| e.to_string())?;
            let ok = j.get("ok").and_then(|v| v.as_bool()).unwrap_or(false);
            if ok {
                let ms = j.get("ms").map(|v| v.to_string()).unwrap_or_else(|| "?".into());
                return Ok((true, ms, String::new()));
            }
            let err = j.get("err").and_then(|v| v.as_str()).unwrap_or("?").to_owned();
            let ip = sp.ip().to_owned();
            let cat = blocking(move || crate::win::network_category(&crate::win::local_ip_towards(&ip)))
                .await
                .unwrap_or_default();
            Ok((false, err, cat))
        }));
    }

    fn page_relay(&mut self, ui: &mut egui::Ui, core: &CoreHandle) {
        let cfg = core.cfg().get();
        if core.state().get().relay_port == 0 {
            ui.label(t::RELAY_PORT_BUSY);
            ui.horizontal(|ui| {
                ui.add(egui::TextEdit::singleline(&mut self.new_port).desired_width(80.0));
                if ui.button("OK").clicked() {
                    match self.new_port.trim().parse::<u16>() {
                        Ok(p) if p > 1024 => {
                            core.cfg().update(|c| c.relay_port = p);
                            self.save(core, false);
                            self.port_msg = msg(&t::saved_port(p), Look::Hint);
                        }
                        _ => self.port_msg = msg(super::strings::settings::PORT_RANGE, Look::Bad),
                    }
                }
            });
            show_msg(ui, &self.port_msg);
            return;
        }
        poll(&mut self.relay_task, |r| match r {
            Err(e) => self.relay_msg = msg(&t::relay_fail(&e), Look::Bad),
            Ok((true, ms, _)) => {
                core.state().update(|s| s.relay_ok = Some(true));
                self.relay_msg = msg(&t::relay_ok(&ms), Look::Good);
            }
            Ok((false, err, cat)) => {
                core.state().update(|s| s.relay_ok = Some(false));
                let mut text = t::relay_fail(&err);
                if cat == "Public" {
                    text = format!("{text}\n{}", t::PUBLIC);
                    self.relay_public = true;
                }
                self.relay_msg = msg(&text, Look::Bad);
            }
        });
        ui.label(t::relay_intro(cfg.relay_port));
        ui.add_space(8.0);
        if ui.add_enabled(self.relay_task.is_none(), egui::Button::new(t::RELAY_RUN)).clicked() {
            self.relay_test(ui, core);
        }
        if !self.relay_tested_on_open {
            self.relay_tested_on_open = true;
            self.relay_test(ui, core);
        }
        ui.add_space(6.0);
        show_msg(ui, &self.relay_msg);
        if self.relay_public && ui.button(t::ALLOW).clicked() {
            crate::win::allow_relay_on_this_network(cfg.relay_port, "Public");
            self.relay_msg = msg(t::ALLOWED, Look::Hint);
            self.relay_public = false;
        }
    }
}

fn show_msg(ui: &mut egui::Ui, m: &Msg) {
    if let Some((text, color)) = m {
        ui.label(RichText::new(text).color(*color));
    }
}
