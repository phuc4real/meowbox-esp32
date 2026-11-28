//! "Cài đặt" (B2): one left-aligned list, label on the left, value and control on the right, groups apart by space.
//! Agent version and update, yt-dlp, ffmpeg (downloads with a progress bar), daily update check, run at Windows
//! start, relay port, language; at the bottom the log folder, GitHub and about.

use super::strings::{f, s};
use super::task::{Task, blocking, poll};
use super::theme::Look;
use super::widgets::{self, Mark};
use crate::link::CoreHandle;
use crate::tools::{ffmpeg, ytdlp};
use eframe::egui::{self, Ui};
use std::time::Duration;

enum AgentBtn {
    Check,
    Install(String),
    Github,
}

pub enum Act {
    /// The new installer was started: the agent quits so it can replace the files.
    Quit,
}

pub struct Settings {
    agent_mark: Mark,
    agent_btn: AgentBtn,
    agent_task: Option<Task<Option<String>>>,
    install_task: Option<Task<Result<std::path::PathBuf, String>>>,

    yt_mark: Mark,
    yt_update_btn: bool,
    yt_check: Option<Task<Option<String>>>,
    yt_update: Option<Task<Result<String, String>>>,

    /// (version, status) of ffmpeg, read off the UI thread.
    ff: Option<(Option<String>, ffmpeg::Status)>,
    ff_task: Option<Task<(Option<String>, ffmpeg::Status)>>,
    ff_installing: bool,

    port: String,
    port_note: Option<Mark>,
    started: bool,
    /// The port field gets the cursor (the mirror's "Change port").
    pub focus_port: bool,
}

impl Settings {
    pub fn new(core: &CoreHandle) -> Self {
        Self {
            agent_mark: Mark::Dim(String::new()),
            agent_btn: AgentBtn::Check,
            agent_task: None,
            install_task: None,
            yt_mark: Mark::Dim("…".into()),
            yt_update_btn: false,
            yt_check: None,
            yt_update: None,
            ff: None,
            ff_task: None,
            ff_installing: false,
            port: core.cfg().get().relay_port.to_string(),
            port_note: None,
            started: false,
            focus_port: false,
        }
    }

    /// Check for a newer agent now (the tray's "Có bản x.y.z" and the mirror's "See the new version").
    pub fn check_agent(&mut self, ui: &Ui, core: &CoreHandle) {
        if self.agent_task.is_none() && self.install_task.is_none() {
            self.agent_mark = Mark::Wait(s().s_checking.into());
            self.agent_task = Some(Task::spawn(core, ui.ctx(), crate::update::latest_version()));
        }
    }

    fn refresh_local(&mut self, ui: &Ui, core: &CoreHandle) {
        let t = s();
        match ytdlp::version() {
            Some(v) => self.yt_mark = Mark::Text(f(t.s_version, &[&v])),
            None => {
                self.yt_mark = Mark::Bad(t.s_ytdlp_missing.into());
                self.yt_update_btn = true;
            }
        }
        self.ff_task = Some(Task::spawn(core, ui.ctx(), async {
            blocking(|| (ffmpeg::version(), ffmpeg::status())).await.unwrap_or_default()
        }));
    }

