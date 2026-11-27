#!/usr/bin/env python3
"""Contract test: starts mock_server.py, exercises every endpoint in API.md, checks response shapes,
and checks that every /api/... path literal used in src/app.js is served by the mock (not 404).
Usage: python test_api.py   (stdlib only)
"""
import json, os, re, socket, struct, subprocess, sys, time, urllib.request, urllib.error
from http.cookiejar import CookieJar

HERE = os.path.dirname(os.path.abspath(__file__))
s = socket.socket(); s.bind(("127.0.0.1", 0)); PORT = s.getsockname()[1]; s.close()
BASE = f"http://127.0.0.1:{PORT}"
jar = CookieJar()
op = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar))
FAIL = []


def req(method, path, body=None, headers=None, raw=None, opener=None):
    h = dict(headers or {})
    data = raw
    if body is not None:
        data = json.dumps(body).encode(); h["Content-Type"] = "application/json"
    r = urllib.request.Request(BASE + path, data=data, method=method, headers=h)
    try:
        with (opener or op).open(r, timeout=10) as resp:
            return resp.status, parse(resp.read(), resp.headers)
    except urllib.error.HTTPError as e:
        return e.code, parse(e.read(), e.headers)


def parse(raw, hdr):
    """JSON body as dict; non-JSON (BMP, CSS...) as (content-type, bytes)."""
    if "json" in (hdr.get("Content-Type") or ""):
        return json.loads(raw or b"{}")
    return (hdr.get("Content-Type"), raw)


def check(name, cond):
    print(("  ok   " if cond else "  FAIL ") + name)
    if not cond:
        FAIL.append(name)


def keys(d, *ks):
    return isinstance(d, dict) and all(k in d for k in ks)


