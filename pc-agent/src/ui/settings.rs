//! "Cài đặt": agent update, yt-dlp, ffmpeg, options. Port of settings_dialog.py.

use super::strings::settings as t;
use super::task::{Task, blocking, poll};
use super::{Look, msg};
use crate::link::CoreHandle;
use crate::tools::{ffmpeg, ytdlp};
use eframe::egui::{self, RichText};
use std::time::Duration;

type Msg = Option<(String, egui::Color32)>;

enum AgentBtn {
    Check,
    Install(String),
    Github,
}

enum YtBtn {
    Check,
    Update,
}

pub struct Settings {
    agent_text: String,
    agent_btn: AgentBtn,
    agent_task: Option<Task<Option<String>>>,
    install_task: Option<Task<Result<std::path::PathBuf, String>>>,

    yt_text: String,
    yt_btn: YtBtn,
    yt_check: Option<Task<Option<String>>>,
    yt_update: Option<Task<Result<String, String>>>,

    /// (version, status) of ffmpeg, read off the UI thread.
    ff: Option<(Option<String>, ffmpeg::Status)>,
    ff_task: Option<Task<(Option<String>, ffmpeg::Status)>>,
    ff_installing: bool,

    port: String,
    port_msg: Msg,
    started: bool,
}

impl Settings {
    pub fn new(core: &CoreHandle) -> Self {
        Self {
            agent_text: t::version(crate::VERSION),
            agent_btn: AgentBtn::Check,
            agent_task: None,
            install_task: None,
            yt_text: "…".into(),
            yt_btn: YtBtn::Check,
            yt_check: None,
            yt_update: None,
            ff: None,
            ff_task: None,
            ff_installing: false,
            port: core.cfg().get().relay_port.to_string(),
            port_msg: None,
            started: false,
        }
    }

    fn refresh_local(&mut self, ui: &egui::Ui, core: &CoreHandle) {
        let v = ytdlp::version();
        self.yt_text = match &v {
            Some(v) => t::version(v),
            None => t::YTDLP_MISSING.into(),
        };
        if v.is_none() {
            self.yt_btn = YtBtn::Update;
        }
        self.ff_task = Some(Task::spawn(core, ui.ctx(), async {
            blocking(|| (ffmpeg::version(), ffmpeg::status())).await.unwrap_or_default()
        }));
    }

