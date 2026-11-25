#!/usr/bin/env python3
"""Drive a running MeowBox speaker (or firmware/web/mock_server.py) over its REST API. Stdlib only; Pillow optional
(PNG screenshots, hidden-area mask). Run with Python 3.11+, e.g. `py -3.14` on the dev PC.

  python driver.py --host <ip>[:port] <command> [args]        (or set MEOWBOX_HOST)

Commands (each prints JSON or one line per result, exit code 0 = ok):
  wait-up [--timeout S]           wait until /api/ping answers and login works (after flash / reboot)
  status                          short status: version, mode, rssi, pc, play state, sleep timer
  shot [--tile N] [--out F] [--mask]   screenshot -> PNG (or .bmp without Pillow); --tile SWITCHES the visible tile
  api METHOD PATH [JSON]          any endpoint, e.g. api PUT /api/config/display '{"theme":"zen"}'
  radio play [ID] | stop | next | prev | vol N
  notify TEXT [--title T]         text notification (card on screen + chime)
  tone [--secs S] [--hz F]        audio notification: a generated 16-bit WAV beep (same path as `meowbox say`)
  theme ID | night on|off|auto    display settings (a change rebuilds every tile, ~150-190 ms)
  mem                             internal/DMA/PSRAM free + lowest, every task's lowest free stack
  ui-alive [--secs S]             did the UI task (taskLVGL) get CPU in the window? exit 1 if not (screen frozen)
  cpu [--secs S]                  per-task CPU share over a window (one core = 100 %)
  serial [--port COM10] [--secs S] [--grep RE]   speaker log over USB (pyserial; opening the port may reboot it)

The session cookie is cached in %TEMP%/meowbox-driver-<host>.cookie and reused: the speaker keeps only 4 sessions
and drops the oldest, so logging in on every call would log the web page out.
"""
import argparse
import http.cookiejar
import io
import json
import math
import os
import re
import struct
import sys
import tempfile
import time
import urllib.error
import urllib.request
import wave

W, H = 296, 240
CORNER_R, HIDE_L = 60, 3  # visible-area model, same as firmware/components/ui/include/ui.h and tools/ui_shots.py


class Speaker:
    def __init__(self, host, user, pw):
        self.base = f"http://{host}"
        self.user, self.pw = user, pw
        self.jar_path = os.path.join(tempfile.gettempdir(), "meowbox-driver-" + re.sub(r"[^\w.-]", "_", host) + ".cookie")
        self.jar = http.cookiejar.LWPCookieJar(self.jar_path)
        try:
            self.jar.load(ignore_discard=True)
        except (OSError, http.cookiejar.LoadError):
            pass
        self.op = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(self.jar))

    def login(self):
        self._raw("POST", "/api/login", json.dumps({"user": self.user, "pass": self.pw}).encode(), "application/json")
        self.jar.save(ignore_discard=True)

    def _raw(self, method, path, data=None, ctype="application/json", timeout=30):
        req = urllib.request.Request(self.base + path, data, {"Content-Type": ctype}, method=method)
        with self.op.open(req, timeout=timeout) as r:
            return r.read()

    def call(self, method, path, body=None, raw=None, ctype="application/json", timeout=30):
        data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
        for attempt in (0, 1):
            try:
                out = self._raw(method, path, data, ctype, timeout)
                break
            except urllib.error.HTTPError as e:
                if e.code == 401 and attempt == 0:
                    self.login()
                    continue
                raise SystemExit(f"{method} {path} -> HTTP {e.code} {e.read()[:200]!r}") from None
        return json.loads(out) if out[:1] in (b"{", b"[") else out


def out(obj):
    print(json.dumps(obj, ensure_ascii=False, indent=1) if isinstance(obj, (dict, list)) else obj)


def visible(x, y):
    if x < HIDE_L:
        return False
    cx = min(max(x, CORNER_R), W - 1 - CORNER_R)
    cy = min(max(y, CORNER_R), H - 1 - CORNER_R)
    return math.hypot(x - cx, y - cy) <= CORNER_R


