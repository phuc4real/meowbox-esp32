//! The MeowBox cat (B5). The tray shows the link state in the cat's eyes (open = connected, half closed =
//! connecting, x = error) with a coloured dot as a second cue: hand-drawn pixel icons for 16-40 px
//! (assets/icons, embedded by build.rs), the vector outline (same as the web favicon) for anything larger and
//! for the window icon.

use tiny_skia::{Color, FillRule, Paint, PathBuilder, Pixmap, Stroke, Transform};

include!(concat!(env!("OUT_DIR"), "/tray_icons.rs"));

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum State {
    Connected,
    Connecting,
    Error,
}

impl State {
    fn name(self) -> &'static str {
        match self {
            Self::Connected => "connected",
            Self::Connecting => "connecting",
            Self::Error => "error",
        }
    }
    pub fn dot(self) -> (u8, u8, u8) {
        match self {
            Self::Connected => (46, 160, 67),
            Self::Connecting => (140, 140, 140),
            Self::Error => (210, 50, 50),
        }
    }
}

/// Straight RGBA for the tray at `size` px: the hand-drawn icon of that size, else the vector one.
pub fn tray_rgba(state: State, size: u32) -> Vec<u8> {
    TRAY_ICONS
        .iter()
        .find(|(st, n, _)| *st == state.name() && *n == size)
        .map(|(_, _, px)| px.to_vec())
        .unwrap_or_else(|| cat_rgba(size, Some(state)))
}

const HEAD: [(f32, f32); 11] = [
    (5.0, 3.0),
    (12.0, 9.0),
    (20.0, 9.0),
    (27.0, 3.0),
    (27.0, 18.0),
    (25.5, 24.0),
    (21.0, 28.0),
    (16.0, 29.0),
    (11.0, 28.0),
    (6.5, 24.0),
    (5.0, 18.0),
];

fn paint(r: u8, g: u8, b: u8) -> Paint<'static> {
    let mut p = Paint::default();
    p.set_color(Color::from_rgba8(r, g, b, 255));
    p.anti_alias = true;
    p
}

/// Straight (not premultiplied) RGBA, `size`×`size`, drawn on the 32-unit grid of the favicon. `state` None: open
/// eyes and no dot (the window icon).
pub fn cat_rgba(size: u32, state: Option<State>) -> Vec<u8> {
    let mut pm = Pixmap::new(size, size).expect("icon size > 0");
    let s = size as f32 / 32.0;
    let id = Transform::identity();
    let mut pb = PathBuilder::new();
    pb.move_to(HEAD[0].0 * s, HEAD[0].1 * s);
    for &(x, y) in &HEAD[1..] {
        pb.line_to(x * s, y * s);
    }
    pb.close();
    pm.fill_path(&pb.finish().expect("head path"), &paint(30, 30, 30), FillRule::Winding, id, None);

    let light = paint(245, 240, 230);
    for cx in [11.5f32, 20.5] {
        match state.unwrap_or(State::Connected) {
            State::Connected | State::Connecting => {
                // half closed: only the lower half of the eye
                let top = if state == Some(State::Connecting) { 17.0 } else { 17.0 - 2.2 };
                let r = tiny_skia::Rect::from_ltrb((cx - 1.8) * s, top * s, (cx + 1.8) * s, (17.0 + 2.2) * s);
                if let Some(eye) = r.and_then(PathBuilder::from_oval) {
                    pm.fill_path(&eye, &light, FillRule::Winding, id, None);
                }
            }
            State::Error => {
                let mut x = PathBuilder::new();
                x.move_to((cx - 1.8) * s, 15.2 * s);
                x.line_to((cx + 1.8) * s, 18.8 * s);
                x.move_to((cx + 1.8) * s, 15.2 * s);
                x.line_to((cx - 1.8) * s, 18.8 * s);
                let stroke = Stroke { width: 1.2 * s, ..Stroke::default() };
                pm.stroke_path(&x.finish().expect("x eye"), &light, &stroke, id, None);
            }
        }
    }
    let mut nose = PathBuilder::new();
    nose.move_to(14.6 * s, 21.2 * s);
    nose.line_to(17.4 * s, 21.2 * s);
    nose.line_to(16.0 * s, 22.8 * s);
    nose.close();
    pm.fill_path(&nose.finish().expect("nose"), &light, FillRule::Winding, id, None);

    if let Some((r, g, b)) = state.map(State::dot) {
        let rad = 6.5 * s;
        let c = size as f32 - rad - 0.5;
        if let Some(circle) = PathBuilder::from_circle(c, c, rad) {
            pm.fill_path(&circle, &paint(r, g, b), FillRule::Winding, id, None);
            let stroke = Stroke { width: (size as f32 / 32.0).max(1.0), ..Stroke::default() };
            pm.stroke_path(&circle, &paint(255, 255, 255), &stroke, id, None);
        }
    }
    // tiny-skia stores premultiplied RGBA; icons (Win32 and egui) want straight alpha
    pm.pixels()
        .iter()
        .flat_map(|p| {
            let c = p.demultiply();
            [c.red(), c.green(), c.blue(), c.alpha()]
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_tray_size_is_hand_drawn_for_every_state() {
        for size in [16, 20, 24, 32, 40] {
            for st in [State::Connected, State::Connecting, State::Error] {
                let px = tray_rgba(st, size);
                assert_eq!(px.len(), (size * size * 4) as usize);
                let found = TRAY_ICONS.iter().any(|(n, s, _)| *n == st.name() && *s == size);
                assert!(found, "assets/icons/cat-{}-{size}.png is missing", st.name());
                // the dot's colour is in the bottom-right quarter
                let (r, g, b) = st.dot();
                let has_dot = px.chunks(4).any(|p| p == [r, g, b, 255]);
                assert!(has_dot, "{size} {st:?}: no status dot");
            }
        }
    }

    #[test]
    fn the_eyes_differ_by_state() {
        let eyes = |st| tray_rgba(st, 32).chunks(4).filter(|p| *p == [245, 240, 230, 255]).count();
        let (open, half, x) = (eyes(State::Connected), eyes(State::Connecting), eyes(State::Error));
        assert!(open > half && half > 0 && x > 0 && x != open, "open {open}, half {half}, x {x}");
    }

    #[test]
    fn vector_icon_for_large_sizes() {
        let px = tray_rgba(State::Connected, 48);
        assert_eq!(px.len(), 48 * 48 * 4);
        let at = |x: usize, y: usize| &px[(y * 48 + x) * 4..(y * 48 + x) * 4 + 4];
        assert_eq!(at(0, 0)[3], 0, "corner is transparent");
        assert_eq!(&at(24, 18)[..3], &[30, 30, 30], "head is dark");
    }
}
