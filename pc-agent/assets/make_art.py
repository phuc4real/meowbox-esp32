#!/usr/bin/env python3
"""The MeowBox Agent's pictures (docs/enhance_agent.md, B5 and B7). Run from pc-agent/:

    py -3.14 assets/make_art.py            # meowbox.ico + the installer's wizard images
    py -3.14 assets/make_art.py --pixel    # also redraw the tray PNGs in assets/icons/ (overwrites hand edits)

Tray icons, 16/20/24/32/40 px (100-250 % scaling) x 3 states, are pixel art: the cat's head is the favicon's
outline snapped to the pixel grid, and the eyes, nose and status dot are placed by hand per size (the table SIZES).
The state is in the eyes, not only in the dot's colour: open = connected, half closed = connecting, x = error.
The PNGs in assets/icons/ are the sources build.rs embeds; touch them up in any pixel editor and do not run
--pixel again afterwards. 48 px and up (Explorer, the installer, assets/meowbox.ico) use the smooth outline.
"""
import argparse
import pathlib

from PIL import Image, ImageDraw, ImageFont

HERE = pathlib.Path(__file__).resolve().parent
ICONS = HERE / "icons"
PACKAGING = HERE.parent / "packaging"
FONT = HERE / "fonts" / "archivo-display.ttf"

HEAD = [(5.0, 3.0), (12.0, 9.0), (20.0, 9.0), (27.0, 3.0), (27.0, 18.0), (25.5, 24.0), (21.0, 28.0),
        (16.0, 29.0), (11.0, 28.0), (6.5, 24.0), (5.0, 18.0)]
FUR = (30, 30, 30, 255)
LIGHT = (245, 240, 230, 255)
RING = (255, 255, 255, 255)
DOT = {"connected": (46, 160, 67, 255), "connecting": (140, 140, 140, 255), "error": (210, 50, 50, 255)}
# Swiss theme (firmware/themes/swiss.json) for the installer
SW_BG, SW_RED, SW_TEXT = (242, 240, 234), (215, 20, 26), (0, 0, 0)

# Per size, by hand: eye centres x (left, right) and y, eye width and height when open, the x's arm length, nose
# (x, y, width), dot radius. Odd widths get a centre pixel; everything is in whole pixels of that size.
SIZES = {
    16: dict(eyes=(6, 11), ey=8, ew=2, eh=3, x=1, nose=(7, 11, 2), dot=3),
    20: dict(eyes=(8, 13), ey=10, ew=2, eh=3, x=1, nose=(9, 13, 2), dot=4),
    24: dict(eyes=(9, 15), ey=12, ew=3, eh=4, x=2, nose=(11, 16, 2), dot=4),
    32: dict(eyes=(12, 21), ey=16, ew=4, eh=5, x=2, nose=(15, 21, 3), dot=6),
    40: dict(eyes=(15, 26), ey=21, ew=4, eh=6, x=3, nose=(19, 27, 3), dot=7),
}


def head_mask(size, ss=8):
    """The outline at `size`, a pixel in when at least half of it is covered (supersampled)."""
    big = Image.new("L", (size * ss, size * ss), 0)
    k = size * ss / 32
    ImageDraw.Draw(big).polygon([(x * k, y * k) for x, y in HEAD], fill=255)
    return big.resize((size, size), Image.BOX).point(lambda v: 255 if v >= 128 else 0)


