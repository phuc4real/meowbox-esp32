"""Stats + media -> speaker over WebSocket, media commands back, YouTube relay. Runs in its own thread (Core) so the
tray can drive it; the CLI `run` command uses the same class without a tray."""
import asyncio
import functools
import json
import logging
import os
import re
import shutil
import subprocess
import threading
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone

import aiohttp
import psutil

from pathlib import Path

from . import CONFIG_PATH, PROTO, __version__
from .coding import DEFAULT_PORT as CODING_PORT, Tracker, start_server as start_coding
from .config import AgentState, pc_name, save_config
from .discovery import find_speaker
from .relay import NOWIN, kill_all, start_relay
from .tools import maintain_ytdlp

log = logging.getLogger("meowbox")
compact = functools.partial(json.dumps, ensure_ascii=False, separators=(",", ":"))

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
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, creationflags=NOWIN)
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


# ---------------------------------------------------------------- HTTP to the speaker (notify, say, wifi-setup)


def speaker_request(host: str, path: str, token: str = "", body: bytes | None = None,
                    ctype: str = "application/json", timeout: float = 15) -> tuple[int, str]:
    req = urllib.request.Request(f"http://{host}{path}", data=body, method="POST" if body is not None else "GET",
                                 headers={"Content-Type": ctype, **({"Authorization": f"Bearer {token}"} if token else {})})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read().decode(errors="replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode(errors="replace")


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


# ---------------------------------------------------------------- the link


class Core:
    """Owns the asyncio loop (own thread): relay server + WebSocket link with reconnect. Thread-safe controls."""

    def __init__(self, cfg: dict, state: AgentState, relay: bool = True, ytdlp_update: bool = True,
                 cfg_path: Path = CONFIG_PATH):
        self.cfg, self.state, self.cfg_path = cfg, state, cfg_path
        self.relay_wanted, self.ytdlp_update = relay, ytdlp_update
        self.loop: asyncio.AbstractEventLoop | None = None
        self.thread: threading.Thread | None = None
        self._wake: asyncio.Event | None = None
        self._take = False
        self._ws = None
        self._stopping = False
        self.extra_allowed: set[str] = set()  # IP being tested by the connect dialog (relay_test)
        self.coding = Tracker()  # Meow Coding: Claude Code sessions, fed by its hooks

    # -- thread control
    def start(self) -> None:
        self.thread = threading.Thread(target=self._thread, name="meowbox-core", daemon=True)
        self.thread.start()

    def run_forever(self) -> None:  # CLI: run in this thread
        self._thread()

    def _thread(self) -> None:
        self.loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self.loop)
        try:
            self.loop.run_until_complete(self._main())
        except asyncio.CancelledError:
            pass
        finally:
            kill_all()

    def _call(self, fn) -> None:
        if self.loop and self.loop.is_running():
            self.loop.call_soon_threadsafe(fn)

    def reconnect_now(self, take: bool = False) -> None:
        """Skip the wait and search again (tray "Search now" / "Take over", or after the dialog saved a speaker)."""
        def go():
            self._take = take or self._take
            if self._ws is not None and not self._ws.closed:
                asyncio.ensure_future(self._ws.close())
            if self._wake:
                self._wake.set()
        self._call(go)

    def stop(self) -> None:
        self._stopping = True

        def go():
            for t in asyncio.all_tasks(self.loop):
                t.cancel()
        self._call(go)
        kill_all()
        if self.thread:
            self.thread.join(5)

    # -- asyncio side
    def _allowed(self) -> set[str]:
        ips = set(self.extra_allowed)
        if self.state.connected and self.state.ip():
            ips.add(self.state.ip())
        return ips

    async def _main(self) -> None:
        self._wake = asyncio.Event()
        cport = int(self.cfg.get("coding_port") or CODING_PORT)
        try:
            await start_coding(self.coding, cport)
        except OSError as e:
            log.error("Meow Coding port %d busy: %s", cport, e)
        port = int(self.cfg.get("relay_port") or 8765) if self.relay_wanted else 0
        if port:
            try:
                await start_relay(port, self._allowed)
                self.state.update(relay_port=port)
            except OSError as e:
                log.error("relay port %d busy: %s", port, e)
                self.state.update(error="port", relay_port=0)
                port = 0
            if port and self.ytdlp_update:
                asyncio.create_task(asyncio.to_thread(maintain_ytdlp, bool(self.cfg.get("auto_update", True))))
        await self._link_loop(port)

    async def _sleep(self, secs: int) -> None:
        self._wake.clear()
        for left in range(secs, 0, -1):
            self.state.update(retry_in=left)
            try:
                await asyncio.wait_for(self._wake.wait(), 1)
                break
            except asyncio.TimeoutError:
                pass
        self.state.update(retry_in=0)

    async def _link_loop(self, relay_port: int) -> None:
        stats, media, backoff = Stats(), Media(), 1
        async with aiohttp.ClientSession() as http:
            while not self._stopping:
                self.state.update(connected=False, searching="start")
                sp = await asyncio.to_thread(find_speaker, self.cfg, lambda s: self.state.update(searching=s))
                self.state.update(searching="")
                wait = backoff
                if not sp:
                    self.state.update(error="not_found", host="")
                    log.warning("speaker %s not found, retry in %ss", self.cfg.get("speaker") or "", backoff)
                else:
                    self.state.update(host=sp.host, name=sp.name, ver=sp.ver, how=sp.how)
                    if sp.proto < 1:  # firmware before 0.2.0 has no protocol number
                        log.info("speaker %s has an old firmware (proto %s)", sp.name, sp.proto)
                    try:
                        async with http.ws_connect(f"ws://{sp.host}/ws/pc", heartbeat=15) as ws:
                            self._ws = ws
                            wait = await self._session(ws, http, stats, media, relay_port, sp)
                            if wait == 0:
                                backoff = 1
                    except (aiohttp.ClientError, OSError, asyncio.TimeoutError) as e:
                        log.warning("connection to %s failed: %s", sp.host, e)
                        self.state.update(error="not_found")
                    finally:
                        self._ws = None
                    self.state.update(connected=False, reconnects=self.state.reconnects + 1)
                if self._stopping:
                    break
                await self._sleep(wait or backoff)
                backoff = min(backoff * 2, 30)

    async def _session(self, ws, http, stats: Stats, media: Media, relay_port: int, sp) -> int:
        """One connection. Returns seconds to wait before the next attempt (0 = normal reconnect backoff)."""
        hello = {"t": "hello", "name": pc_name(), "ver": __version__, "proto": PROTO}
        if relay_port:
            ip = ws.get_extra_info("sockname")[0]
            hello["relay"] = f"http://{ip}:{relay_port}"
        if self._take:
            hello["take"] = True
            self._take = False
        await ws.send_str(compact(hello))
        # no "welcome" message: connected unless a busy/proto answer comes back right away
        self.state.update(connected=True, error="", busy_with="")
        remember(self.cfg, sp, self.cfg_path)
        log.info("connected to %s (%s, %s)", sp.name, sp.host, sp.how)

        async def sender():
            last, last_t = None, 0.0
            code_last, code_t = None, 0.0
            while True:
                await asyncio.to_thread(self.coding.refresh_usage)
                cs, lim = self.coding.snapshot(), self.coding.limits_msg()
                ckey = ([{k: v for k, v in x.items() if k != "for"} for x in cs], lim)
                if ckey != code_last or time.monotonic() - code_t >= 30:
                    msg = {"t": "coding", "s": cs}
                    if lim:
                        msg["lim"] = lim
                    await ws.send_str(compact(msg))
                    code_last, code_t = ckey, time.monotonic()
                if not self.cfg.get("paused"):
                    await ws.send_str(compact(await stats.sample(http)))
                m = await media.read()
                key = {k: v for k, v in m.items() if k != "pos"}
                if key != last or time.monotonic() - last_t >= 5:
                    await ws.send_str(compact(m))
                    last, last_t = key, time.monotonic()
                await asyncio.sleep(1)

        task = asyncio.create_task(sender())
        wait = 0
        try:
            async for msg in ws:
                if msg.type != aiohttp.WSMsgType.TEXT:
                    continue
                try:
                    data = json.loads(msg.data)
                except ValueError:
                    continue
                t = data.get("t")
                if t == "busy":   # another PC holds the speaker (or just took it over from us)
                    self.state.update(connected=False, error="busy", busy_with=data.get("pc", ""))
                    log.info("speaker busy with %s", data.get("pc"))
                    return 60
                if t == "proto":  # this agent is too old for the speaker's firmware
                    self.state.update(connected=False, error="proto_agent")
                    log.warning("speaker needs agent protocol %s (have %s)", data.get("need"), PROTO)
                    return 300
                if t == "media_cmd":
                    await media.command(data.get("cmd", ""))
                    await ws.send_str(compact(await media.read()))
        finally:
            task.cancel()
        return wait


def remember(cfg: dict, sp, path: Path = CONFIG_PATH) -> None:
    """After a successful connection: keep the IP for next time and fill the speaker name if empty."""
    changed = False
    if cfg.get("last_host") != sp.host:
        cfg["last_host"] = sp.host
        changed = True
    if not cfg.get("speaker"):
        cfg["speaker"] = sp.name
        changed = True
    if changed:
        save_config(cfg, path)
