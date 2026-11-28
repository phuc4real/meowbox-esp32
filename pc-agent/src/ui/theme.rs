//! The window wears the speaker's theme (docs/enhance_agent.md, B1): palettes, shapes and display fonts generated
//! from firmware/themes/*.json (`themes_gen.rs`), light or dark as Windows is set. Body text stays Segoe UI; the
//! theme's display font is for headings and the mirror's big numbers.
//!
//! Colour rules: the accent is only ever a fill with `on_accent` text, never text on `bg`; links are `text`
//! underlined; status words are never told by colour alone. `tests` checks every text/background pair the window
//! uses for contrast (WCAG), so a theme change cannot make the window unreadable unnoticed.

use super::themes_gen::{DEFAULT, THEMES};
use eframe::egui::{self, Color32, CornerRadius, FontData, FontDefinitions, FontFamily, Stroke};
use std::path::PathBuf;
use std::sync::Arc;

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Palette {
    pub bg: [u8; 3],
    pub surface: [u8; 3],
    pub text: [u8; 3],
    pub dim: [u8; 3],
    pub line: [u8; 3],
    pub accent: [u8; 3],
    pub on_accent: [u8; 3],
    pub ok: [u8; 3],
    pub warn: [u8; 3],
    pub alert: [u8; 3],
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Btn {
    Fill,
    Cut,
    Text,
    Raw,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Shape {
    pub radius: u8,
    pub border: u8,
    pub btn: Btn,
    pub shadow: (i8, i8),
}

#[derive(Debug)]
pub struct ThemeDef {
    pub id: &'static str,
    pub name: &'static str,
    pub light: Palette,
    pub dark: Palette,
    pub shape: Shape,
    /// A file in assets/fonts, or "system:<file>" in the Windows font folder.
    pub display_font: &'static str,
}

/// The theme by id; an unknown id (a newer firmware's theme) gives the default one.
pub fn find(id: &str) -> &'static ThemeDef {
    THEMES.iter().find(|t| t.id == id).or_else(|| THEMES.iter().find(|t| t.id == DEFAULT)).unwrap_or(&THEMES[0])
}

/// Windows "app mode" is dark (Settings → Personalisation → Colours).
pub fn windows_dark() -> bool {
    winreg::RegKey::predef(winreg::enums::HKEY_CURRENT_USER)
        .open_subkey(r"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize")
        .and_then(|k| k.get_value::<u32, _>("AppsUseLightTheme"))
        .is_ok_and(|v| v == 0)
}

/// What the window looks like now: a theme in light or dark.
#[derive(Debug, Clone, Copy)]
pub struct Look {
    pub def: &'static ThemeDef,
    pub dark: bool,
}

impl Look {
    pub fn new(theme: &str, dark: bool) -> Self {
        Self { def: find(theme), dark }
    }
    pub fn pal(&self) -> &'static Palette {
        if self.dark { &self.def.dark } else { &self.def.light }
    }
    pub fn c(&self, f: fn(&Palette) -> [u8; 3]) -> Color32 {
        rgb(f(self.pal()))
    }
    pub fn text(&self) -> Color32 {
        self.c(|p| p.text)
    }
    pub fn dim(&self) -> Color32 {
        self.c(|p| p.dim)
    }
    pub fn ok(&self) -> Color32 {
        self.c(|p| p.ok)
    }
    pub fn alert(&self) -> Color32 {
        self.c(|p| p.alert)
    }
    pub fn surface(&self) -> Color32 {
        self.c(|p| p.surface)
    }
    pub fn line(&self) -> Color32 {
        self.c(|p| p.line)
    }
    pub fn accent(&self) -> Color32 {
        self.c(|p| p.accent)
    }
    pub fn on_accent(&self) -> Color32 {
        self.c(|p| p.on_accent)
    }
    /// Corner radius for buttons and fields (Zen's 20 px pills are kept smaller on 28 px widgets).
    pub fn radius(&self) -> u8 {
        self.def.shape.radius.min(14)
    }
    pub fn border(&self) -> f32 {
        f32::from(self.def.shape.border.clamp(1, 3))
    }
}

