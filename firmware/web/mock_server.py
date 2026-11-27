#!/usr/bin/env python3
"""MeowBox mock device: serves web/src/ and implements API.md with in-memory state.

Usage:  python mock_server.py [--port 8080] [--dir src]
Then open http://localhost:8080  (meowbox / m3owb0x2026)
Stdlib only.
"""
import argparse, copy, hashlib, json, os, random, re, secrets, string, struct, sys, threading, time
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlsplit, parse_qs

HERE = os.path.dirname(os.path.abspath(__file__))
STORAGE = os.path.join(HERE, "assets")  # /fonts/*, /themes/*.png come from here
PACK_FILE = os.path.normpath(os.path.join(HERE, "..", "components", "webui", "web.pack"))
PACK = None  # --dist: {path: (data, gzip, immutable, etag)} read from web.pack, served like the firmware does
THEMES = json.load(open(os.path.join(HERE, "themes.json"), encoding="utf-8"))  # written by tools/gen_themes.py
THEME_IDS = [t["id"] for t in THEMES]
DEFAULT_THEME = next((t["id"] for t in THEMES if t.get("default")), "swiss")
BOOT = time.time()
MAX_FW = 5505024
DEFAULT_PW = "m3owb0x2026"
DEFAULT_USER = "meowbox"
LOCK = threading.RLock()


def tok():
    return "".join(random.choice(string.ascii_letters + string.digits) for _ in range(16))


def pw_hash(salt, pw):
    return hashlib.sha256((salt + pw).encode()).hexdigest()


def fresh_state():
    salt = secrets.token_hex(8)
    return {
        "salt": salt, "pw": pw_hash(salt, DEFAULT_PW), "def_pw": True,
        "wifi": {"cur": "NhaMeo", "max": 5, "nets": [{"ssid": "NhaMeo", "pass": "12345678"}]},
        "radio": {"vol": 40, "last": 1, "max": 50, "st": [
            {"id": 1, "name": "Radio Paradise", "url": "http://stream.radioparadise.com/mp3-128", "type": "stream"},
            {"id": 2, "name": "VOV3", "url": "https://str.vov.gov.vn/vovlive/vov3.sdp_aac/playlist.m3u8", "type": "hls"},
            {"id": 3, "name": "Lofi Girl", "url": "https://www.youtube.com/watch?v=jfKfPfyJRdk", "type": "youtube"},
        ]},
        "alarms": {"snooze": 5, "max": 5, "al": [
            {"on": True, "h": 6, "m": 30, "days": 31, "snd": "chime", "st": 0}]},
        "weather": {"on": True, "name": "Hồ Chí Minh", "lat": 10.823, "lon": 106.630, "unit": "c",
                    "now": {"t": 24.5, "code": 3, "hi": 28, "lo": 20, "at": int(time.time())}},
        "display": {"bri": 80, "idle_bri": 20, "idle_s": 60, "sleep_s": 300, "h24": True, "night": True,
                    "n_from": 22, "n_to": 7, "n_bri": 5, "lang": "vi", "tz": "ICT-7",
                    "theme": DEFAULT_THEME, "night_mode": "auto", "fx": "full"},
        "cat": {"name": "Mướp", "fur": "tabby", "clap": True, "show": True, "snd": True},
        "notify": {"token": tok(), "dur": 10, "vol": 70, "quiet_low": True},
        "pc": {"host": "192.168.1.10", "port": 8765, "auto": True},
        "update": {"auto": False},
        # firmware update simulation (API.md "Update"): the mock "finds" 0.2.1 on the first check
        "upd": {"state": "idle", "pct": 0, "err": "", "checked": 0, "t0": 0, "new": None},
        "play": {"st": "stop", "id": 0, "name": "", "title": ""},
        "sleep_end": 0, "hist": [], "nid": 0,
    }


S = fresh_state()
SESS = {}          # sid -> last use
FAILS: dict[str, dict] = {}  # login lockout per client IP: {"n": failures, "until": locked until, "last": last failure}
NO_AUTH = False  # --no-auth: every request counts as logged in (dev/headless tests)
NO_SHOT = False  # --no-screenshot: /api/screenshot -> 404 (test the web fallback)


class Err(Exception):
    def __init__(self, code, err):
        self.code, self.err = code, err


def need(cond, code=400, err="bad_value"):
    if not cond:
        raise Err(code, err)