proc = subprocess.Popen([sys.executable, os.path.join(HERE, "mock_server.py"), "--port", str(PORT), "--host", "127.0.0.1"],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    for _ in range(50):
        try:
            urllib.request.urlopen(BASE + "/", timeout=1); break
        except Exception:
            time.sleep(0.1)

    print("static")
    for f in ("/", "/app.css", "/app.js", "/themes.css", "/lang/vi.json", "/lang/en.json", "/theme/zen.css"):
        with urllib.request.urlopen(BASE + f) as r:
            check(f"GET {f} 200", r.status == 200)

    check("missing theme preview -> 404 (UI shows a placeholder)", req("GET", "/themes/nope.png")[0] == 404)
    check("static path traversal blocked", req("GET", "/fonts/../../web/API.md")[0] == 404)

    print("theme (no auth)")
    c, j = req("GET", "/api/theme")
    check("GET /api/theme without session", c == 200 and j.get("theme") == "swiss" and isinstance(j.get("fonts"), list))
    check("themes without session -> 401", req("GET", "/api/themes")[0] == 401)
    check("screenshot without session -> 401", req("GET", "/api/screenshot")[0] == 401)

    print("auth")
    check("status without session -> 401", req("GET", "/api/status")[0] == 401)
    check("bad login -> 401", req("POST", "/api/login", {"user": "meowbox", "pass": "x"})[0] == 401)
    c, j = req("POST", "/api/login", {"user": "meowbox", "pass": "m3owb0x2026"})
    check("login ok + def_pw", c == 200 and j.get("def_pw") is True and any(k.name == "sid" for k in jar))
    c, j = req("GET", "/api/session"); check("session", c == 200 and keys(j, "user", "def_pw"))

    print("status")
    c, j = req("GET", "/api/status")
    check("status shape", c == 200 and keys(j, "ver", "build", "up", "mode", "ssid", "ip", "rssi", "bat", "chg", "usb",
                                             "heap", "heap_min", "psram", "psram_tot", "time", "vol", "sleep", "pc", "def_pw", "play")
          and keys(j["play"], "st", "id", "name", "title"))

    print("wifi")
    c, j = req("GET", "/api/config/wifi"); check("wifi get", keys(j, "cur", "max", "nets") and "pass" not in json.dumps(j))
    c, j = req("GET", "/api/wifi/scan"); check("scan", c == 200 and keys(j["aps"][0], "ssid", "rssi", "ch", "sec"))
    c, j = req("POST", "/api/wifi/connect", {"ssid": "VanPhong_5G", "pass": "abcdefgh"}); check("connect", j.get("ok"))
    c, j = req("PUT", "/api/config/wifi", {"nets": [{"ssid": "NhaMeo"}, {"ssid": "VanPhong_5G"}]})
    check("wifi put reorder", c == 200 and [n["ssid"] for n in j["nets"]] == ["NhaMeo", "VanPhong_5G"])
    check("wifi put unknown w/o pass -> 400", req("PUT", "/api/config/wifi", {"nets": [{"ssid": "Nope"}]})[0] == 400)

    print("radio")
    c, j = req("GET", "/api/config/radio"); check("radio get", keys(j, "vol", "last", "max", "st") and keys(j["st"][0], "id", "name", "url", "type"))
    st = j["st"] + [{"id": 9, "name": "Test", "url": "http://x/y.mp3", "type": "stream"},
                    {"id": 10, "name": "YT", "url": "https://youtu.be/abc", "type": "youtube"}]
    c, j = req("PUT", "/api/config/radio", {"st": st}); check("radio put", c == 200 and len(j["st"]) == len(st))
    check("radio put dup id -> 400", req("PUT", "/api/config/radio", {"st": st + [st[0]]})[0] == 400)
    for b in ({"cmd": "play", "id": 9}, {"cmd": "play"}, {"cmd": "play", "url": "http://a/b", "name": "P"},
              {"cmd": "next"}, {"cmd": "prev"}, {"cmd": "vol", "vol": 55}, {"cmd": "stop"}):
        c, j = req("POST", "/api/radio/play", b)
        check(f"radio/play {b}", c == 200 and keys(j, "ok", "vol", "play"))
    check("radio/play unknown id -> 404", req("POST", "/api/radio/play", {"cmd": "play", "id": 999})[0] == 404)
    for u in ("youtu.be/abc", "ftp://ice.example/live.mp3"):
        c, j = req("POST", "/api/radio/play", {"cmd": "play", "url": u})
        check(f"radio/play url {u!r} -> 400", c == 400 and j.get("err") == "bad_value")
    c, j = req("POST", "/api/sleep", {"min": 30}); check("sleep set", 1795 <= j.get("left", 0) <= 1800)
    c, j = req("GET", "/api/sleep"); check("sleep get", keys(j, "left"))
    c, j = req("POST", "/api/sleep", {"min": 0}); check("sleep cancel", j.get("left") == 0)

    print("alarms")
    c, j = req("GET", "/api/config/alarms"); check("alarms get", keys(j, "snooze", "max", "al") and keys(j["al"][0], "on", "h", "m", "days", "snd", "st"))
    al = [{"on": True, "h": 6, "m": 30, "days": 31, "snd": "chime", "st": 0}, {"on": False, "h": 8, "m": 0, "days": 96, "snd": "radio", "st": 9}]
    c, j = req("PUT", "/api/config/alarms", {"snooze": 7, "al": al}); check("alarms put", c == 200 and j["snooze"] == 7 and len(j["al"]) == 2)
    check("alarms >5 -> 400", req("PUT", "/api/config/alarms", {"al": al * 3})[0] == 400)
    req("PUT", "/api/config/radio", {"st": st[:-2]})
    c, j = req("GET", "/api/config/alarms"); check("deleted station alarm -> chime", j["al"][1]["snd"] == "chime")

    print("weather/display/cat/pc/notify")
    c, j = req("PUT", "/api/config/weather", {"on": True, "name": "Đà Nẵng", "lat": 16.068, "lon": 108.212, "unit": "c"})
    check("weather put", c == 200 and j["name"] == "Đà Nẵng" and keys(j["now"], "t", "code", "hi", "lo", "at"))
    c, j = req("GET", "/api/config/display"); check("display get", keys(j, "bri", "idle_bri", "idle_s", "sleep_s", "h24", "night", "n_from", "n_to", "n_bri", "lang", "tz"))
    c, j = req("PUT", "/api/config/display", {"bri": 50, "lang": "en", "n_from": 23}); check("display put", j["bri"] == 50 and j["lang"] == "en")
    c, j = req("PUT", "/api/config/display", {"sleep_s": 120}); check("display sleep_s", c == 200 and j["sleep_s"] == 120)
    check("display bad type -> 400", req("PUT", "/api/config/display", {"bri": "x"})[0] == 400)
    check("display defaults theme/night_mode/fx", j.get("theme") == "swiss" and j.get("night_mode") == "auto" and j.get("fx") == "full")
    c, j = req("GET", "/api/themes")
    ids = [x["id"] for x in j.get("themes", [])]
    check("themes list", c == 200 and j.get("current") == "swiss" and {"swiss", "cyberpunk", "zen", "brutal"} <= set(ids)
          and all(keys(x, "id", "name", "desc", "default", "fx", "preview", "fonts") and keys(x["name"], "vi", "en") for x in j["themes"]))
    check("fx flag: effects only in cyberpunk and brutal",
          {x["id"]: x["fx"] for x in j["themes"]} == {"swiss": False, "cyberpunk": True, "zen": False, "brutal": True})
    check("exactly one default theme (swiss)", [x["id"] for x in j["themes"] if x["default"]] == ["swiss"])
    c, j = req("PUT", "/api/config/display", {"theme": "zen"}); check("set theme", c == 200 and j["theme"] == "zen")
    c, j = req("GET", "/api/theme", opener=urllib.request.build_opener()); check("/api/theme follows display.theme", j.get("theme") == "zen")
    c, j = req("GET", "/api/themes"); check("themes.current follows", j["current"] == "zen")
    check("unknown theme -> 400", req("PUT", "/api/config/display", {"theme": "neon"})[0] == 400)
    c, j = req("PUT", "/api/config/display", {"night_mode": "off", "fx": "eco"})
    check("night_mode/fx put", j["night_mode"] == "off" and j["fx"] == "eco" and j["night"] is False)
    check("bad night_mode -> 400", req("PUT", "/api/config/display", {"night_mode": "dusk"})[0] == 400)
    check("bad fx -> 400", req("PUT", "/api/config/display", {"fx": "max"})[0] == 400)
    req("PUT", "/api/config/display", {"theme": "swiss", "night_mode": "auto", "fx": "full"})
    c, j = req("GET", "/api/log")
    check("log", c == 200 and keys(j, "boot", "reason", "up_s", "prev", "prev_log", "log", "boots")
          and isinstance(j["prev_log"], list) and isinstance(j["boots"], list))
    c, (ct, raw) = req("GET", "/api/screenshot")
    w, hh = struct.unpack("<ii", raw[18:26]) if len(raw) > 26 else (0, 0)
    check("screenshot BMP 296x240", c == 200 and ct == "image/bmp" and raw[:2] == b"BM" and (w, abs(hh)) == (296, 240))
    c, j = req("GET", "/api/config/cat"); check("cat get", keys(j, "name", "fur", "clap", "show", "snd", "st") and keys(j["st"], "pc", "s"))
    check("cat sessions", all(keys(x, "id", "ag", "name", "dir", "st", "act", "for", "tok", "model") and
                              x["st"] in ("work", "wait", "done", "err", "idle") for x in j["st"]["s"]) and
          ("lim" not in j["st"] or all(isinstance(v, int) for v in j["st"]["lim"].values())))
    c, j = req("PUT", "/api/config/cat", {"name": "Mun", "fur": "black", "clap": False, "show": False, "snd": True}); check("cat put", j["fur"] == "black" and j["clap"] is False and j["show"] is False and "st" in j)
    c, j = req("GET", "/api/config/pc"); check("pc get", keys(j, "host", "port", "auto", "st") and keys(j["st"], "on", "name", "ip", "ver", "seen"))
    c, j = req("PUT", "/api/config/pc", {"host": "10.0.0.2", "port": 9000, "auto": False}); check("pc put", j["port"] == 9000)
    c, n = req("GET", "/api/config/notify"); check("notify cfg", keys(n, "token", "dur", "vol", "quiet_low"))
    c, j = req("PUT", "/api/config/notify", {"dur": 12}); check("notify cfg put", j["dur"] == 12)
    c, j = req("PUT", "/api/config/notify", {"regen": True}); check("token regen", j["token"] != n["token"] and len(j["token"]) == 16)
    T = j["token"]
    bare = urllib.request.build_opener()  # no cookies
    msg = {"title": "Build xong", "text": "esp32-playground OK", "sound": "chime", "priority": "normal", "duration": 10}
    check("notify no auth -> 401", req("POST", "/api/notify", msg, opener=bare)[0] == 401)
    check("notify old token -> 401", req("POST", "/api/notify", msg, {"Authorization": "Bearer " + n["token"]}, opener=bare)[0] == 401)
    c, j = req("POST", "/api/notify", msg, {"Authorization": "Bearer " + T}, opener=bare); check("notify bearer", c == 200 and keys(j, "ok", "id"))
    c, j = req("GET", f"/api/notify?text=Xin%20chao&token={T}", opener=bare); check("notify GET short", c == 200 and j.get("ok"))
    c, j = req("POST", "/api/notify/audio?title=Loa", raw=b"RIFF....WAVE", headers={"Authorization": "Bearer " + T, "Content-Type": "audio/wav"}, opener=bare)
    check("notify audio", c == 200 and j.get("ok"))
    c, j = req("GET", "/api/notify/history", headers={"Authorization": "Bearer " + T}, opener=bare)
    check("history", len(j["items"]) == 3 and keys(j["items"][0], "id", "ts", "title", "text", "sound", "priority", "src") and j["items"][0]["src"] == "audio")

    print("system")
    fw = b"\xe9" + os.urandom(300000)
    c, j = req("POST", "/api/ota", raw=fw, headers={"Content-Type": "application/octet-stream"}); check("ota ok", c == 200 and j.get("ok"))
    c, j = req("POST", "/api/ota", raw=b"\x00" * 100, headers={"Content-Type": "application/octet-stream"}); check("ota bad -> 400", c == 400 and j["err"] == "bad_image")
    check("password bad old -> 403", req("POST", "/api/password", {"old": "nope", "new": "abcdef"})[0] == 403)
    c, j = req("POST", "/api/wifi/portal", {"on": True}); check("portal on", c == 200 and j["on"] and j["off_in"] > 0)
    check("portal bad -> 400", req("POST", "/api/wifi/portal", {"on": "yes"})[0] == 400)
    c, j = req("POST", "/api/wifi/portal", {"on": False}); check("portal off", c == 200 and not j["on"])
    check("password short -> 400", req("POST", "/api/password", {"old": "m3owb0x2026", "new": "abc"})[0] == 400)
    c, j = req("POST", "/api/password", {"old": "m3owb0x2026", "new": "m30w-secret"}); check("password ok", j.get("ok"))
    c, j = req("GET", "/api/status"); check("def_pw cleared", j["def_pw"] is False)
    check("factory_reset w/o confirm -> 400", req("POST", "/api/factory_reset", {})[0] == 400)
    c, j = req("POST", "/api/logout", {}); check("logout", j.get("ok"))
    check("after logout -> 401", req("GET", "/api/status")[0] == 401)
    c, j = req("POST", "/api/login", {"user": "meowbox", "pass": "m30w-secret"}); check("login new pw", c == 200 and j["def_pw"] is False)
    c, j = req("POST", "/api/reboot", {}); check("reboot", j.get("ok"))
    req("POST", "/api/login", {"user": "meowbox", "pass": "m30w-secret"})
    check("factory_reset bad scope -> 400", req("POST", "/api/factory_reset", {"confirm": True, "scope": "x"})[0] == 400)
    c, j = req("POST", "/api/factory_reset", {"confirm": True, "scope": "data"}); check("clear data", j.get("ok"))
    c, j = req("POST", "/api/login", {"user": "meowbox", "pass": "m30w-secret"}); check("clear data keeps password", c == 200)
    c, j = req("POST", "/api/factory_reset", {"confirm": True, "scope": "all"}); check("factory reset", j.get("ok"))
    c, j = req("POST", "/api/login", {"user": "meowbox", "pass": "m3owb0x2026"}); check("default pw after reset", c == 200 and j["def_pw"])
    c, j = req("GET", "/api/theme"); check("factory reset -> swiss", j.get("theme") == "swiss")

    print("update (GitHub Releases)")
    c, j = req("GET", "/api/update")
    check("GET /api/update: cur/state/auto, no update known yet", c == 200 and keys(j, "cur", "state", "pct", "auto", "checked")
          and j["state"] == "idle" and "new" not in j and j["auto"] is False)
    c, j = req("POST", "/api/update", {"ver": "0.2.1"}); check("install before any check -> 409 stale", c == 409 and j.get("err") == "stale")
    c, j = req("POST", "/api/update/check", {}); check("check starts", c == 200 and j.get("state") == "check")
    time.sleep(2.2)
    c, j = req("GET", "/api/update")
    check("check finds a newer release with notes", j.get("state") == "idle" and j.get("new") == "0.2.1"
          and keys(j.get("notes", {}), "vi", "en") and j.get("checked", 0) > 0)
    c, j = req("GET", "/api/status"); check("status.upd names the new version", j.get("upd") == "0.2.1")
    c, j = req("POST", "/api/update", {"ver": "9.9.9"}); check("install other version -> 409 stale", c == 409 and j.get("err") == "stale")
    c, j = req("POST", "/api/update", {"ver": "0.2.1"}); check("install starts", c == 200 and j.get("state") == "download")
    c, j = req("POST", "/api/update/check", {}); check("check while installing -> 409 busy", c == 409 and j.get("err") == "busy")
    time.sleep(1.2)
    c, j = req("GET", "/api/update"); check("download progresses", j.get("state") == "download" and 0 < j.get("pct", 0) < 100)
    c, j = req("PUT", "/api/config/update", {"auto": True}); check("update.auto on", c == 200 and j.get("auto") is True)
    c, j = req("GET", "/api/update"); check("GET /api/update reflects auto", j.get("auto") is True)
    req("PUT", "/api/config/update", {"auto": False})

    print("app.js path coverage")
    js = open(os.path.join(HERE, "src", "app.js"), encoding="utf-8").read()
    paths = sorted(set(re.findall(r"'(/api/[a-z_/]+)'", js)))
    for p in paths:
        code = None
        for m in ("GET", "POST", "PUT"):
            code, _ = req(m, p, {} if m != "GET" else None,
                          headers={"Authorization": "Bearer x"} if p.startswith("/api/notify") else None)
            if code != 404:
                break
        check(f"app.js uses {p} -> served ({code})", code != 404)
    print(f"{len(paths)} distinct API paths referenced by app.js")

    print("strings (src/lang/*.json)")
    langs = {os.path.splitext(f)[0]: json.load(open(os.path.join(HERE, "src", "lang", f), encoding="utf-8"))
             for f in os.listdir(os.path.join(HERE, "src", "lang")) if f.endswith(".json")}
    check("vi and en present", {"vi", "en"} <= set(langs))
    for code, d in langs.items():
        miss, extra = set(langs["vi"]) - set(d), set(d) - set(langs["vi"])
        check(f"{code}.json has the same keys as vi.json (missing {sorted(miss)[:5]}, extra {sorted(extra)[:5]})",
              not miss and not extra)
        for k, v in d.items():
            if isinstance(v, dict) and isinstance(langs["vi"].get(k), dict) and set(v) != set(langs["vi"][k]):
                check(f"{code}.json '{k}' has the same sub-keys as vi.json", False)
    used = set(re.findall(r"\b(?:t|opt)\('([A-Za-z_][A-Za-z0-9_]*)'", js))
    missing = sorted(k for k in used if k not in langs["vi"])
    check(f"every t('...')/opt('...') key used in app.js exists ({len(used)} keys; missing {missing})", not missing)
    bat = set(re.findall(r"\bB\('([A-Za-z_]+)'", js))
    check(f"every B('...') key exists in 'bat' (missing {sorted(bat - set(langs['vi'].get('bat', {})))})",
          bat <= set(langs["vi"].get("bat", {})))

    print("login lockout (last: it locks this client out for 30 s)")
    check("password over 64 chars -> 401", req("POST", "/api/login", {"user": "meowbox", "pass": "x" * 5000})[0] == 401)
    for _ in range(4):
        req("POST", "/api/login", {"user": "meowbox", "pass": "wrong"})
    c, j = req("POST", "/api/login", {"user": "meowbox", "pass": "m3owb0x2026"})
    check("5 failures -> 429 even with the right password", c == 429 and j.get("err") == "too_many")
finally:
    proc.terminate()

# ---- every theme ships the same pieces (docs/firmware_v0.2.0.md, 1.3)
print("theme files")
THEMES_META = json.load(open(os.path.join(HERE, "themes.json"), encoding="utf-8"))
font_files = set(os.listdir(os.path.join(HERE, "assets", "fonts")))
for th in THEMES_META:
    tid = th["id"]
    css_p = os.path.join(HERE, "src", "theme", tid + ".css")
    check(f"{tid}: src/theme/{tid}.css exists", os.path.isfile(css_p))
    check(f"{tid}: assets/themes/{tid}.png exists", os.path.isfile(os.path.join(HERE, "assets", "themes", tid + ".png")))
    missing = [f for f in th["fonts"] if f not in font_files
               and not any(x.startswith(f[:-len("-vi.woff2")] + "-") for x in font_files)]
    check(f"{tid}: web fonts present (missing {missing})", not missing)
    if th["fx"] and os.path.isfile(css_p):
        check(f"{tid}: has effects, so theme/{tid}.css has a [data-fx=eco] saver variant",
              "data-fx=eco" in open(css_p, encoding="utf-8").read())


# ---- built pack (web/build.py -> components/webui/web.pack), served by the mock exactly like the firmware
print("built pack (mock --dist)")
s2 = socket.socket(); s2.bind(("127.0.0.1", 0)); PORT2 = s2.getsockname()[1]; s2.close()
proc = subprocess.Popen([sys.executable, os.path.join(HERE, "mock_server.py"), "--port", str(PORT2), "--host", "127.0.0.1",
                         "--dist"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
B2 = f"http://127.0.0.1:{PORT2}"


def raw_get(path, hdr=None):
    r = urllib.request.Request(B2 + path, headers=hdr or {})
    try:
        with urllib.request.urlopen(r) as x:
            return x.status, dict(x.headers), x.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()


try:
    import gzip as _gz
    for _ in range(50):
        try:
            urllib.request.urlopen(B2 + "/api/theme", timeout=1); break
        except Exception:
            time.sleep(0.1)
    c, h, body = raw_get("/")
    html = _gz.decompress(body).decode() if h.get("Content-Encoding") == "gzip" else body.decode()
    check("GET / -> index.html, no-cache + ETag", c == 200 and h.get("Cache-Control") == "no-cache" and bool(h.get("ETag")))
    c304, _, b304 = raw_get("/", {"If-None-Match": h.get("ETag", "")})
    check("If-None-Match on index.html -> 304, empty body", c304 == 304 and not b304)
    refs = re.findall(r'(?:href|src)="([^"]+\.(?:css|js))"', html)
    mbm = re.search(r"window\.MB=(\{.*?\});", html)
    mb = json.loads(mbm.group(1)) if mbm else {}
    check("index.html has window.MB with vi/en and the 4 themes",
          {"vi", "en"} <= set(mb.get("l", {})) and {"swiss", "cyberpunk", "zen", "brutal"} <= set(mb.get("t", {})))
    hashed = refs + list(mb.get("l", {}).values()) + list(mb.get("t", {}).values())
    check(f"index.html references hashed files ({len(hashed)})",
          len(refs) == 3 and all(re.search(r"\.[0-9a-f]{8}\.(css|js|json)$", x) for x in hashed))
    bad = []
    for x in hashed:
        c, h, _ = raw_get("/" + x)
        if c != 200 or "immutable" not in h.get("Cache-Control", "") or h.get("Content-Encoding") != "gzip":
            bad.append((x, c, h.get("Cache-Control")))
    check(f"every hashed file -> 200, gzip, immutable (bad: {bad})", not bad)
    c, h, _ = raw_get("/fonts/fonts.json")
    check("fonts/fonts.json served, cached 7 days", c == 200 and h.get("Cache-Control") == "max-age=604800")
    c, h, _ = raw_get("/themes/swiss.png")
    check("themes/swiss.png served", c == 200 and h.get("Content-Type") == "image/png")
    check("unhashed or unknown file -> 404", raw_get("/app.js")[0] == 404 and raw_get("/nope.css")[0] == 404)
finally:
    proc.terminate()

print("\nRESULT:", "ALL PASSED" if not FAIL else f"{len(FAIL)} FAILED: {FAIL}")
sys.exit(1 if FAIL else 0)
