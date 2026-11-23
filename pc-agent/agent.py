"""MeowBox PC agent: PC stats + media -> speaker (WebSocket), notify CLI, TTS, YouTube relay."""
import argparse
import asyncio
import functools
import json
import logging
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

import aiohttp
import psutil
from aiohttp import web

log = logging.getLogger("meowbox")
compact = functools.partial(json.dumps, ensure_ascii=False, separators=(",", ":"))
CONFIG_PATH = Path(os.environ.get("APPDATA", Path.home())) / "MeowBox" / "agent.json"
DEFAULTS = {"host": "", "token": "", "relay_port": 8765}

# ---------------------------------------------------------------- config / discovery


def load_config(path: Path) -> dict:
    cfg = dict(DEFAULTS)
    try:
        cfg.update(json.loads(path.read_text(encoding="utf-8")))
    except FileNotFoundError:
        pass
    return cfg


def save_config(path: Path, cfg: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(cfg, indent=2), encoding="utf-8")


def discover(timeout: float = 3.0) -> str | None:
    """Find the speaker: mDNS service _meowbox._tcp first, then the hostname meowbox.local."""
    try:
        from zeroconf import ServiceBrowser, Zeroconf

        found: list[str] = []
        zc = Zeroconf()

        class Listener:
            def add_service(self, zc_, type_, name):
                info = zc_.get_service_info(type_, name, 2000)
                addrs = sorted(info.parsed_addresses(), key=lambda a: a.startswith("169.254.")) if info else []
                if addrs:
                    found.append(f"{addrs[0]}:{info.port}")

            update_service = remove_service = lambda *a: None

        ServiceBrowser(zc, "_meowbox._tcp.local.", Listener())
        end = time.monotonic() + timeout
        while not found and time.monotonic() < end:
            time.sleep(0.1)
        zc.close()
        if found:
            return found[0]
    except Exception as e:  # noqa: BLE001
        log.debug("zeroconf failed: %s", e)
    try:
        return socket.gethostbyname("meowbox.local")
    except OSError:
        return None


def resolve_host(cfg: dict) -> str:
    host = cfg.get("host") or discover()
    if not host:
        raise SystemExit("Không tìm thấy loa (mDNS). Dùng --host IP hoặc: agent.py config --host IP")
    return host


# ---------------------------------------------------------------- stats


class Stats:
    def __init__(self):
        psutil.cpu_percent(None)
        self.net = psutil.net_io_counters()
        self.t = time.monotonic()
        self.disk_path = os.environ.get("SystemDrive", "C:") + "\\"
        self.lhm_retry = 0.0
        self.nvsmi = shutil.which("nvidia-smi")

    async def lhm(self, http: aiohttp.ClientSession) -> dict:
        """Read LibreHardwareMonitor's web server (Options > Remote Web Server, port 8085)."""
        if time.monotonic() < self.lhm_retry:
            return {}
        try:
            async with http.get("http://localhost:8085/data.json", timeout=aiohttp.ClientTimeout(total=0.5)) as r:
                return parse_lhm(await r.json(content_type=None))
        except Exception:  # noqa: BLE001
            self.lhm_retry = time.monotonic() + 30
            return {}

    async def nvidia(self) -> dict:
        if not self.nvsmi:
            return {}
        try:
            p = await asyncio.create_subprocess_exec(
                self.nvsmi, "--query-gpu=utilization.gpu,temperature.gpu", "--format=csv,noheader,nounits",
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, creationflags=subprocess.CREATE_NO_WINDOW)
            out, _ = await asyncio.wait_for(p.communicate(), 3)
            load, temp = (float(x) for x in out.decode().splitlines()[0].split(","))
            return {"gpu": load, "gpu_temp": temp}
        except Exception:  # noqa: BLE001
            return {}

    async def sample(self, http: aiohttp.ClientSession) -> dict:
        now, net = time.monotonic(), psutil.net_io_counters()
        dt = max(now - self.t, 1e-3)
        msg = {
            "t": "stats",
            "cpu": round(psutil.cpu_percent(None)),
            "ram": round(psutil.virtual_memory().percent),
            "gpu": None, "cpu_temp": None, "gpu_temp": None,
            "net_up": round((net.bytes_sent - self.net.bytes_sent) / 1024 / dt),
            "net_down": round((net.bytes_recv - self.net.bytes_recv) / 1024 / dt),
            "disk": round(psutil.disk_usage(self.disk_path).percent),
        }
        self.net, self.t = net, now
        for extra in (await self.nvidia(), await self.lhm(http)):  # LHM wins if both present
            msg.update({k: round(v) for k, v in extra.items() if v is not None})
        return msg