    pub fn ui(&mut self, ui: &mut Ui, look: &Look, core: &CoreHandle) -> Option<Act> {
        let t = s();
        let mut act = None;
        if !self.started {
            self.started = true;
            self.refresh_local(ui, core);
        }
        self.poll(ui, core);

        // -- agent
        widgets::row(ui, look, t.s_agent, |ui| {
            ui.vertical(|ui| {
                ui.horizontal(|ui| {
                    ui.label(f(t.s_version, &[&crate::VERSION]));
                    widgets::mark(ui, look, &self.agent_mark);
                });
                if self.install_task.is_some() {
                    let (d, total) = crate::update::METER.get();
                    widgets::progress(ui, look, d, total);
                }
            });
            widgets::right(ui, |ui| {
                let busy = self.agent_task.is_some() || self.install_task.is_some();
                let (label, main) = match &self.agent_btn {
                    AgentBtn::Check => (t.s_check, false),
                    AgentBtn::Install(_) => (t.s_install, true),
                    AgentBtn::Github => (t.s_github, false),
                };
                let clicked = if main && !busy {
                    widgets::primary(ui, look, label).clicked()
                } else {
                    widgets::button_enabled(ui, look, label, !busy).clicked()
                };
                if clicked {
                    match &self.agent_btn {
                        AgentBtn::Check => self.check_agent(ui, core),
                        AgentBtn::Install(v) if crate::update::installed_build() => {
                            self.agent_mark = Mark::Wait(t.s_downloading.into());
                            let v = v.clone();
                            self.install_task = Some(Task::spawn(core, ui.ctx(), async move {
                                crate::update::download_installer(&v).await.map_err(|e| e.to_string())
                            }));
                        }
                        AgentBtn::Install(_) | AgentBtn::Github => crate::win::open(&crate::update::releases_page()),
                    }
                }
            });
        });
        if let Some(r) = self.install_task.as_ref().and_then(Task::take) {
            self.install_task = None;
            match r {
                // the installer replaces the files and starts the new agent
                Ok(path) if std::process::Command::new(&path).spawn().is_ok() => act = Some(Act::Quit),
                Ok(_) => self.agent_btn = AgentBtn::Github,
                Err(e) => {
                    self.agent_mark = Mark::Bad(f(t.s_agent_failed, &[&e]));
                    self.agent_btn = AgentBtn::Github;
                }
            }
        }

        // -- yt-dlp
        widgets::row(ui, look, t.s_ytdlp, |ui| {
            ui.vertical(|ui| {
                widgets::mark(ui, look, &self.yt_mark);
                if self.yt_update.is_some() {
                    let (d, total) = ytdlp::METER.get();
                    widgets::progress(ui, look, d, total);
                }
            });
            widgets::right(ui, |ui| {
                let busy = self.yt_check.is_some() || self.yt_update.is_some();
                let label = if self.yt_update_btn { t.s_update } else { t.s_check };
                if widgets::button_enabled(ui, look, label, !busy).clicked() {
                    if self.yt_update_btn {
                        self.yt_mark = Mark::Wait(t.s_downloading.into());
                        self.yt_update = Some(Task::spawn(core, ui.ctx(), async {
                            ytdlp::update(|_| {}).await.map_err(|e| e.to_string())
                        }));
                    } else {
                        self.yt_mark = Mark::Wait(t.s_checking.into());
                        self.yt_check = Some(Task::spawn(core, ui.ctx(), ytdlp::latest()));
                    }
                }
            });
        });

        // -- ffmpeg
        widgets::row(ui, look, t.s_ffmpeg, |ui| {
            let (m, button) = match &self.ff {
                None => (Mark::Dim("…".into()), None),
                Some((_, st)) if st.busy || self.ff_installing => (Mark::Wait(t.s_downloading.into()), None),
                Some((v, st)) if st.path.is_some() && !st.own => {
                    (Mark::Text(f(t.s_ff_path, &[&v.as_deref().unwrap_or("?")])), None)
                }
                Some((v, st)) if st.path.is_some() => {
                    (Mark::Text(f(t.s_ff_own, &[&v.as_deref().unwrap_or("?")])), Some(t.s_ff_redownload))
                }
                Some((_, st)) if !st.error.is_empty() => {
                    (Mark::Bad(f(t.s_ff_last_error, &[&st.error])), Some(t.s_ff_redownload))
                }
                Some(_) => (Mark::Dim(t.s_ff_none.into()), Some(t.s_ff_download)),
            };
            let busy = button.is_none() && self.ff.as_ref().is_some_and(|(_, st)| st.busy || self.ff_installing);
            ui.vertical(|ui| {
                ui.set_max_width((ui.available_width() - 120.0).max(120.0));
                match &m {
                    Mark::Dim(text) | Mark::Bad(text) if text.chars().count() > 40 => {
                        let color = if matches!(m, Mark::Bad(_)) { look.alert() } else { look.dim() };
                        ui.add(egui::Label::new(egui::RichText::new(text).color(color)).wrap());
                    }
                    _ => widgets::mark(ui, look, &m),
                }
                if busy {
                    let (d, total) = ffmpeg::METER.get();
                    widgets::progress(ui, look, d, total);
                }
            });
            if let Some(label) = button {
                widgets::right(ui, |ui| {
                    if widgets::button(ui, look, label).clicked() {
                        self.ff_installing = true;
                        if let Some((_, st)) = self.ff.as_mut() {
                            st.busy = true;
                        }
                        core.spawn(async {
                            let _ = ffmpeg::install().await;
                        });
                    }
                });
            }
        });

        ui.add_space(12.0);
        // -- options
        let mut auto = core.cfg().get().auto_update;
        widgets::row(ui, look, t.s_auto_update, |ui| {
            if ui.checkbox(&mut auto, t.s_auto_update_box).changed() {
                core.cfg().update(|c| c.auto_update = auto);
            }
        });
        let mut logon = crate::win::autostart_enabled();
        widgets::row(ui, look, t.s_autostart, |ui| {
            if ui.checkbox(&mut logon, t.s_autostart_box).changed()
                && let Err(e) = crate::win::set_autostart(logon)
            {
                tracing::warn!("autostart: {e}");
            }
        });
        widgets::labelled_row(ui, look, t.s_port, |ui, label| {
            let r = ui.add(egui::TextEdit::singleline(&mut self.port).desired_width(70.0)).labelled_by(label);
            if std::mem::take(&mut self.focus_port) {
                r.request_focus();
            }
            let enter = r.lost_focus() && ui.input(|i| i.key_pressed(egui::Key::Enter));
            if widgets::button(ui, look, t.s_save).clicked() || enter {
                self.port_note = Some(match self.port.trim().parse::<u16>() {
                    Ok(p) if p > 1024 => {
                        core.cfg().update(|c| c.relay_port = p);
                        Mark::Good(t.s_port_saved.into())
                    }
                    _ => Mark::Bad(t.s_port_range.into()),
                });
            }
        });
        if let Some(n) = &self.port_note {
            ui.horizontal(|ui| {
                ui.add_space(widgets::LABEL_W + 8.0);
                widgets::mark(ui, look, n);
            });
        }
        let lang = core.cfg().get().lang;
        widgets::row(ui, look, t.s_lang, |ui| {
            let name = |l: &str| match l {
                "vi" => "Tiếng Việt",
                "en" => "English",
                _ => t.s_lang_auto,
            };
            let mut pick = lang.clone();
            egui::ComboBox::from_id_salt("lang").selected_text(name(&pick)).width(160.0).show_ui(ui, |ui| {
                for l in ["auto", "vi", "en"] {
                    ui.selectable_value(&mut pick, l.to_owned(), name(l));
                }
            });
            if pick != lang {
                super::strings::set_lang(&pick);
                core.cfg().update(|c| c.lang = pick);
            }
        });

        ui.add_space(16.0);
        ui.horizontal(|ui| {
            if widgets::button(ui, look, t.s_log_dir).clicked() {
                crate::win::open(&crate::app_dir().to_string_lossy());
            }
            if widgets::button(ui, look, t.s_github).clicked() {
                crate::win::open(&format!("https://github.com/{}", crate::REPO));
            }
            if widgets::button(ui, look, &f(t.s_about, &[&crate::VERSION])).clicked() {
                crate::win::open(&format!("https://github.com/{}#readme", crate::REPO));
            }
        });
        act
    }

