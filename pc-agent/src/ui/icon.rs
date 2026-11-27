//! The MeowBox cat (same outline as the web favicon and the Python tray) with an optional status dot.

use tiny_skia::{Color, FillRule, Paint, PathBuilder, Pixmap, Stroke, Transform};

pub const GREEN: (u8, u8, u8) = (46, 160, 67);
pub const GREY: (u8, u8, u8) = (140, 140, 140);
pub const RED: (u8, u8, u8) = (210, 50, 50);

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

/// Straight (not premultiplied) RGBA, `size`×`size`, drawn on the 32-unit grid of the favicon.
pub fn cat_rgba(size: u32, dot: Option<(u8, u8, u8)>) -> Vec<u8> {
    let mut pm = Pixmap::new(size, size).expect("icon size > 0");
    let s = size as f32 / 32.0;
    let mut pb = PathBuilder::new();
    pb.move_to(HEAD[0].0 * s, HEAD[0].1 * s);
    for &(x, y) in &HEAD[1..] {
        pb.line_to(x * s, y * s);
    }
    pb.close();
    let head = pb.finish().expect("head path");
    pm.fill_path(&head, &paint(30, 30, 30), FillRule::Winding, Transform::identity(), None);

    let light = paint(245, 240, 230);
    for cx in [11.5f32, 20.5] {
        if let Some(eye) = PathBuilder::from_oval(
            tiny_skia::Rect::from_ltrb((cx - 1.8) * s, (17.0 - 2.2) * s, (cx + 1.8) * s, (17.0 + 2.2) * s).unwrap(),
        ) {
            pm.fill_path(&eye, &light, FillRule::Winding, Transform::identity(), None);
        }
    }
    let mut nose = PathBuilder::new();
    nose.move_to(14.6 * s, 21.2 * s);
    nose.line_to(17.4 * s, 21.2 * s);
    nose.line_to(16.0 * s, 22.8 * s);
    nose.close();
    pm.fill_path(&nose.finish().unwrap(), &light, FillRule::Winding, Transform::identity(), None);

    if let Some((r, g, b)) = dot {
        let rad = 6.5 * s;
        let c = size as f32 - rad - 0.5;
        if let Some(circle) = PathBuilder::from_circle(c, c, rad) {
            pm.fill_path(&circle, &paint(r, g, b), FillRule::Winding, Transform::identity(), None);
            let stroke = Stroke { width: (size as f32 / 32.0).max(1.0), ..Stroke::default() };
            pm.stroke_path(&circle, &paint(255, 255, 255), &stroke, Transform::identity(), None);
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
    fn icon_has_cat_and_dot() {
        let px = cat_rgba(32, Some(GREEN));
        assert_eq!(px.len(), 32 * 32 * 4);
        let at = |x: usize, y: usize| &px[(y * 32 + x) * 4..(y * 32 + x) * 4 + 4];
        assert_eq!(at(0, 0)[3], 0, "corner is transparent");
        assert_eq!(&at(16, 12)[..3], &[30, 30, 30], "head is dark");
        assert_eq!(&at(28, 28)[..3], &[46, 160, 67], "dot is green");
    }
}
