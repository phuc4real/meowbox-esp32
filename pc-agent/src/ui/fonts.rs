//! Segoe UI (the Windows UI font, full Vietnamese) and Segoe UI Symbol (✓ ● ✕ ›) from the Windows font folder, in
//! front of egui's built-in fonts. Nothing is bundled; a missing file just leaves egui's fonts.

use eframe::egui::{FontData, FontDefinitions, FontFamily};
use std::path::PathBuf;
use std::sync::Arc;

fn font_dir() -> PathBuf {
    std::env::var_os("SystemRoot")
        .or_else(|| std::env::var_os("windir"))
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("C:\\Windows"))
        .join("Fonts")
}

pub fn definitions() -> FontDefinitions {
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
    // monospace keeps egui's font first, with Segoe as a fallback for Vietnamese
    let mono = defs.families.entry(FontFamily::Monospace).or_default();
    mono.extend(names);
    defs
}