def parse_lhm(root: dict) -> dict:
    sensors = []  # (hardware kind, group, name, value)

    def walk(node, hw, group):
        img, text = node.get("ImageURL", ""), node.get("Text", "")
        kids = node.get("Children") or []
        if not kids and node.get("Value"):
            m = re.match(r"-?[\d.,]+", node["Value"])
            if m:
                sensors.append((hw, group, text, float(m.group().replace(",", "."))))
            return
        if "temperature" in img:
            group = "temp"
        elif "load" in img:
            group = "load"
        elif "cpu" in img:
            hw = "cpu"
        elif any(k in img for k in ("nvidia", "ati", "amd", "gpu", "intel")):
            hw = "gpu"
        for k in kids:
            walk(k, hw, group)

    walk(root, None, None)

    def pick(hw, group, names):
        vals = [s for s in sensors if s[0] == hw and s[1] == group]
        for n in names:
            for s in vals:
                if s[2].startswith(n):
                    return s[3]
        return max((s[3] for s in vals), default=None) if group == "temp" else None

    return {
        "cpu_temp": pick("cpu", "temp", ("CPU Package", "Core (Tctl", "Package", "CPU Core")),
        "gpu_temp": pick("gpu", "temp", ("GPU Core", "GPU Hot Spot")),
        "gpu": pick("gpu", "load", ("GPU Core", "D3D 3D")),
    }


# ---------------------------------------------------------------- media (Windows SMTC)

try:
    from winrt.windows.media.control import GlobalSystemMediaTransportControlsSessionManager as MediaManager
except ImportError:
    try:
        from winsdk.windows.media.control import GlobalSystemMediaTransportControlsSessionManager as MediaManager
    except ImportError:
        MediaManager = None

VK = {"play_pause": 0xB3, "next": 0xB0, "prev": 0xB1, "vol_up": 0xAF, "vol_down": 0xAE}
STATES = {4: "playing", 5: "paused"}  # GlobalSystemMediaTransportControlsSessionPlaybackStatus


class Media:
    def __init__(self):
        self.mgr = None

    async def session(self):
        if MediaManager is None:
            return None
        if self.mgr is None:
            self.mgr = await MediaManager.request_async()
        return self.mgr.get_current_session()

    async def read(self) -> dict:
        empty = {"t": "media", "app": "", "title": "", "artist": "", "state": "stopped", "pos": 0, "len": 0}
        try:
            s = await self.session()
            if not s:
                return empty
            props = await s.try_get_media_properties_async()
            tl = s.get_timeline_properties()
            state = STATES.get(int(s.get_playback_info().playback_status), "stopped")
            pos, length = tl.position.total_seconds(), tl.end_time.total_seconds()
            if state == "playing" and tl.last_updated_time.year > 1601:  # position is a snapshot
                pos += (datetime.now(timezone.utc) - tl.last_updated_time).total_seconds()
            app = s.source_app_user_model_id.split("!")[-1].removesuffix(".exe")
            return {**empty, "app": app, "title": props.title, "artist": props.artist, "state": state,
                    "pos": round(min(pos, length) if length else pos), "len": round(length)}
        except Exception as e:  # noqa: BLE001
            log.debug("media read failed: %s", e)
            return empty

    async def command(self, cmd: str) -> None:
        if cmd not in VK:
            log.warning("unknown media_cmd %r", cmd)
            return
        log.info("media_cmd %s", cmd)
        try:
            if cmd.startswith("vol_"):
                set_volume(+0.05 if cmd == "vol_up" else -0.05)
                return
            s = await self.session()
            if s:
                op = {"play_pause": s.try_toggle_play_pause_async, "next": s.try_skip_next_async,
                      "prev": s.try_skip_previous_async}[cmd]
                if await op():
                    return
        except Exception as e:  # noqa: BLE001
            log.debug("media api failed (%s), using media key", e)
        press_key(VK[cmd])


def set_volume(delta: float) -> None:
    try:
        from pycaw.pycaw import AudioUtilities

        ep = AudioUtilities.GetSpeakers().EndpointVolume
        ep.SetMasterVolumeLevelScalar(min(1.0, max(0.0, ep.GetMasterVolumeLevelScalar() + delta)), None)
    except Exception as e:  # noqa: BLE001
        log.debug("pycaw failed (%s), using volume key", e)
        press_key(VK["vol_up" if delta > 0 else "vol_down"])


def press_key(vk: int) -> None:
    import ctypes

    ctypes.windll.user32.keybd_event(vk, 0, 0, 0)
    ctypes.windll.user32.keybd_event(vk, 0, 2, 0)  # KEYEVENTF_KEYUP