def merge(dst, src, keys):
    for k in keys:
        if k in src:
            need(type(src[k]) is type(dst[k]) or (isinstance(dst[k], float) and isinstance(src[k], int)), err="bad_value")
            dst[k] = src[k]


def status():
    left = max(0, int(S["sleep_end"] - time.time())) if S["sleep_end"] else 0
    return {"ver": "0.1.0-mock", "build": "2026-11-22 10:00", "up": int(time.time() - BOOT),
            "mode": "sta", "ssid": S["wifi"]["cur"], "ip": "192.168.1.50", "rssi": random.randint(-70, -50),
            "bat": 78, "chg": True, "usb": True,
            "heap": random.randint(130000, 150000), "heap_min": 98304, "psram": 6815744, "psram_tot": 8388608,
            "time": int(time.time()), "vol": S["radio"]["vol"], "sleep": left, "pc": True, "def_pw": S["def_pw"],
            "play": play_out(), **({"upd": S["upd"]["new"]["ver"]} if S["upd"]["new"] else {})}


def play_out():
    """S['play'] plus the stream numbers the firmware adds while a network stream plays (API.md, play.net)"""
    p = dict(S["play"])
    if p["st"] in ("buf", "play", "err"):
        br = 128
        p["net"] = {"kbps": random.randint(110, 135), "br": br, "buf": round(random.uniform(4, 12), 1), "pre": 6,
                    "fill": 100 if p["st"] == "play" else random.randint(10, 90), "under": 1, "reconn": 0,
                    "fmt": "mp3", "err": ""}
    return p


MOCK_RELEASE = {"ver": "0.2.1", "date": "2026-12-01", "size": 3155424, "proto": 1, "usb_only": False,
                "notes": {"vi": ["Giao diện web nhẹ hơn", "Radio ổn định hơn trên Wi-Fi lạ"],
                          "en": ["Lighter web UI", "Steadier radio on unfamiliar Wi-Fi"]},
                "page": "https://github.com/phuc4real/meowbox-esp32/releases/tag/v0.2.1"}


def upd_out():
    """GET /api/update, advancing the simulated check (2 s) / download (8 s) / verify (1 s)"""
    u, now = S["upd"], time.time()
    if u["state"] == "check" and now - u["t0"] > 2:
        u.update(state="idle", checked=int(now), new=dict(MOCK_RELEASE))
    if u["state"] == "download":
        u["pct"] = min(99, int((now - u["t0"]) / 8 * 100))
        if now - u["t0"] > 8:
            u.update(state="verify", t0=now)
    if u["state"] == "verify" and now - u["t0"] > 1:
        u.update(state="reboot", pct=100)
    r = {"cur": "0.1.0-mock", "state": u["state"], "pct": u["pct"], "err": u["err"], "auto": S["update"]["auto"],
         "checked": u["checked"]}
    if u["new"]:
        r["latest"] = u["new"]["ver"]
        r["new"] = u["new"]["ver"]
        r.update({k: v for k, v in u["new"].items() if k != "ver"})
    return r


def wifi_out():
    w = S["wifi"]
    return {"cur": w["cur"], "max": w["max"], "nets": [{"ssid": n["ssid"]} for n in w["nets"]]}