    pub fn ui(&mut self, ui: &mut egui::Ui, core: &CoreHandle) -> Option<Action> {
        let mut action = None;
        if !self.started {
            self.started = true;
            self.refresh_local(ui, core);
        }
        poll(&mut self.ff_task, |r| self.ff = Some(r));
        if self.ff.as_ref().is_some_and(|(_, s)| s.busy) && self.ff_task.is_none() {
            // download running: refresh the percentage every second; the version (it runs ffmpeg) only once done
            let ctx = ui.ctx().clone();
            let before = self.ff.as_ref().and_then(|(v, _)| v.clone());
            self.ff_task = Some(Task::spawn(core, ui.ctx(), async move {
                tokio::time::sleep(Duration::from_secs(1)).await;
                ctx.request_repaint();
                blocking(move || {
                    let st = ffmpeg::status();
                    (if st.busy { before } else { ffmpeg::version() }, st)
                })
                .await
                .unwrap_or_default()
            }));
        }
        if self.ff_installing && self.ff.as_ref().is_some_and(|(_, s)| !s.busy) && self.ff_task.is_none() {
            self.ff_installing = false;
        }
        self.poll_agent();
        self.poll_ytdlp();

        // -- agent
        section(ui, t::AGENT, |ui| {
            ui.label(&self.agent_text);
            right(ui, |ui| {
                let busy = self.agent_task.is_some() || self.install_task.is_some();
                let label = match &self.agent_btn {
                    AgentBtn::Check => t::CHECK.to_owned(),
                    AgentBtn::Install(_) => t::INSTALL.to_owned(),
                    AgentBtn::Github => super::strings::M_GITHUB.to_owned(),
                };
                if ui.add_enabled(!busy, egui::Button::new(label)).clicked() {
                    match &self.agent_btn {
                        AgentBtn::Check => {
                            self.agent_text = format!("{} · {}", t::version(crate::VERSION), t::CHECKING);
                            self.agent_task = Some(Task::spawn(core, ui.ctx(), crate::update::latest_version()));
                        }
                        AgentBtn::Install(v) => {
                            if crate::update::installed_build() {
                                self.agent_text = t::downloading_agent(v);
                                let v = v.clone();
                                self.install_task = Some(Task::spawn(core, ui.ctx(), async move {
                                    crate::update::download_installer(&v).await.map_err(|e| e.to_string())
                                }));
                            } else {
                                crate::win::open(&crate::update::releases_page());
                            }
                        }
                        AgentBtn::Github => crate::win::open(&crate::update::releases_page()),
                    }
                }
            });
        });
        if let Some(r) = self.install_task.as_ref().and_then(Task::take) {
            self.install_task = None;
            match r {
                Ok(path) => {
                    // the installer replaces the files and starts the new agent
                    if std::process::Command::new(&path).spawn().is_ok() {
                        action = Some(Action::Quit);
                    }
                }
                Err(e) => {
                    self.agent_text = t::agent_download_failed(&e);
                    self.agent_btn = AgentBtn::Github;
                }
            }
        }

        // -- yt-dlp
        section(ui, t::YTDLP, |ui| {
            ui.label(&self.yt_text);
            right(ui, |ui| {
                let busy = self.yt_check.is_some() || self.yt_update.is_some();
                let label = match self.yt_btn {
                    YtBtn::Check => t::CHECK,
                    YtBtn::Update => t::UPDATE,
                };
                if ui.add_enabled(!busy, egui::Button::new(label)).clicked() {
                    match self.yt_btn {
                        YtBtn::Check => self.yt_check = Some(Task::spawn(core, ui.ctx(), ytdlp::latest())),
                        YtBtn::Update => {
                            self.yt_text = t::DOWNLOADING_YTDLP.into();
                            self.yt_update = Some(Task::spawn(core, ui.ctx(), async {
                                ytdlp::update(|_| {}).await.map_err(|e| e.to_string())
                            }));
                        }
                    }
                }
            });
        });

        // -- ffmpeg
        section(ui, t::FFMPEG, |ui| {
            let (text, button) = match &self.ff {
                None => ("…".to_owned(), None),
                Some((_, s)) if s.busy || self.ff_installing => (t::ff_busy(s.pct), None),
                Some((v, s)) if s.path.is_some() && !s.own => (t::ff_path(v.as_deref().unwrap_or("?")), None),
                Some((v, s)) if s.path.is_some() => (t::ff_own(v.as_deref().unwrap_or("?")), Some(t::FF_REDOWNLOAD)),
                Some((_, s)) => {
                    let mut text = t::FF_NONE.to_owned();
                    if !s.error.is_empty() {
                        text = format!("{text}\n{}", t::ff_last_error(&s.error));
                    }
                    (text, Some(t::FF_DOWNLOAD))
                }
            };
            ui.add(egui::Label::new(text).wrap());
            right(ui, |ui| {
                if ui.add_enabled(button.is_some(), egui::Button::new(button.unwrap_or(t::FF_DOWNLOAD))).clicked() {
                    self.ff_installing = true;
                    if let Some((_, s)) = self.ff.as_mut() {
                        s.busy = true;
                        s.pct = 0;
                    }
                    core.spawn(async {
                        let _ = ffmpeg::install().await;
                    });
                }
            });
        });

        // -- options
        section(ui, t::OPTIONS, |ui| {
            ui.vertical(|ui| {
                let mut auto = core.cfg().get().auto_update;
                if ui.checkbox(&mut auto, t::AUTO_UPDATE).changed() {
                    core.cfg().update(|c| c.auto_update = auto);
                }
                let mut logon = crate::win::autostart_enabled();
                if ui.checkbox(&mut logon, super::strings::M_AUTOSTART).changed()
                    && let Err(e) = crate::win::set_autostart(logon)
                {
                    tracing::warn!("autostart: {e}");
                }
                ui.horizontal(|ui| {
                    ui.label(t::PORT);
                    ui.add(egui::TextEdit::singleline(&mut self.port).desired_width(70.0));
                    if ui.button(t::SAVE).clicked() {
                        match self.port.trim().parse::<u16>() {
                            Ok(p) if p > 1024 => {
                                core.cfg().update(|c| c.relay_port = p);
                                self.port_msg = msg(t::PORT_SAVED, Look::Hint);
                            }
                            _ => self.port_msg = msg(t::PORT_RANGE, Look::Bad),
                        }
                    }
                });
                if let Some((m, c)) = &self.port_msg {
                    ui.label(RichText::new(m).color(*c));
                }
            });
        });

        action
    }

    fn poll_agent(&mut self) {
        poll(&mut self.agent_task, |v| match v {
            None => self.agent_text = format!("{} · {}", t::version(crate::VERSION), t::NO_RELEASE),
            Some(v) if crate::update::newer_than_running(&v) => {
                self.agent_text = format!("{} · {}", t::version(crate::VERSION), t::has(&v));
                self.agent_btn = AgentBtn::Install(v);
            }
            Some(_) => self.agent_text = format!("{} · {}", t::version(crate::VERSION), t::LATEST),
        });
    }

    fn poll_ytdlp(&mut self) {
        poll(&mut self.yt_check, |r| {
            let have = ytdlp::version();
            let have_s = have.clone().unwrap_or_else(|| "—".into());
            match r {
                None => self.yt_text = format!("{} · {}", t::version(&have_s), t::CANNOT_CHECK),
                Some(r) if Some(&r) != have.as_ref() => {
                    self.yt_text = format!("{} · {}", t::version(&have_s), t::has(&r));
                    self.yt_btn = YtBtn::Update;
                }
                Some(_) => self.yt_text = format!("{} · {}", t::version(&have_s), t::LATEST),
            }
        });
        poll(&mut self.yt_update, |r| {
            self.yt_btn = YtBtn::Check;
            self.yt_text = match r {
                Ok(v) => t::ytdlp_installed(&v),
                Err(e) => t::ytdlp_failed(&e),
            };
        });
    }
}

pub enum Action {
    Quit,
}

/// A titled box, like ttk.LabelFrame.
fn section(ui: &mut egui::Ui, title: &str, body: impl FnOnce(&mut egui::Ui)) {
    ui.add_space(4.0);
    egui::Frame::group(ui.style()).inner_margin(egui::Margin::symmetric(10, 8)).show(ui, |ui| {
        ui.set_width(ui.available_width());
        ui.label(RichText::new(title).strong());
        ui.horizontal(body);
    });
}

fn right(ui: &mut egui::Ui, body: impl FnOnce(&mut egui::Ui)) {
    ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), body);
}