pub fn rgb(c: [u8; 3]) -> Color32 {
    Color32::from_rgb(c[0], c[1], c[2])
}

/// egui's visuals from the look: no shadows, flat panels, accent only as a fill.
pub fn apply(ctx: &egui::Context, look: &Look) {
    let p = look.pal();
    let (bg, surface, text, dim, line) = (rgb(p.bg), rgb(p.surface), rgb(p.text), rgb(p.dim), rgb(p.line));
    let mut v = if is_dark(p) { egui::Visuals::dark() } else { egui::Visuals::light() };
    let r = CornerRadius::same(look.radius());
    let border = look.border();
    v.override_text_color = Some(text);
    v.weak_text_color = Some(dim);
    v.hyperlink_color = text;
    v.panel_fill = bg;
    v.window_fill = bg;
    v.faint_bg_color = surface;
    v.extreme_bg_color = bg;
    v.text_edit_bg_color = Some(bg);
    v.code_bg_color = surface;
    v.warn_fg_color = rgb(p.warn);
    v.error_fg_color = rgb(p.alert);
    v.window_stroke = Stroke::new(1.0, line);
    v.window_shadow = egui::Shadow::NONE;
    v.popup_shadow = egui::Shadow::NONE;
    v.window_corner_radius = r;
    v.menu_corner_radius = r;
    v.selection.bg_fill = rgb(p.accent).gamma_multiply(0.35);
    // also the focus ring of a text field
    v.selection.stroke = Stroke::new(2.0, line);
    let w = &mut v.widgets;
    w.noninteractive.bg_fill = bg;
    w.noninteractive.weak_bg_fill = bg;
    w.noninteractive.bg_stroke = Stroke::new(1.0, line);
    w.noninteractive.fg_stroke = Stroke::new(1.0, text);
    for (wv, stroke) in [
        (&mut w.inactive, Stroke::new(border, line)),
        (&mut w.hovered, Stroke::new(border, text)),
        // also keyboard focus: a visible ring in the theme's line colour, thicker than the border
        (&mut w.active, Stroke::new(border + 1.0, line)),
        (&mut w.open, Stroke::new(border, line)),
    ] {
        wv.bg_fill = surface;
        wv.weak_bg_fill = surface;
        wv.bg_stroke = stroke;
        wv.fg_stroke = Stroke::new(1.0, text);
        wv.corner_radius = r;
        wv.expansion = 0.0;
    }
    ctx.set_visuals(v);
}

fn lum(c: [u8; 3]) -> f64 {
    let ch = |x: u8| {
        let x = f64::from(x) / 255.0;
        if x <= 0.03928 { x / 12.92 } else { ((x + 0.055) / 1.055).powf(2.4) }
    };
    0.2126 * ch(c[0]) + 0.7152 * ch(c[1]) + 0.0722 * ch(c[2])
}

/// WCAG contrast ratio, 1..21.
pub fn contrast(a: [u8; 3], b: [u8; 3]) -> f64 {
    let (la, lb) = (lum(a), lum(b));
    (la.max(lb) + 0.05) / (la.min(lb) + 0.05)
}

fn is_dark(p: &Palette) -> bool {
    lum(p.bg) < 0.2
}

fn font_dir() -> PathBuf {
    std::env::var_os("SystemRoot")
        .or_else(|| std::env::var_os("windir"))
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("C:\\Windows"))
        .join("Fonts")
}

/// The display font's bytes: embedded (made by firmware/tools/fonts/gen_theme_fonts.py --agent) or from Windows.
fn display_font(def: &ThemeDef) -> Option<Vec<u8>> {
    if let Some(file) = def.display_font.strip_prefix("system:") {
        return std::fs::read(font_dir().join(file)).ok();
    }
    let b: &[u8] = match def.display_font {
        "archivo-display.ttf" => include_bytes!("../../assets/fonts/archivo-display.ttf"),
        "chakra-petch-display.ttf" => include_bytes!("../../assets/fonts/chakra-petch-display.ttf"),
        "cormorant-display.ttf" => include_bytes!("../../assets/fonts/cormorant-display.ttf"),
        _ => return None,
    };
    Some(b.to_vec())
}