def cmd_wait_up(sp, a):
    end = time.time() + a.timeout
    while time.time() < end:
        try:
            sp.login()  # /api/ping exists on the speaker only, not in mock_server.py
            st = sp.call("GET", "/api/status")
            out(f"up: {st.get('ver')} mode={st.get('mode')}")
            return 0
        except Exception:
            time.sleep(2)
    out("not reachable")
    return 1


def cmd_status(sp, a):
    s = sp.call("GET", "/api/status")
    p = s.get("play") or {}
    out({k: s.get(k) for k in ("ver", "build", "mode", "rssi", "pc", "vol", "sleep", "heap_min")} |
        {"play": {k: p.get(k) for k in ("st", "id", "name", "title")}})
    return 0


def cmd_shot(sp, a):
    q = f"?tile={a.tile}" if a.tile is not None else ""
    bmp = sp.call("GET", "/api/screenshot" + q, timeout=40)
    try:
        from PIL import Image
    except ImportError:
        path = a.out if a.out.endswith(".bmp") else os.path.splitext(a.out)[0] + ".bmp"
        open(path, "wb").write(bmp)
        out(path)
        return 0
    img = Image.open(io.BytesIO(bmp)).convert("RGBA")
    if a.mask:  # tint what the case hides
        m = Image.new("RGBA", img.size, (0, 0, 0, 0))
        px = m.load()
        for y in range(img.height):
            for x in range(img.width):
                if not visible(x, y):
                    px[x, y] = (255, 0, 0, 110)
        img = Image.alpha_composite(img, m)
    img.resize((img.width * 2, img.height * 2), Image.NEAREST).save(a.out)
    out(os.path.abspath(a.out))
    return 0


def cmd_api(sp, a):
    path = a.path
    m = re.match(r"^[A-Za-z]:/.*?/Git(/.*)$", path)  # Git Bash turned "/api/x" into "C:/Program Files/Git/api/x"
    if m:
        path = m.group(1)
    if not path.startswith("/"):
        path = "/" + path
    r = sp.call(a.method.upper(), path, json.loads(a.json) if a.json else ({} if a.method.upper() == "POST" else None))
    out(r if isinstance(r, (dict, list)) else f"{len(r)} bytes")
    return 0


def cmd_radio(sp, a):
    body = {"cmd": a.action}
    if a.action == "play" and a.arg is not None:
        body["id"] = int(a.arg)
    if a.action == "vol":
        body["vol"] = int(a.arg)
    r = sp.call("POST", "/api/radio/play", body)
    out({"vol": r.get("vol"), "play": r.get("play")})
    return 0


def cmd_notify(sp, a):
    out(sp.call("POST", "/api/notify", {"title": a.title, "text": a.text, "duration": a.duration}))
    return 0


def cmd_tone(sp, a):
    rate = 16000
    b = io.BytesIO()
    with wave.open(b, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(struct.pack("<h", int(6000 * math.sin(2 * math.pi * a.hz * i / rate)))
                               for i in range(int(rate * a.secs))))
    out(sp.call("POST", "/api/notify/audio?title=tone", raw=b.getvalue(), ctype="audio/wav"))
    return 0


def cmd_display(sp, a, key, val):
    r = sp.call("PUT", "/api/config/display", {key: val})
    out({k: r.get(k) for k in ("theme", "night_mode", "sleep_s")})
    return 0


def cmd_mem(sp, a):
    d = debug(sp)
    out({"up_s": d["up_ms"] // 1000, "int": d["int"], "dma": d["dma"], "psram": d["psram"],
         "stack_free_lowest": {t["name"]: t["stack_free"] for t in sorted(d["tasks"], key=lambda t: t["stack_free"])}})
    return 0


def debug(sp):
    try:
        return sp.call("GET", "/api/debug")
    except SystemExit as e:
        if "404" in str(e):
            raise SystemExit("/api/debug is speaker-only (mock_server.py has no tasks or heap)") from None
        raise


def window(sp, secs):
    a = debug(sp)
    time.sleep(secs)
    b = debug(sp)
    ra = {t["name"]: t["rt"] for t in a["tasks"]}
    dt = b["rt_total"] - a["rt_total"]
    return {t["name"]: 100.0 * (t["rt"] - ra.get(t["name"], t["rt"])) / dt for t in b["tasks"]} if dt > 0 else {}