# ---------------------------------------------------------------- WebSocket link to the speaker


async def ws_loop(cfg: dict, relay_port: int | None) -> None:
    stats, media, backoff = Stats(), Media(), 1
    async with aiohttp.ClientSession() as http:
        while True:
            host = cfg.get("host") or await asyncio.to_thread(discover)
            if not host:
                log.warning("speaker not found, retry in %ss", backoff)
            else:
                try:
                    async with http.ws_connect(f"ws://{host}/ws/pc", heartbeat=15) as ws:
                        log.info("connected to %s", host)
                        backoff = 1
                        await session(ws, http, stats, media, relay_port)
                except (aiohttp.ClientError, OSError, asyncio.TimeoutError) as e:
                    log.warning("connection to %s failed: %s", host, e)
                log.info("disconnected, retry in %ss", backoff)
            await asyncio.sleep(backoff)
            backoff = min(backoff * 2, 30)


async def session(ws, http, stats: Stats, media: Media, relay_port: int | None) -> None:
    if relay_port:
        ip = ws.get_extra_info("sockname")[0]
        await ws.send_str(compact({"t": "hello", "relay": f"http://{ip}:{relay_port}"}))

    async def sender():
        last, last_t = None, 0.0
        while True:
            await ws.send_str(compact(await stats.sample(http)))
            m = await media.read()
            key = {k: v for k, v in m.items() if k != "pos"}
            if key != last or time.monotonic() - last_t >= 5:
                await ws.send_str(compact(m))
                last, last_t = key, time.monotonic()
            await asyncio.sleep(1)

    task = asyncio.create_task(sender())
    try:
        async for msg in ws:
            if msg.type != aiohttp.WSMsgType.TEXT:
                continue
            try:
                data = json.loads(msg.data)
            except ValueError:
                continue
            if data.get("t") == "media_cmd":
                await media.command(data.get("cmd", ""))
                await ws.send_str(compact(await media.read()))
    finally:
        task.cancel()


# ---------------------------------------------------------------- YouTube relay

YTDLP = ["yt-dlp", "--no-warnings", "--quiet", "--no-progress"]
_info_cache: dict[tuple, tuple[float, dict]] = {}


async def ytjson(url: str, single: bool) -> dict:
    key = (url, single)
    hit = _info_cache.get(key)
    if hit and time.monotonic() - hit[0] < 1800:
        return hit[1]
    args = [*YTDLP, "-J", "--flat-playlist", *(["--no-playlist"] if single else []), url]
    p = await asyncio.create_subprocess_exec(*args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                             creationflags=subprocess.CREATE_NO_WINDOW)
    out, err = await asyncio.wait_for(p.communicate(), 90)
    if p.returncode:
        raise web.HTTPBadGateway(text=f"yt-dlp: {err.decode(errors='replace').strip()[-500:]}")
    info = json.loads(out)
    _info_cache[key] = (time.monotonic(), info)
    return info


def entry_url(e: dict) -> str:
    u = e.get("url") or e.get("webpage_url") or e["id"]
    return u if u.startswith("http") else f"https://www.youtube.com/watch?v={u}"


async def resolve_video(url: str, index: int | None) -> tuple[dict, int | None, int | None]:
    """Return (full video info, playlist index, playlist count)."""
    info = await ytjson(url, single=index is None)
    if info.get("_type") != "playlist":
        return info, None, None
    entries = list(info.get("entries") or [])
    i = index or 0
    if not 0 <= i < len(entries):
        raise web.HTTPNotFound(text=f"index out of range (0..{len(entries) - 1})")
    return await ytjson(entry_url(entries[i]), single=True), i, len(entries)


async def yt_info(req: web.Request) -> web.Response:
    url = req.query.get("url") or ""
    if not url:
        raise web.HTTPBadRequest(text="missing url")
    info = await ytjson(url, single=False)
    res = {"type": info.get("_type", "video"), "title": info.get("fulltitle") or info.get("title"),
           "duration": info.get("duration"), "is_live": bool(info.get("is_live")), "uploader": info.get("uploader") or info.get("channel")}
    if res["type"] == "playlist":
        res["entries"] = [{"index": i, "title": e.get("title"), "duration": e.get("duration"), "url": entry_url(e)}
                          for i, e in enumerate(info.get("entries") or [])]
    return web.json_response(res)


def kill_tree(p: asyncio.subprocess.Process) -> None:
    if p.returncode is None:  # yt-dlp.exe (PyInstaller) has a child process, so kill the whole tree
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(p.pid)], capture_output=True,
                       creationflags=subprocess.CREATE_NO_WINDOW)


