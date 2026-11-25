"""Screenshot every theme x tile from a running speaker and mark what the case hides.

  python tools/ui_shots.py --host 192.168.1.50 [--night] [--themes swiss,zen] [--out shots/]

Uses GET /api/screenshot?tile=N (firmware/web/API.md). Writes one PNG per theme/tile plus sheet.png (themes as rows,
tiles as columns). The hidden area follows the visible-area model in docs/pinmap.md: the glass shows the whole 296x240
panel except ~3 px on the left and four rounded corners of radius ~60 px. Hidden pixels are tinted red, so anything
drawn there is cut off on the real device. The theme and night mode are restored afterwards.
"""
import argparse
import http.cookiejar
import io
import json
import math
import time
import urllib.request
from pathlib import Path

from PIL import Image, ImageDraw

W, H = 296, 240
CORNER_R, HIDE_L = 60, 3  # keep in sync with UI_CORNER_R / UI_HIDE_L in firmware/components/ui/include/ui.h
TILES = ["clock", "radio", "pc", "cat", "system"]


def visible(x: int, y: int) -> bool:
    if x < HIDE_L:
        return False
    cx = min(max(x, CORNER_R), W - 1 - CORNER_R)
    cy = min(max(y, CORNER_R), H - 1 - CORNER_R)
    return math.hypot(x - cx, y - cy) <= CORNER_R


def mask() -> Image.Image:
    m = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    px = m.load()
    for y in range(H):
        for x in range(W):
            if not visible(x, y):
                px[x, y] = (255, 0, 0, 110)
    return m


class Speaker:
    def __init__(self, host: str, user: str, pw: str):
        self.base = f"http://{host}"
        self.op = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))
        self.req("POST", "/api/login", {"user": user, "pass": pw})

    def req(self, method: str, path: str, body=None, raw=False):
        data = json.dumps(body).encode() if body is not None else None
        r = urllib.request.Request(self.base + path, data, {"Content-Type": "application/json"}, method=method)
        out = self.op.open(r, timeout=20).read()
        return out if raw else json.loads(out)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--user", default="meowbox")
    ap.add_argument("--password", default="m3owb0x2026")
    ap.add_argument("--themes", help="comma-separated ids (default: all)")
    ap.add_argument("--tiles", default="0,1,2,3,4")
    ap.add_argument("--night", action="store_true", help="force night mode while shooting")
    ap.add_argument("--scale", type=int, default=2)
    ap.add_argument("--out", type=Path, default=Path("shots"))
    a = ap.parse_args()

    sp = Speaker(a.host, a.user, a.password)
    disp = sp.req("GET", "/api/config/display")
    themes = a.themes.split(",") if a.themes else [t["id"] for t in sp.req("GET", "/api/themes")["themes"]]
    tiles = [int(t) for t in a.tiles.split(",")]
    a.out.mkdir(parents=True, exist_ok=True)
    m = mask()
    rows = []
    try:
        if a.night:
            sp.req("PUT", "/api/config/display", {"night_mode": "on"})
        for th in themes:
            sp.req("PUT", "/api/config/display", {"theme": th})
            time.sleep(2.5)  # rebuild happens in the UI task
            row = []
            for t in tiles:
                bmp = sp.req("GET", f"/api/screenshot?tile={t}", raw=True)
                img = Image.open(io.BytesIO(bmp)).convert("RGBA")
                img = Image.alpha_composite(img, m).resize((W * a.scale, H * a.scale), Image.NEAREST)
                name = f"{th}{'-night' if a.night else ''}-{t}-{TILES[t]}.png"
                img.save(a.out / name)
                row.append(img)
                print(name, flush=True)
            rows.append((th, row))
    finally:
        sp.req("PUT", "/api/config/display", {"theme": disp["theme"], "night_mode": disp["night_mode"]})

    gap, label = 8, 20
    cw, ch = W * a.scale, H * a.scale
    sheet = Image.new("RGB", (len(tiles) * (cw + gap) + gap, len(rows) * (ch + gap + label) + gap), (40, 40, 40))
    d = ImageDraw.Draw(sheet)
    for r, (th, row) in enumerate(rows):
        y = gap + r * (ch + gap + label)
        for c, img in enumerate(row):
            x = gap + c * (cw + gap)
            d.text((x, y + 4), f"{th} / {TILES[tiles[c]]}", fill=(230, 230, 230))
            sheet.paste(img, (x, y + label))
    sheet.save(a.out / f"sheet{'-night' if a.night else ''}.png")
    print(a.out / "sheet.png")


if __name__ == "__main__":
    main()