    fn poll(&mut self, ui: &Ui, core: &CoreHandle) {
        let t = s();
        poll(&mut self.ff_task, |r| self.ff = Some(r));
        if self.ff.as_ref().is_some_and(|(_, st)| st.busy) && self.ff_task.is_none() {
            // download running: refresh the status every second; the version (it runs ffmpeg) only once done
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
        if self.ff_installing && self.ff.as_ref().is_some_and(|(_, st)| !st.busy) && self.ff_task.is_none() {
            self.ff_installing = false;
        }
        if self.install_task.is_some() || self.yt_update.is_some() {
            ui.ctx().request_repaint_after(Duration::from_millis(250)); // progress bars
        }
        poll(&mut self.agent_task, |v| match v {
            None => self.agent_mark = Mark::Dim(t.s_no_release.into()),
            Some(v) if crate::update::newer_than_running(&v) => {
                self.agent_mark = Mark::Good(f(t.s_has, &[&v]));
                self.agent_btn = AgentBtn::Install(v);
            }
            Some(_) => self.agent_mark = Mark::Good(t.s_latest.into()),
        });
        poll(&mut self.yt_check, |r| {
            let have = ytdlp::version().unwrap_or_else(|| "—".into());
            match r {
                None => self.yt_mark = Mark::Dim(format!("{} · {}", f(t.s_version, &[&have]), t.s_cannot_check)),
                Some(r) if r != have => {
                    self.yt_mark = Mark::Good(format!("{} · {}", f(t.s_version, &[&have]), f(t.s_has, &[&r])));
                    self.yt_update_btn = true;
                }
                Some(_) => self.yt_mark = Mark::Good(format!("{} · {}", f(t.s_version, &[&have]), t.s_latest)),
            }
        });
        poll(&mut self.yt_update, |r| {
            self.yt_update_btn = false;
            self.yt_mark = match r {
                Ok(v) => Mark::Good(f(t.s_ytdlp_installed, &[&v])),
                Err(e) => Mark::Bad(f(t.s_ytdlp_failed, &[&e])),
            };
        });
    }
}