async def yt_stream(req: web.Request) -> web.StreamResponse:
    url = req.query.get("url") or ""
    if not url:
        raise web.HTTPBadRequest(text="missing url")
    index = int(req.query["index"]) if req.query.get("index", "").isdigit() else None
    info, i, count = await resolve_video(url, index)
    title = re.sub(r"[\r\n]+", " ", info.get("fulltitle") or info.get("title") or "YouTube")

    fd, info_path = tempfile.mkstemp(suffix=".info.json")
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump(info, f)
    rfd, wfd = os.pipe()
    flags = subprocess.CREATE_NO_WINDOW
    ytdlp = await asyncio.create_subprocess_exec(
        *YTDLP, "--load-info-json", info_path, "-f", "bestaudio/best", "-o", "-",
        stdout=wfd, stderr=subprocess.DEVNULL, creationflags=flags)
    ffmpeg = await asyncio.create_subprocess_exec(
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-i", "pipe:0", "-vn", "-c:a", "libmp3lame",
        "-b:a", "128k", "-ar", "44100", "-ac", "2", "-f", "mp3", "pipe:1",
        stdin=rfd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, creationflags=flags)
    os.close(rfd)
    os.close(wfd)
    log.info("relay start: %s (%s)", title, req.remote)
    resp, sent = None, 0
    try:
        while chunk := await ffmpeg.stdout.read(16384):
            if resp is None:  # send headers only once audio really flows, so errors can still be a 502
                headers = {"Content-Type": "audio/mpeg", "Cache-Control": "no-cache", "icy-name": title,
                           "icy-br": "128", "X-Is-Live": str(bool(info.get("is_live"))).lower()}
                if info.get("duration"):
                    headers["X-Duration"] = str(round(info["duration"]))
                if i is not None:
                    headers.update({"X-Playlist-Index": str(i), "X-Playlist-Count": str(count)})
                resp = web.StreamResponse(headers=headers)
                resp.enable_chunked_encoding()
                await resp.prepare(req)
            await resp.write(chunk)
            sent += len(chunk)
    except (ConnectionError, aiohttp.ClientConnectionResetError):
        log.info("relay client gone")
    finally:
        kill_tree(ytdlp)
        kill_tree(ffmpeg)
        Path(info_path).unlink(missing_ok=True)
        log.info("relay stop: %s, %d KB sent", title, sent // 1024)
    if resp is None:
        raise web.HTTPBadGateway(text="no audio from yt-dlp/ffmpeg")
    return resp


async def start_relay(port: int) -> web.AppRunner:
    app = web.Application()
    app.add_routes([web.get("/yt/info", yt_info), web.get("/yt/stream", yt_stream)])
    runner = web.AppRunner(app, access_log=None)
    await runner.setup()
    await web.TCPSite(runner, "0.0.0.0", port).start()
    log.info("YouTube relay on http://0.0.0.0:%d/yt/stream?url=...", port)
    return runner


async def update_ytdlp(stamp: Path) -> None:
    """yt-dlp -U at most once per day; failures (e.g. winget/pip installs) are ignored."""
    try:
        if time.time() - stamp.stat().st_mtime < 86400:
            return
    except FileNotFoundError:
        pass
    stamp.parent.mkdir(parents=True, exist_ok=True)
    stamp.touch()
    try:
        p = await asyncio.create_subprocess_exec("yt-dlp", "-U", stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                                 creationflags=subprocess.CREATE_NO_WINDOW)
        out, _ = await asyncio.wait_for(p.communicate(), 180)
        log.info("yt-dlp -U: %s", out.decode(errors="replace").strip().splitlines()[-1:])
    except Exception as e:  # noqa: BLE001
        log.info("yt-dlp -U skipped: %s", e)


async def run(cfg: dict, args) -> None:
    port = None if args.no_relay else int(cfg["relay_port"])
    if port:
        try:
            await start_relay(port)
        except OSError as e:
            raise SystemExit(f"Không mở được cổng relay {port} (agent đang chạy rồi?): {e}")
        if not args.no_update:
            asyncio.create_task(update_ytdlp(args.config.parent / "ytdlp_update.stamp"))
    await ws_loop(cfg, port)


# ---------------------------------------------------------------- notify / say


def post(cfg: dict, path: str, body: bytes, ctype: str) -> None:
    req = urllib.request.Request(f"http://{resolve_host(cfg)}{path}", data=body, method="POST",
                                 headers={"Content-Type": ctype, "Authorization": f"Bearer {cfg['token']}"})
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            print(r.status, r.read().decode(errors="replace"))
    except urllib.error.HTTPError as e:
        raise SystemExit(f"HTTP {e.code}: {e.read().decode(errors='replace')}")
    except OSError as e:
        raise SystemExit(f"Lỗi kết nối: {e}")


async def tts_wav(text: str, voice_hint: str | None) -> bytes:
    from winrt.windows.media.speechsynthesis import SpeechSynthesizer
    from winrt.windows.storage.streams import DataReader

    synth = SpeechSynthesizer()
    voices = list(SpeechSynthesizer.all_voices)
    want = [v for v in voices if voice_hint and voice_hint.lower() in (v.display_name + v.language).lower()]
    want = want or [v for v in voices if v.language.lower().startswith("vi")]
    if want:
        synth.voice = want[0]
    log.info("TTS voice: %s", synth.voice.display_name)
    stream = await synth.synthesize_text_to_stream_async(text)
    reader = DataReader(stream.get_input_stream_at(0))
    n = await reader.load_async(stream.size)
    buf = bytearray(n)
    reader.read_bytes(buf)
    return bytes(buf)


# ---------------------------------------------------------------- CLI


def main() -> None:
    for s in (sys.stdout, sys.stderr):
        if s:  # None under pythonw.exe
            s.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description="MeowBox PC agent")
    ap.add_argument("--config", type=Path, default=CONFIG_PATH, help=f"file cấu hình (mặc định {CONFIG_PATH})")
    ap.add_argument("--host", help="IP[:port] của loa (bỏ qua mDNS)")
    ap.add_argument("-v", "--verbose", action="store_true")
    sub = ap.add_subparsers(dest="cmd")
    r = sub.add_parser("run", help="chạy agent (mặc định)")
    r.add_argument("--no-relay", action="store_true", help="không mở YouTube relay")
    r.add_argument("--no-update", action="store_true", help="không chạy yt-dlp -U")
    n = sub.add_parser("notify", help="gửi thông báo")
    n.add_argument("title")
    n.add_argument("text", nargs="?", default="")
    n.add_argument("--sound", choices=["chime", "alert", "none"], default="chime")
    n.add_argument("--priority", choices=["low", "normal", "high"], default="normal")
    n.add_argument("--duration", type=int, default=10)
    s = sub.add_parser("say", help="đọc câu bằng TTS Windows và gửi lên loa")
    s.add_argument("text")
    s.add_argument("--voice", help="một phần tên/ngôn ngữ giọng, vd 'An' hoặc 'vi-VN'")
    s.add_argument("--save", type=Path, help="chỉ lưu WAV ra file, không gửi")
    c = sub.add_parser("config", help="xem/sửa cấu hình")
    c.add_argument("--set-host", dest="set_host", metavar="HOST", help="IP[:port] loa, '' = tự tìm mDNS")
    c.add_argument("--token")
    c.add_argument("--relay-port", type=int)
    args, extra = ap.parse_known_args()
    if args.cmd is None:  # default subcommand: run (its options may come without the word "run")
        args = ap.parse_args([*(a for a in sys.argv[1:] if a not in extra), "run", *extra])
    elif extra:
        ap.error(f"unrecognized arguments: {' '.join(extra)}")
    logfile = None if sys.stderr else args.config.parent / "agent.log"  # pythonw: log to file
    if logfile:
        logfile.parent.mkdir(parents=True, exist_ok=True)
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO, filename=logfile, encoding="utf-8",
                        format="%(asctime)s %(levelname)s %(message)s", datefmt="%m-%d %H:%M:%S")

    cfg = load_config(args.config)
    if args.cmd == "config":
        for k, v in (("host", args.set_host), ("token", args.token), ("relay_port", args.relay_port)):
            if v is not None:
                cfg[k] = v
        save_config(args.config, cfg)
        print(args.config, json.dumps(cfg, indent=2), sep="\n")
        return
    if args.host:
        cfg["host"] = args.host

    if args.cmd == "run":
        try:
            asyncio.run(run(cfg, args))
        except KeyboardInterrupt:
            pass
    elif args.cmd == "notify":
        body = {"title": args.title, "text": args.text, "sound": args.sound,
                "priority": args.priority, "duration": args.duration}
        post(cfg, "/api/notify", json.dumps(body).encode(), "application/json")
    elif args.cmd == "say":
        wav = asyncio.run(tts_wav(args.text, args.voice))
        if args.save:
            args.save.write_bytes(wav)
            print(f"saved {len(wav)} bytes -> {args.save}")
        else:
            post(cfg, "/api/notify/audio?" + urllib.parse.urlencode({"title": args.text[:60]}), wav, "audio/wav")


if __name__ == "__main__":
    main()