def pixel_icon(size, state):
    s = SIZES[size]
    im = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    im.paste(FUR, mask=head_mask(size))
    px = im.load()

    def put(x, y, c=LIGHT):
        if 0 <= x < size and 0 <= y < size:
            px[x, y] = c

    for cx in s["eyes"]:
        x0 = cx - s["ew"] // 2
        y0 = s["ey"] - s["eh"] // 2
        if state == "connected":
            for y in range(s["eh"]):
                for x in range(s["ew"]):
                    put(x0 + x, y0 + y)
        elif state == "connecting":
            # half closed: the lower part of the eye, under a flat lid
            for y in range((s["eh"] + 1) // 2, s["eh"]):
                for x in range(s["ew"]):
                    put(x0 + x, y0 + y)
        else:
            # x eyes: two diagonals through the eye's centre pixel
            a, xc = s["x"], x0 + (s["ew"] - 1) // 2
            for d in range(-a, a + 1):
                put(xc + d, s["ey"] + d)
                put(xc - d, s["ey"] + d)
    nx, ny, nw = s["nose"]
    for x in range(nw):
        put(nx + x, ny)
    if nw >= 3:
        put(nx + nw // 2, ny + 1)
    # the status dot in the corner, a white ring around it
    r = s["dot"]
    c = size - r - 1
    for y in range(size):
        for x in range(size):
            d = ((x - c) ** 2 + (y - c) ** 2) ** 0.5
            if d <= r - 0.5:
                px[x, y] = DOT[state]
            elif d <= r + 0.6:
                px[x, y] = RING
    return im


def smooth_cat(size, eyes=True):
    """The vector outline (anti-aliased), open eyes, no dot: Explorer, the installer, the window."""
    ss = 4
    big = Image.new("RGBA", (size * ss, size * ss), (0, 0, 0, 0))
    k = size * ss / 32
    d = ImageDraw.Draw(big)
    d.polygon([(x * k, y * k) for x, y in HEAD], fill=FUR)
    if eyes:
        for cx in (11.5, 20.5):
            d.ellipse([(cx - 1.8) * k, (17 - 2.2) * k, (cx + 1.8) * k, (17 + 2.2) * k], fill=LIGHT)
        d.polygon([(14.6 * k, 21.2 * k), (17.4 * k, 21.2 * k), (16 * k, 22.8 * k)], fill=LIGHT)
    return big.resize((size, size), Image.LANCZOS)


def make_ico():
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    # the exe's own icon has no status dot
    images = [pixel_nodot(n) if n in SIZES else smooth_cat(n) for n in sizes]
    images[-1].save(HERE / "meowbox.ico", sizes=[(n, n) for n in sizes], append_images=images[:-1])


def pixel_nodot(size):
    s = SIZES[size]
    im = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    im.paste(FUR, mask=head_mask(size))
    tmp = pixel_icon(size, "connected")
    # copy the eyes and nose, leave the corner as the bare head
    r = s["dot"]
    c = size - r - 1
    for y in range(size):
        for x in range(size):
            if ((x - c) ** 2 + (y - c) ** 2) ** 0.5 > r + 0.6 and tmp.getpixel((x, y)) == LIGHT:
                im.putpixel((x, y), LIGHT)
    return im


def wizard_images():
    """Inno's large side image (164x314) and small top-right one (55x58) in Swiss colours, at 100/150/200 %."""
    font_ok = FONT.exists()
    for scale, (w, h), (sw, sh) in [(1, (164, 314), (55, 58)), (1.5, (246, 471), (83, 87)), (2, (328, 628), (110, 116))]:
        im = Image.new("RGB", (w, h), SW_BG)
        d = ImageDraw.Draw(im)
        d.rectangle([0, 0, w, int(h * 0.42)], fill=SW_RED)
        cat = smooth_cat(int(w * 0.78))
        im.paste(cat, ((w - cat.width) // 2, int(h * 0.42) - cat.height // 2), cat)
        if font_ok:
            f = ImageFont.truetype(str(FONT), int(30 * scale))
            d.text((int(14 * scale), int(h * 0.80)), "MeowBox", font=f, fill=SW_TEXT)
            f2 = ImageFont.truetype(str(FONT), int(15 * scale))
            d.text((int(15 * scale), int(h * 0.80 + 34 * scale)), "Agent", font=f2, fill=SW_RED)
        tag = f"{int(scale * 100)}"
        im.save(PACKAGING / f"wizard-{tag}.bmp")
        small = Image.new("RGB", (sw, sh), SW_BG)
        c = smooth_cat(int(min(sw, sh) * 0.92))
        small.paste(c, ((sw - c.width) // 2, (sh - c.height) // 2), c)
        small.save(PACKAGING / f"wizard-small-{tag}.bmp")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pixel", action="store_true", help="redraw assets/icons/*.png (overwrites hand edits)")
    a = ap.parse_args()
    if a.pixel:
        ICONS.mkdir(exist_ok=True)
        for n in SIZES:
            for st in DOT:
                pixel_icon(n, st).save(ICONS / f"cat-{st}-{n}.png")
        print(f"assets/icons: {len(SIZES) * len(DOT)} PNGs")
    make_ico()
    wizard_images()
    print("assets/meowbox.ico, packaging/wizard-*.bmp")


if __name__ == "__main__":
    main()
