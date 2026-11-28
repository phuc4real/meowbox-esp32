//! The few widgets the window is built from, drawn in the theme's shape (B1/B2): buttons (accent fill only for the
//! one main action), tabs, list rows (label left, value and control right), check marks with words, progress bars.

use super::theme::{Btn, Look};
use eframe::egui::{self, Color32, CornerRadius, Response, RichText, Stroke, Ui};

/// Width of the label column of a list row.
pub const LABEL_W: f32 = 150.0;

fn paint_button(ui: &mut Ui, look: &Look, text: &str, primary: bool, enabled: bool) -> Response {
    let (fill, fg) = if primary { (look.accent(), look.on_accent()) } else { (look.surface(), look.text()) };
    let border = look.border();
    let stroke = match (look.def.shape.btn, primary) {
        // Zen: secondary buttons are text with a hairline
        (Btn::Text, false) => Stroke::new(1.0, look.line()),
        _ => Stroke::new(border, look.line()),
    };
    // Brutalism's hard offset shadow is a shape, not an effect: drawn under the button
    let shadow = look.def.shape.shadow;
    let slot = ui.painter().add(egui::Shape::Noop);
    let b = egui::Button::new(RichText::new(text).color(fg))
        .fill(fill)
        .stroke(stroke)
        .corner_radius(CornerRadius::same(look.radius()));
    let r = ui.add_enabled(enabled, b);
    if shadow != (0, 0) && enabled {
        let rect = r.rect.translate(egui::vec2(f32::from(shadow.0), f32::from(shadow.1)) * 0.6);
        ui.painter().set(slot, egui::Shape::rect_filled(rect, CornerRadius::same(look.radius()), look.line()));
    }
    r
}

/// A normal button.
pub fn button(ui: &mut Ui, look: &Look, text: &str) -> Response {
    paint_button(ui, look, text, false, true)
}

pub fn button_enabled(ui: &mut Ui, look: &Look, text: &str, enabled: bool) -> Response {
    paint_button(ui, look, text, false, enabled)
}

/// The main action of a state (one per screen at most): accent fill, `on_accent` text.
pub fn primary(ui: &mut Ui, look: &Look, text: &str) -> Response {
    paint_button(ui, look, text, true, true)
}

/// A tab: the selected one is an accent fill, the others plain text.
pub fn tab(ui: &mut Ui, look: &Look, selected: bool, text: &str) -> Response {
    let (fill, fg, stroke) = if selected {
        (look.accent(), look.on_accent(), Stroke::NONE)
    } else {
        (Color32::TRANSPARENT, look.text(), Stroke::NONE)
    };
    let b = egui::Button::new(RichText::new(text).color(fg).size(15.0))
        .fill(fill)
        .stroke(stroke)
        .corner_radius(CornerRadius::same(look.radius()))
        .selected(selected);
    ui.add(b)
}

/// A list row: the label in a fixed column on the left, `body` after it.
pub fn row<R>(ui: &mut Ui, look: &Look, label: &str, body: impl FnOnce(&mut Ui) -> R) -> R {
    labelled_row(ui, look, label, |ui, _| body(ui))
}

/// The same, with the label's id for a field "labelled by" it (screen readers).
pub fn labelled_row<R>(ui: &mut Ui, look: &Look, label: &str, body: impl FnOnce(&mut Ui, egui::Id) -> R) -> R {
    ui.horizontal(|ui| {
        ui.set_min_height(30.0);
        let id = ui.allocate_ui_with_layout(
            egui::vec2(LABEL_W, 28.0),
            egui::Layout::left_to_right(egui::Align::Center),
            |ui| {
                ui.set_width(LABEL_W);
                ui.label(RichText::new(label).color(look.text())).id
            },
        );
        body(ui, id.inner)
    })
    .inner
}

/// Lay out `body` from the right edge.
pub fn right<R>(ui: &mut Ui, body: impl FnOnce(&mut Ui) -> R) -> R {
    ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), body).inner
}

/// A result told by a glyph and a word, never by colour alone.
#[derive(Debug, Clone, PartialEq)]
pub enum Mark {
    /// A plain value in the text colour.
    Text(String),
    Wait(String),
    Good(String),
    Bad(String),
    Dim(String),
}

pub fn mark(ui: &mut Ui, look: &Look, m: &Mark) {
    let (glyph, text, color) = match m {
        Mark::Text(t) => ("", t, look.text()),
        Mark::Wait(t) => ("…", t, look.dim()),
        Mark::Good(t) => ("✓", t, look.ok()),
        Mark::Bad(t) => ("✕", t, look.alert()),
        Mark::Dim(t) => ("", t, look.dim()),
    };
    let s = if glyph.is_empty() {
        text.clone()
    } else if text.is_empty() {
        glyph.into()
    } else {
        format!("{glyph} {text}")
    };
    ui.add(egui::Label::new(RichText::new(s).color(color)).truncate());
}

/// Hint or explanation text (wraps).
pub fn note(ui: &mut Ui, look: &Look, text: &str) {
    ui.add(egui::Label::new(RichText::new(text).color(look.dim())).wrap());
}

/// An error that says what to do (wraps).
pub fn problem(ui: &mut Ui, look: &Look, text: &str) {
    ui.add(egui::Label::new(RichText::new(text).color(look.alert())).wrap());
}

/// A download: thin bar in the accent, "3 / 75 MB" next to it.
pub fn progress(ui: &mut Ui, look: &Look, done: u64, total: u64) {
    let mb = |b: u64| format!("{:.0}", b as f64 / 1_048_576.0);
    ui.horizontal(|ui| {
        let frac = if total > 0 { done as f32 / total as f32 } else { 0.0 };
        ui.add(
            egui::ProgressBar::new(frac.clamp(0.0, 1.0))
                .desired_width(180.0)
                .desired_height(8.0)
                .fill(look.accent())
                .corner_radius(CornerRadius::same(look.radius().min(4))),
        );
        let t = if total > 0 {
            super::strings::f(super::strings::s().s_mb, &[&mb(done), &mb(total)])
        } else {
            super::strings::s().s_downloading.to_owned()
        };
        ui.label(RichText::new(t).color(look.dim()));
    });
}