def cat_out():
    """GET /api/config/cat: settings + the Meow Coding sessions the PC agent reported (st, read-only)."""
    t = int(time.time())
    ss = [
        {"id": "085ba2bd", "ag": "claude", "name": "Meow Coding cho loa", "dir": "meowbox", "br": "main",
         "st": ("work", "wait", "work", "done")[t // 20 % 4], "act": "edit", "for": t % 20,
         "tok": 4_975_388, "ctx": 36, "model": "Opus 5.5"},
        {"id": "5f1c09aa", "ag": "claude", "name": "Fix login redirect", "dir": "shop-web", "br": "fix/login",
         "st": "idle", "act": "", "for": 840, "tok": 812_400, "ctx": 12, "model": "Sonnet 5.5"},
    ]
    lim = {"h5": 23, "h5r": t - t % 3600 + 7200, "d7": 41, "d7r": t - t % 86400 + 3 * 86400}
    return {**S["cat"], "st": {"pc": True, "s": ss, "lim": lim}}


def pc_out():
    return dict(S["pc"], st={"on": True, "name": "DESKTOP-MEO", "ip": "192.168.1.10", "ver": "0.2.0", "proto": 1,
                             "proto_need": 1, "seen": random.randint(0, 2)})


def add_notify(b, src):
    text = str(b.get("text", "")).strip()
    need(text, err="bad_value")
    pri = b.get("priority", "normal")
    snd = b.get("sound", "chime")
    need(pri in ("low", "normal", "high") and snd in ("chime", "alert", "none"))
    S["nid"] += 1
    item = {"id": S["nid"], "ts": int(time.time()), "title": str(b.get("title", ""))[:48], "text": text[:200],
            "sound": snd, "priority": pri, "src": src}
    S["hist"].insert(0, item)
    del S["hist"][20:]
    print(f"  [NOTIFY] {item['title']!r} {item['text']!r} ({pri}, {snd}, {b.get('duration', S['notify']['dur'])}s)")
    return {"ok": True, "id": item["id"]}


MIME = {".html": "text/html; charset=utf-8", ".css": "text/css", ".js": "application/javascript",
        ".svg": "image/svg+xml", ".json": "application/json", ".woff2": "font/woff2", ".png": "image/png",
        ".bmp": "image/bmp"}


def theme_colors(tid):
    try:
        c = json.load(open(os.path.join(HERE, "..", "themes", tid + ".json"), encoding="utf-8"))["color"]
    except Exception:
        c = {}
    rgb = lambda s, d: bytes.fromhex((c.get(s) or d).lstrip("#"))
    return rgb("bg", "#F2F0EA"), rgb("text", "#000000"), rgb("accent", "#D7141A"), rgb("dim", "#5E5E5E")


SEG = {"0": "abcdef", "1": "bc", "2": "abged", "3": "abgcd", "4": "fgbc", "5": "afgcd", "6": "afgedc",
       "7": "abc", "8": "abcdefg", "9": "abcdfg"}


def screenshot_bmp():
    """296x240 24-bit BMP in the current theme colours: 7-segment clock (ICT) + bars, like a clock screen."""
    W, H = 296, 240
    bg, fg, acc, dim = theme_colors(S["display"]["theme"])
    px = bytearray(bg[::-1] * (W * H))  # BGR, top-down

    def rect(x, y, w, h, col):
        row = col[::-1] * w
        for yy in range(max(0, y), min(H, y + h)):
            o = (yy * W + x) * 3
            px[o:o + w * 3] = row

    tm = time.gmtime(time.time() + 7 * 3600)
    x0, y0, dw, dh, th = 24, 56, 46, 84, 9
    for i, ch in enumerate(f"{tm.tm_hour:02d}{tm.tm_min:02d}"):
        x = x0 + i * (dw + 14) + (22 if i > 1 else 0)
        segs = {"a": (x, y0, dw, th), "b": (x + dw - th, y0, th, dh // 2), "c": (x + dw - th, y0 + dh // 2, th, dh // 2),
                "d": (x, y0 + dh - th, dw, th), "e": (x, y0 + dh // 2, th, dh // 2), "f": (x, y0, th, dh // 2),
                "g": (x, y0 + dh // 2 - th // 2, dw, th)}
        for s in SEG[ch]:
            rect(*segs[s], fg)
    if tm.tm_sec % 2 == 0:
        rect(x0 + 2 * (dw + 14) + 2, y0 + 22, 10, 10, acc)
        rect(x0 + 2 * (dw + 14) + 2, y0 + dh - 32, 10, 10, acc)
    rect(24, 168, 180, 8, fg)
    rect(24, 186, 120, 6, acc)
    rect(24, 202, 200, 4, dim)
    rect(24, 20, 40, 4, dim)
    rect(232, 18, 40, 10, fg)
    raw = b"".join(bytes(px[(H - 1 - r) * W * 3:(H - r) * W * 3]) for r in range(H))  # bottom-up rows; 888 B is 4-aligned
    hdr = b"BM" + struct.pack("<IHHI", 54 + len(raw), 0, 0, 54)
    dib = struct.pack("<IiiHHIIiiII", 40, W, H, 1, 24, 0, len(raw), 2835, 2835, 0, 0)
    return hdr + dib + raw


class H(BaseHTTPRequestHandler):
    server_version = "MeowBoxMock/1"
    root = os.path.join(HERE, "src")

    def log_message(self, fmt, *a):
        print("  " + fmt % a)

    # ---- helpers
    def send_json(self, obj, code=200, headers=()):
        data = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        for k, v in headers:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def raw_body(self, limit):
        n = int(self.headers.get("Content-Length") or 0)
        if n > limit:
            self.rfile.read(n)  # drain
            raise Err(413, "too_large")
        return self.rfile.read(n) if n else b""

    def body(self):
        raw = self.raw_body(8192)
        if not raw:
            return {}
        try:
            b = json.loads(raw)
        except ValueError:
            raise Err(400, "bad_json")
        need(isinstance(b, dict), err="bad_json")
        return b

    def sid(self):
        for part in (self.headers.get("Cookie") or "").split(";"):
            k, _, v = part.strip().partition("=")
            if k == "sid":
                return v
        return None

    def has_session(self):
        if NO_AUTH:
            return True
        s = self.sid()
        if s and s in SESS and time.time() - SESS[s] < 86400:
            SESS[s] = time.time()
            return True
        return False

    def need_new_pw(self, m, p):
        """Like the firmware: while the default password is in use, a session may only change it."""
        if S["def_pw"] and not NO_AUTH and (m, p) not in {("GET", "/api/session"), ("POST", "/api/password"),
                                                           ("POST", "/api/logout")}:
            raise Err(403, "def_pw")

    def has_token(self, q):
        a = self.headers.get("Authorization") or ""
        t = a[7:] if a.startswith("Bearer ") else (q.get("token") or [""])[0]
        return t == S["notify"]["token"]

    # ---- static
    def send_bytes(self, data, ctype, cache="no-cache"):
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", cache)
        self.end_headers()
        self.wfile.write(data)

    def static_pack(self, path):
        f = PACK.get(path.lstrip("/") or "index.html")
        if not f or ".." in path:
            return self.send_json({"err": "not_found"}, 404)
        data, gz, imm, etag = f
        p = path.lstrip("/")
        cache = ("public, max-age=31536000, immutable" if imm else
                 "max-age=604800" if p.startswith(("fonts/", "themes/")) else "no-cache")
        if self.headers.get("If-None-Match") == f'"{etag}"':
            self.send_response(304)
            self.send_header("ETag", f'"{etag}"')
            self.send_header("Cache-Control", cache)
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Type", MIME.get(os.path.splitext(p or "index.html")[1].lower(), "application/octet-stream"))
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", cache)
        self.send_header("ETag", f'"{etag}"')
        if gz:
            self.send_header("Content-Encoding", "gzip")
        self.end_headers()
        self.wfile.write(data)

    def static(self, path):
        if PACK is not None:
            return self.static_pack(path)
        if path in ("", "/"):
            path = "/index.html"
        root = self.root
        if path.startswith("/fonts/") or path.startswith("/themes/"):
            root = STORAGE  # built assets: fonts (woff2 + fonts.json) and theme previews (png)
        fp = os.path.normpath(os.path.join(root, path.lstrip("/")))
        if not fp.startswith(os.path.normpath(root) + os.sep) or not os.path.isfile(fp):
            return self.send_json({"err": "not_found"}, 404)
        ctype = MIME.get(os.path.splitext(fp)[1].lower(), "application/octet-stream")
        cache = "max-age=86400" if root == STORAGE and not fp.endswith(".json") else "no-cache"
        self.send_bytes(open(fp, "rb").read(), ctype, cache)

    def do_GET(self): self.handle_any("GET")
    def do_POST(self): self.handle_any("POST")
    def do_PUT(self): self.handle_any("PUT")
    def do_DELETE(self): self.handle_any("DELETE")

    def handle_any(self, method):
        u = urlsplit(self.path)
        path, q = u.path, parse_qs(u.query)
        if not path.startswith("/api/"):
            return self.static(path) if method == "GET" else self.send_json({"err": "not_found"}, 404)
        try:
            with LOCK:
                self.api(method, path, q)
        except Err as e:
            self.send_json({"err": e.err}, e.code)

    # ---- API
    def api(self, m, p, q):
        if p == "/api/login" and m == "POST":
            ip, now = self.client_address[0], time.time()
            f = FAILS.get(ip)
            if f and now >= f["until"] and now - f["last"] > 900:  # 15 min without a failure: forgotten
                del FAILS[ip]
                f = None
            if f and now < f["until"]:
                raise Err(429, "too_many")
            b = self.body()
            pw = str(b.get("pass", ""))
            if b.get("user") == DEFAULT_USER and len(pw) <= 64 and pw_hash(S["salt"], pw) == S["pw"]:
                FAILS.pop(ip, None)
                sid = secrets.token_hex(16)
                SESS[sid] = time.time()
                while len(SESS) > 4:
                    SESS.pop(min(SESS, key=SESS.get))
                return self.send_json({"ok": True, "def_pw": S["def_pw"]},
                                      headers=[("Set-Cookie", f"sid={sid}; Path=/; HttpOnly; SameSite=Strict")])
            f = FAILS.setdefault(ip, {"n": 0, "until": 0, "last": 0})
            f["n"], f["last"] = min(f["n"] + 1, 100), now
            if f["n"] >= 5:  # 30 s, doubling with each further failure, at most 15 min
                f["until"] = now + min(30 << min(f["n"] - 5, 5), 900)
            raise Err(401, "auth")

        if p == "/api/theme" and m == "GET":  # no auth: lets the login page use the theme
            tid = S["display"]["theme"]
            return self.send_json({"theme": tid, "fonts": next((t["fonts"] for t in THEMES if t["id"] == tid), [])})

        if p.startswith("/api/notify"):
            if self.has_token(q):
                return self.notify(m, p, q)
            if not self.has_session():
                raise Err(401, "auth")
            self.need_new_pw(m, p)
            return self.notify(m, p, q)

        if not self.has_session():
            raise Err(401, "auth")
        self.need_new_pw(m, p)

        key = (m, p)
        if key == ("POST", "/api/logout"):
            SESS.pop(self.sid(), None)
            return self.send_json({"ok": True}, headers=[("Set-Cookie", "sid=; Path=/; Max-Age=0")])
        if key == ("GET", "/api/session"):
            return self.send_json({"user": DEFAULT_USER, "def_pw": S["def_pw"]})
        if key == ("POST", "/api/password"):
            b = self.body()
            if pw_hash(S["salt"], str(b.get("old", ""))) != S["pw"]:
                raise Err(403, "bad_old")
            new = str(b.get("new", ""))
            need(6 <= len(new) <= 32)
            S["salt"] = secrets.token_hex(8)
            S["pw"] = pw_hash(S["salt"], new)
            S["def_pw"] = new == DEFAULT_PW
            me = self.sid()
            for s in list(SESS):
                if s != me:
                    SESS.pop(s)
            return self.send_json({"ok": True})
        if key == ("GET", "/api/status"):
            return self.send_json(status())
        if key == ("GET", "/api/themes"):
            return self.send_json({"themes": THEMES, "current": S["display"]["theme"]})
        if key == ("GET", "/api/log"):
            return self.send_json({
                "boot": 7, "reason": "task_wdt", "up_s": int(time.time() - BOOT),
                "prev": {"up_s": 18234, "int_free": 41210, "int_min": 39480},
                "prev_log": ["W (18201345) player: underrun 1, prebuffer now 4 s",
                             "E (18233977) task_wdt: Task watchdog got triggered."],
                "log": ["W (912) lcd_panel.nv3023: The 3Ah command has been used"],
                "boots": [{"n": 7, "reason": "task_wdt", "up_s": 0, "int_min": 0},
                          {"n": 6, "reason": "sw", "up_s": 18234, "int_min": 39480}]})
        if key == ("GET", "/api/screenshot"):
            if NO_SHOT:
                raise Err(404, "not_found")
            return self.send_bytes(screenshot_bmp(), "image/bmp", "no-store")

        # wifi
        if key == ("GET", "/api/config/wifi"):
            return self.send_json(wifi_out())
        if key == ("PUT", "/api/config/wifi"):
            b = self.body()
            nets = b.get("nets")
            need(isinstance(nets, list) and len(nets) <= S["wifi"]["max"])
            old = {n["ssid"]: n.get("pass", "") for n in S["wifi"]["nets"]}
            out = []
            for n in nets:
                ssid = str(n.get("ssid", ""))
                need(0 < len(ssid) <= 32)
                if "pass" in n:
                    out.append({"ssid": ssid, "pass": str(n["pass"])})
                else:
                    need(ssid in old)
                    out.append({"ssid": ssid, "pass": old[ssid]})
            S["wifi"]["nets"] = out
            return self.send_json(wifi_out())
        if key in (("GET", "/api/wifi/portal"), ("POST", "/api/wifi/portal")):
            ap = S.setdefault("portal", {"on": False, "since": 0.0})
            if key[0] == "POST":
                b = self.body()
                need(isinstance(b.get("on"), bool))
                ap["on"], ap["since"] = b["on"], time.time()
            left = max(0, int(600 - (time.time() - ap["since"]))) if ap["on"] else None
            return self.send_json({"on": ap["on"], "ssid": "MeowBox-A1B2", "off_in": left})
        if key == ("GET", "/api/wifi/scan"):
            time.sleep(0.8)
            aps = [{"ssid": "NhaMeo", "rssi": -52, "ch": 6, "sec": True},
                   {"ssid": "VanPhong_5G", "rssi": -67, "ch": 36, "sec": True},
                   {"ssid": "Hàng xóm", "rssi": -74, "ch": 1, "sec": True},
                   {"ssid": "Cafe Free WiFi", "rssi": -83, "ch": 11, "sec": False}]
            return self.send_json({"aps": aps})
        if key == ("POST", "/api/wifi/connect"):
            b = self.body()
            ssid = str(b.get("ssid", ""))
            need(0 < len(ssid) <= 32)
            nets = [n for n in S["wifi"]["nets"] if n["ssid"] != ssid]
            known = next((n for n in S["wifi"]["nets"] if n["ssid"] == ssid), None)
            need("pass" in b or known)
            nets.insert(0, {"ssid": ssid, "pass": str(b.get("pass", known and known["pass"]))})
            S["wifi"]["nets"] = nets[:S["wifi"]["max"]]
            S["wifi"]["cur"] = ssid
            return self.send_json({"ok": True})

        # radio
        r = S["radio"]
        if key == ("GET", "/api/config/radio"):
            return self.send_json(r)
        if key == ("PUT", "/api/config/radio"):
            b = self.body()
            if "st" in b:
                st = b["st"]
                need(isinstance(st, list) and len(st) <= r["max"])
                ids = set()
                clean = []
                for s in st:
                    need(isinstance(s.get("id"), int) and 0 < s["id"] < 65536 and s["id"] not in ids)
                    need(s.get("type") in ("stream", "hls", "youtube"))
                    need(0 < len(str(s.get("name", ""))) <= 48 and 0 < len(str(s.get("url", ""))) <= 256)
                    need(re.match(r"https?://", str(s["url"]), re.I))
                    ids.add(s["id"])
                    clean.append({k: s[k] for k in ("id", "name", "url", "type")})
                r["st"] = clean
                for a in S["alarms"]["al"]:
                    if a["snd"] == "radio" and a["st"] not in ids:
                        a["snd"], a["st"] = "chime", 0
            if "vol" in b:
                need(isinstance(b["vol"], int) and 0 <= b["vol"] <= 100)
                r["vol"] = b["vol"]
            return self.send_json(r)
        if key == ("POST", "/api/radio/play"):
            b = self.body()
            cmd = b.get("cmd")
            pl = S["play"]
            if cmd == "play":
                if b.get("url") and not re.match(r"https?://", str(b["url"]), re.I):
                    raise Err(400, "bad_value")
                if b.get("url"):
                    pl.update(st="play", id=0, name=str(b.get("name") or b["url"])[:48], title="")
                else:
                    sid = b.get("id", r["last"])
                    s = next((x for x in r["st"] if x["id"] == sid), None)
                    if not s:
                        raise Err(404, "not_found")
                    r["last"] = s["id"]
                    pl.update(st="play", id=s["id"], name=s["name"], title="Mock song – Artist")
            elif cmd == "stop":
                pl.update(st="stop", title="")
            elif cmd in ("next", "prev"):
                need(r["st"])
                idx = next((i for i, x in enumerate(r["st"]) if x["id"] == r["last"]), 0)
                s = r["st"][(idx + (1 if cmd == "next" else -1)) % len(r["st"])]
                r["last"] = s["id"]
                pl.update(st="play", id=s["id"], name=s["name"], title="Mock song – Artist")
            elif cmd == "vol":
                v = b.get("vol")
                need(isinstance(v, int) and 0 <= v <= 100)
                r["vol"] = v
            else:
                raise Err(400, "bad_value")
            return self.send_json({"ok": True, "vol": r["vol"], "play": pl})
        if p == "/api/battery" and m == "GET":
            now = int(time.time())
            pts = [[now - 3600 + i * 60, round(90 - i * 0.2, 1), 4050 - i * 2] for i in range(60)]
            return self.send_json({"pct": 78, "raw": 2330, "mv": 3930, "chg": False, "rate": 12.0, "eta": 390,
                                   "cur": {"open": True, "start": now - 3600, "min": 60, "from": 90},
                                   "sessions": [{"start": now - 86400, "min": 412, "from": 100, "to": 6, "mv_from": 4180,
                                                 "mv_to": 3400, "bri": 62, "play_min": 180, "end": "empty",
                                                 "full_h": 7.3}],
                                   "pts": pts})
        if p == "/api/sleep":
            if m == "POST":
                mins = self.body().get("min")
                need(isinstance(mins, int) and 0 <= mins <= 240)
                S["sleep_end"] = time.time() + mins * 60 if mins else 0
            left = max(0, int(S["sleep_end"] - time.time())) if S["sleep_end"] else 0
            return self.send_json({"left": left})

        # alarms
        if key == ("GET", "/api/config/alarms"):
            return self.send_json(S["alarms"])
        if key == ("PUT", "/api/config/alarms"):
            b = self.body()
            al = S["alarms"]
            if "snooze" in b:
                need(isinstance(b["snooze"], int) and 1 <= b["snooze"] <= 30)
                al["snooze"] = b["snooze"]
            if "al" in b:
                need(isinstance(b["al"], list) and len(b["al"]) <= al["max"])
                out = []
                for a in b["al"]:
                    need(0 <= a.get("h", -1) <= 23 and 0 <= a.get("m", -1) <= 59 and 0 <= a.get("days", -1) <= 127)
                    need(a.get("snd") in ("chime", "radio"))
                    out.append({"on": bool(a.get("on")), "h": a["h"], "m": a["m"], "days": a["days"],
                                "snd": a["snd"], "st": int(a.get("st", 0))})
                al["al"] = out
            return self.send_json(al)

        # simple groups
        simple = {
            "weather": ("on", "name", "lat", "lon", "unit"),
            "display": ("bri", "idle_bri", "idle_s", "sleep_s", "h24", "night", "n_from", "n_to", "n_bri", "lang", "theme", "night_mode", "fx"),
            "cat": ("name", "fur", "clap", "show", "snd"),
            "pc": ("host", "port", "auto"),
            "update": ("auto",),
            "notify": ("dur", "vol", "quiet_low"),
        }
        if p.startswith("/api/config/") and p[12:] in simple:
            g = p[12:]
            out = pc_out if g == "pc" else cat_out if g == "cat" else (lambda: S[g])
            if m == "GET":
                return self.send_json(out())
            if m == "PUT":
                b = self.body()
                if g == "notify" and b.get("regen"):
                    S["notify"]["token"] = tok()
                    return self.send_json(S["notify"])
                tmp = copy.deepcopy(S[g])
                merge(tmp, b, simple[g])
                if g == "cat":
                    need(tmp["fur"] in ("tabby", "orange", "black", "white", "calico", "gray") and
                         0 < len(tmp["name"].encode()) <= 16 and
                         all(isinstance(tmp[k], bool) for k in ("clap", "show", "snd")))
                if g == "display":
                    need(tmp["lang"] in ("vi", "en") and 0 <= tmp["n_from"] <= 23 and 0 <= tmp["n_to"] <= 23)
                    need(tmp["theme"] in THEME_IDS and tmp["night_mode"] in ("auto", "on", "off") and tmp["fx"] in ("full", "eco"))
                    # legacy bool `night` mirrors night_mode; night_mode wins when both are sent
                    if "night_mode" in b:
                        tmp["night"] = tmp["night_mode"] != "off"
                    elif "night" in b:
                        tmp["night_mode"] = "auto" if tmp["night"] else "off"
                if g == "weather":
                    need(-90 <= tmp["lat"] <= 90 and -180 <= tmp["lon"] <= 180 and tmp["unit"] in ("c", "f"))
                    f = (lambda c: c * 9 / 5 + 32) if tmp["unit"] == "f" else (lambda c: c)  # like the device
                    tmp["now"] = {"t": round(f(random.uniform(18, 33)), 1), "code": random.choice([0, 1, 3, 61, 80, 95]),
                                  "hi": round(f(31)), "lo": round(f(22)), "at": int(time.time())}
                if g == "pc":
                    need(0 < tmp["port"] < 65536)
                S[g] = tmp
                return self.send_json(out())

        # system
        if key == ("POST", "/api/ota"):
            n = int(self.headers.get("Content-Length") or 0)
            if n > MAX_FW:
                self.raw_body(n)
                raise Err(413, "too_large")
            data = b""
            while len(data) < n:
                chunk = self.rfile.read(min(65536, n - len(data)))
                if not chunk:
                    break
                data += chunk
                time.sleep(0.01)  # simulate flash write speed so the progress bar moves
            need(data[:1] == b"\xe9", err="bad_image")
            print(f"  [OTA] received {len(data)} bytes, would reboot")
            return self.send_json({"ok": True})
        if p == "/api/update" and m == "GET":
            return self.send_json(upd_out())
        if key == ("POST", "/api/update/check"):
            self.body()
            u = S["upd"]
            need(u["state"] not in ("download", "verify", "reboot"), code=409, err="busy")
            u.update(state="check", t0=time.time(), err="")
            return self.send_json(upd_out())
        if key == ("POST", "/api/update"):
            b = self.body()
            u = S["upd"]
            need(u["state"] not in ("check", "download", "verify", "reboot"), code=409, err="busy")
            need(u["new"] and b.get("ver") == u["new"]["ver"], code=409, err="stale")
            u.update(state="download", t0=time.time(), pct=0, err="")
            return self.send_json(upd_out())
        if key == ("POST", "/api/pc/relay_test"):
            self.body()
            return self.send_json({"ok": True, "relay": "192.168.1.10:8765", "ms": 4})
        if key == ("POST", "/api/reboot"):
            self.body()
            SESS.clear()
            return self.send_json({"ok": True})
        if key == ("POST", "/api/factory_reset"):
            b = self.body()
            need(b.get("confirm") is True and b.get("scope", "all") in ("all", "data"))
            if b.get("scope") == "data":  # history goes, configuration stays (the speaker reboots)
                S["hist"] = fresh_state()["hist"]
            else:
                S.clear()
                S.update(fresh_state())
            SESS.clear()
            return self.send_json({"ok": True})
        raise Err(404, "not_found")

    def notify(self, m, p, q):
        if p == "/api/notify" and m == "POST":
            b = self.body()
            return self.send_json(add_notify(b, "api"))
        if p == "/api/notify" and m == "GET":
            b = {k: v[0] for k, v in q.items() if k != "token"}
            if "duration" in b:
                b["duration"] = int(b["duration"])
            return self.send_json(add_notify(b, "api"))
        if p == "/api/notify/audio" and m == "POST":
            data = self.raw_body(1 << 20)
            need(data, err="bad_value")
            return self.send_json(add_notify({"title": (q.get("title") or [""])[0],
                                              "text": (q.get("text") or [f"audio {len(data)} B"])[0]}, "audio"))
        if p == "/api/notify/history" and m == "GET":
            return self.send_json({"items": S["hist"]})
        raise Err(404, "not_found")


def read_pack(fn):
    """web.pack written by build.py -> {path: (data, gzip, immutable, etag)}"""
    b = open(fn, "rb").read()
    magic, ver, count, off = struct.unpack_from("<4sIII", b, 0)
    if magic != b"MBWP" or ver != 1:
        sys.exit(f"{fn}: not a web pack")
    out = {}
    for i in range(count):
        p, o, n, fl, e = struct.unpack_from("<56sIII12s", b, 16 + 80 * i)
        out[p.rstrip(b"\0").decode()] = (b[off + o:off + o + n], bool(fl & 1), bool(fl & 2), e.rstrip(b"\0").decode())
    return out


def main():
    for f in (sys.stdout, sys.stderr):
        try:
            f.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--dir", default=os.path.join(HERE, "src"))
    ap.add_argument("--no-auth", action="store_true", help="skip login (dev only)")
    ap.add_argument("--no-screenshot", action="store_true", help="/api/screenshot returns 404")
    ap.add_argument("--theme", choices=THEME_IDS, help="start with this display.theme")
    ap.add_argument("--dist", action="store_true", help="serve the built pack (components/webui/web.pack) like the firmware")
    a = ap.parse_args()
    if a.dist:
        global PACK
        PACK = read_pack(PACK_FILE)
    global NO_AUTH, NO_SHOT
    NO_AUTH, NO_SHOT = a.no_auth, a.no_screenshot
    if a.theme:
        S["display"]["theme"] = a.theme
    H.root = os.path.abspath(a.dir)
    print(f"MeowBox mock on http://localhost:{a.port}  (meowbox / m3owb0x2026)  token={S['notify']['token']}")
    ThreadingHTTPServer((a.host, a.port), H).serve_forever()


if __name__ == "__main__":
    main()