pub fn display_family() -> FontFamily {
    FontFamily::Name("display".into())
}

/// Segoe UI (full Vietnamese) and Segoe UI Symbol (✓ ● ✕ ♪) in front of egui's fonts; the theme's display font as
/// the "display" family, with Segoe behind it for what it lacks. Nothing found: egui's own fonts.
pub fn fonts(def: &ThemeDef) -> FontDefinitions {
    let mut defs = FontDefinitions::default();
    let dir = font_dir();
    let mut names = Vec::new();
    for (name, file) in [("segoe", "segoeui.ttf"), ("segoe_symbol", "seguisym.ttf")] {
        if let Ok(bytes) = std::fs::read(dir.join(file)) {
            defs.font_data.insert(name.to_owned(), Arc::new(FontData::from_owned(bytes)));
            names.push(name.to_owned());
        }
    }
    let prop = defs.families.entry(FontFamily::Proportional).or_default();
    for (i, n) in names.iter().enumerate() {
        prop.insert(i, n.clone());
    }
    let fallback = prop.clone();
    // monospace keeps egui's font first, with Segoe as a fallback for Vietnamese
    defs.families.entry(FontFamily::Monospace).or_default().extend(names);
    let mut display = Vec::new();
    if let Some(bytes) = display_font(def) {
        defs.font_data.insert("display".to_owned(), Arc::new(FontData::from_owned(bytes)));
        display.push("display".to_owned());
    }
    display.extend(fallback);
    defs.families.insert(display_family(), display);
    defs
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Every text/background pair the window draws, for every theme in light and dark: ≥ 4.5:1 for text (body
    /// size), ≥ 3:1 for borders and the focus ring.
    #[test]
    fn every_theme_is_readable() {
        type Get = fn(&Palette) -> [u8; 3];
        let text: [(&str, Get, Get); 8] = [
            ("text/bg", |p| p.text, |p| p.bg),
            ("text/surface", |p| p.text, |p| p.surface),
            ("dim/bg", |p| p.dim, |p| p.bg),
            ("dim/surface", |p| p.dim, |p| p.surface),
            ("alert/bg", |p| p.alert, |p| p.bg),
            ("alert/surface", |p| p.alert, |p| p.surface),
            ("ok/bg", |p| p.ok, |p| p.bg),
            ("on_accent/accent", |p| p.on_accent, |p| p.accent),
        ];
        let ui: [(&str, Get, Get); 2] =
            [("line/bg", |p| p.line, |p| p.bg), ("line/surface", |p| p.line, |p| p.surface)];
        let mut bad = Vec::new();
        for t in THEMES {
            for (mode, p) in [("light", &t.light), ("dark", &t.dark)] {
                for (pairs, need) in [(&text[..], 4.5), (&ui[..], 3.0)] {
                    for (name, fg, bg) in pairs {
                        let r = contrast(fg(p), bg(p));
                        if r < need {
                            bad.push(format!("{} {mode} {name}: {r:.2} < {need}", t.id));
                        }
                    }
                }
            }
        }
        assert!(bad.is_empty(), "fix the \"agent\" colours in firmware/themes/<id>.json:\n{}", bad.join("\n"));
    }

    #[test]
    fn unknown_theme_falls_back_to_the_default() {
        assert_eq!(find("no-such-theme").id, DEFAULT);
        assert_eq!(find("zen").id, "zen");
    }

    #[test]
    fn display_fonts_are_there() {
        for t in THEMES {
            if !t.display_font.starts_with("system:") {
                let b = display_font(t).unwrap_or_default();
                assert!(b.len() > 10_000 && b.len() <= 150 * 1024, "{}: {} bytes", t.display_font, b.len());
            }
        }
        assert!((contrast([0, 0, 0], [255, 255, 255]) - 21.0).abs() < 0.01);
    }
}
