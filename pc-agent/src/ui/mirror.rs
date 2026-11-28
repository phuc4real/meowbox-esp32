//! The speaker mirror at the top of the window (B2): the speaker's name, IP and link state, and the numbers its
//! Computer screen shows, taken from the `stats` and `media` messages the core just sent, so the window shows
//! exactly what the speaker gets. Not connected: the block greys out and its last line says why and what to do,
//! with the one button that does it.

use super::strings::{f, s};
use super::theme::{Look, display_family};
use super::widgets;
use crate::config::Config;
use crate::link::Mirror;
use crate::state::{AgentState, LinkError};
use eframe::egui::{self, FontId, RichText, Ui};
use serde_json::Value;

/// What the mirror's button asks for.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Act {
    EnterIp,
    EnterToken,
    Retry,
    TakeOver,
    ChangePort,
    SeeUpdate,
    OpenWeb,
    Resume,
}

/// The sentence and button for a link that is not up, or None while connected.
pub fn why(st: &AgentState, cfg: &Config, ever_connected: bool) -> Option<(String, Option<(&'static str, Act)>)> {
    let t = s();
    if st.connected {
        return None;
    }
    let want = [st.name.as_str(), cfg.speaker.as_str()].into_iter().find(|x| !x.is_empty()).unwrap_or("MeowBox");
    let ip = [st.ip(), cfg.known_host()].into_iter().find(|x| !x.is_empty()).unwrap_or("").to_owned();
    Some(match st.error {
        LinkError::Busy => (f(t.why_busy, &[&want, &st.busy_with]), Some((t.do_take_over, Act::TakeOver))),
        LinkError::Token => (t.why_token.into(), Some((t.do_enter_token, Act::EnterToken))),
        LinkError::Port => (f(t.why_port, &[&cfg.relay_port]), Some((t.do_change_port, Act::ChangePort))),
        LinkError::ProtoAgent => (t.why_proto_agent.into(), Some((t.do_see_update, Act::SeeUpdate))),
        LinkError::ProtoSpeaker => (t.why_proto_speaker.into(), Some((t.do_open_web, Act::OpenWeb))),
        _ if cfg.last_host.is_empty() && cfg.host.is_empty() => {
            (t.why_no_ip.into(), Some((t.do_enter_ip, Act::EnterIp)))
        }
        _ if !st.searching.is_empty() || st.error == LinkError::None => (f(t.why_searching, &[&ip]), None),
        _ if ever_connected => (f(t.why_lost, &[&st.retry_in]), Some((t.do_retry, Act::Retry))),
        _ => (f(t.why_not_found, &[&ip]), Some((t.do_retry, Act::Retry))),
    })
}

fn num(v: &Value, k: &str) -> Option<i64> {
    v.get(k).and_then(Value::as_f64).map(|x| x.round() as i64)
}

pub fn show(ui: &mut Ui, look: &Look, st: &AgentState, cfg: &Config, m: &Mirror, ever: bool) -> Option<Act> {
    let t = s();
    let mut act = None;
    let up = st.connected;
    let (main, dim) = (look.text(), look.dim());
    let fg = if up { main } else { dim };
    let mut frame = egui::Frame::NONE.fill(look.surface()).inner_margin(egui::Margin::symmetric(16, 12));
    if look.pal().surface == look.pal().bg {
        // Brutalism: surface = background, so the block needs its border
        frame = frame.stroke(egui::Stroke::new(look.border(), look.line()));
    }
    frame.corner_radius(egui::CornerRadius::same(look.radius())).show(ui, |ui| {
        ui.set_width(ui.available_width());
        // name, IP, state in words
        ui.horizontal(|ui| {
            let name =
                [st.name.as_str(), cfg.speaker.as_str()].into_iter().find(|x| !x.is_empty()).unwrap_or("MeowBox");
            ui.label(RichText::new(name).font(FontId::new(22.0, display_family())).color(fg));
            let ip = if st.ip().is_empty() { cfg.known_host().split(':').next().unwrap_or("") } else { st.ip() };
            ui.label(RichText::new(ip).color(dim));
            widgets::right(ui, |ui| {
                let word = if up {
                    t.mi_connected
                } else if !st.searching.is_empty() {
                    t.mi_searching
                } else {
                    t.mi_off
                };
                let glyph = if up { "●" } else { "○" };
                ui.label(RichText::new(format!("{glyph} {word}")).color(fg));
            });
        });
        ui.add_space(4.0);
        // the numbers of the speaker's Computer screen
        let stats = m.stats.as_ref().filter(|_| up);
        ui.horizontal(|ui| {
            let cell = |ui: &mut Ui, label: &str, value: Option<String>| {
                ui.vertical(|ui| {
                    ui.label(RichText::new(label).size(12.0).color(dim));
                    let v = value.unwrap_or_else(|| "–".into());
                    ui.label(RichText::new(v).font(FontId::new(30.0, display_family())).color(fg));
                });
                ui.add_space(18.0);
            };
            let pct = |k: &str| stats.and_then(|v| num(v, k)).map(|n| format!("{n}%"));
            cell(ui, "CPU", pct("cpu"));
            cell(ui, "RAM", pct("ram"));
            if stats.is_none_or(|v| v.get("gpu").is_some()) {
                cell(ui, "GPU", pct("gpu"));
            }
            if stats.is_none_or(|v| v.get("gpu_temp").is_some()) {
                cell(ui, t.mi_temp, stats.and_then(|v| num(v, "gpu_temp")).map(|n| format!("{n}°")));
            }
        });
        // what is playing
        if up {
            let media = m.media.as_ref();
            let title = media.and_then(|v| v.get("title")).and_then(Value::as_str).unwrap_or("");
            let artist = media.and_then(|v| v.get("artist")).and_then(Value::as_str).unwrap_or("");
            let playing = media.and_then(|v| v.get("state")).and_then(Value::as_str) == Some("playing");
            // one label, truncated as a whole: a second one after a truncated label would push past the edge
            let paused = if playing { String::new() } else { format!(" {}", t.mi_media_paused) };
            let line = match (title.is_empty(), artist.is_empty()) {
                (true, _) => RichText::new(t.mi_nothing_playing).color(dim),
                (false, true) => RichText::new(format!("♪ {title}{paused}")).color(main),
                (false, false) => RichText::new(format!("♪ {title} · {artist}{paused}")).color(main),
            };
            ui.add(egui::Label::new(line).truncate());
            if cfg.paused {
                ui.horizontal(|ui| {
                    ui.label(RichText::new(t.mi_paused).color(dim));
                    if widgets::button(ui, look, t.mi_resume).clicked() {
                        act = Some(Act::Resume);
                    }
                });
            }
        } else if let Some((text, button)) = why(st, cfg, ever) {
            ui.add_space(2.0);
            let bad = !matches!(button, None | Some((_, Act::Retry)));
            if bad {
                widgets::problem(ui, look, &text);
            } else {
                ui.add(egui::Label::new(RichText::new(text).color(main)).wrap());
            }
            if let Some((label, a)) = button {
                ui.add_space(2.0);
                if widgets::primary(ui, look, label).clicked() {
                    act = Some(a);
                }
            }
        }
    });
    act
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_problem_says_what_to_do() {
        let mut cfg = Config::default();
        let st = |error| AgentState { error, busy_with: "DESKTOP-ABC".into(), ..Default::default() };
        let (_, b) = why(&st(LinkError::NotFound), &cfg, false).unwrap();
        assert_eq!(b.map(|b| b.1), Some(Act::EnterIp), "no IP yet");
        cfg.last_host = "192.168.1.50".into();
        for (e, want) in [
            (LinkError::NotFound, Act::Retry),
            (LinkError::Token, Act::EnterToken),
            (LinkError::Busy, Act::TakeOver),
            (LinkError::Port, Act::ChangePort),
            (LinkError::ProtoAgent, Act::SeeUpdate),
            (LinkError::ProtoSpeaker, Act::OpenWeb),
        ] {
            let (text, b) = why(&st(e), &cfg, true).unwrap();
            assert!(!text.is_empty());
            assert_eq!(b.map(|b| b.1), Some(want), "{e:?}");
        }
        let searching = AgentState { searching: "start".into(), ..Default::default() };
        assert_eq!(why(&searching, &cfg, false).unwrap().1, None, "nothing to do while searching");
        assert!(why(&AgentState { connected: true, ..Default::default() }, &cfg, true).is_none());
    }
}