def cmd_cpu(sp, a):
    c = window(sp, a.secs)
    out({k: round(v, 1) for k, v in sorted(c.items(), key=lambda x: -x[1]) if v >= 0.1})
    return 0


def cmd_ui_alive(sp, a):
    c = window(sp, a.secs)
    lv, dec = c.get("taskLVGL", 0), c.get("decode", 0)
    ok = lv > 0
    out(f"{'ok' if ok else 'FROZEN'}: taskLVGL {lv:.1f}% decode {dec:.1f}% IDLE1 {c.get('IDLE1', 0):.1f}% over {a.secs}s")
    return 0 if ok else 1


def cmd_serial(sp, a):
    try:
        import serial
    except ImportError:
        raise SystemExit("pyserial missing: use the ESP-IDF venv python "
                         r"(D:\esp\.espressif\python_env\idf6.1_py3.14_env\Scripts\python.exe)") from None
    s = serial.Serial(a.port, 115200, timeout=0.2)
    s.dtr = False
    s.rts = False
    rx = re.compile(a.grep) if a.grep else None
    end, buf = time.time() + a.secs, b""
    while time.time() < end:
        buf += s.read(4096)
        *lines, buf = buf.split(b"\n")
        for ln in lines:
            t = ln.decode("utf-8", "replace").rstrip()
            if not rx or rx.search(t):
                print(t, flush=True)
    return 0


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")  # Vietnamese text on a cp1252 console
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=os.environ.get("MEOWBOX_HOST"))
    ap.add_argument("--user", default="meowbox")
    ap.add_argument("--password", default=os.environ.get("MEOWBOX_PASS", "m3owb0x2026"))
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("wait-up"); p.add_argument("--timeout", type=float, default=90)
    sub.add_parser("status")
    p = sub.add_parser("shot"); p.add_argument("--tile", type=int); p.add_argument("--out", default="meowbox-shot.png")
    p.add_argument("--mask", action="store_true")
    p = sub.add_parser("api"); p.add_argument("method"); p.add_argument("path"); p.add_argument("json", nargs="?")
    p = sub.add_parser("radio"); p.add_argument("action", choices=["play", "stop", "next", "prev", "vol"])
    p.add_argument("arg", nargs="?")
    p = sub.add_parser("notify"); p.add_argument("text"); p.add_argument("--title", default="")
    p.add_argument("--duration", type=int, default=5)
    p = sub.add_parser("tone"); p.add_argument("--secs", type=float, default=1); p.add_argument("--hz", type=float, default=660)
    p = sub.add_parser("theme"); p.add_argument("id")
    p = sub.add_parser("night"); p.add_argument("mode", choices=["on", "off", "auto"])
    sub.add_parser("mem")
    p = sub.add_parser("cpu"); p.add_argument("--secs", type=float, default=5)
    p = sub.add_parser("ui-alive"); p.add_argument("--secs", type=float, default=5)
    p = sub.add_parser("serial"); p.add_argument("--port", default="COM10"); p.add_argument("--secs", type=float, default=30)
    p.add_argument("--grep")
    a = ap.parse_args()
    if a.cmd != "serial" and not a.host:
        raise SystemExit("--host <speaker ip> (or MEOWBOX_HOST) is required")
    sp = Speaker(a.host, a.user, a.password) if a.host else None
    fn = {"wait-up": cmd_wait_up, "status": cmd_status, "shot": cmd_shot, "api": cmd_api, "radio": cmd_radio,
          "notify": cmd_notify, "tone": cmd_tone, "mem": cmd_mem, "cpu": cmd_cpu, "ui-alive": cmd_ui_alive,
          "serial": cmd_serial,
          "theme": lambda s, x: cmd_display(s, x, "theme", x.id),
          "night": lambda s, x: cmd_display(s, x, "night_mode", x.mode)}[a.cmd]
    return fn(sp, a)


if __name__ == "__main__":
    sys.exit(main())
