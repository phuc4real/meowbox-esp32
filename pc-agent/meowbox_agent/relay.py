"""YouTube relay: the speaker asks http://<pc>:8765/yt/stream?url=..., yt-dlp + ffmpeg turn it into MP3 128 kbps.

Only the connected speaker may use it (docs/firmware_v0.2.0.md, 2.3): any other client gets 403, so a PC on a shared
Wi-Fi cannot be used by others to run yt-dlp/ffmpeg on arbitrary URLs. /yt/ping answers the speaker's relay_test.
"""
import asyncio
import json
import logging
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import aiohttp
from aiohttp import web

from . import APP_DIR, FROZEN

log = logging.getLogger("meowbox")
_info_cache: dict[tuple, tuple[float, dict]] = {}
NOWIN = subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0


def tool(name: str) -> str:
    """ffmpeg / yt-dlp: PATH first, then the copy shipped with the installer (%LOCALAPPDATA%\\MeowBox\\bin)."""
    found = shutil.which(name)
    if found:
        return found
    for d in (Path(os.environ.get("LOCALAPPDATA", "")) / "MeowBox" / "bin",
              Path(sys.executable).parent / "bin" if FROZEN else Path(__file__).parent.parent / "bin"):
        p = d / f"{name}.exe"
        if p.exists():
            return str(p)
    return name


def ensure_tools() -> None:
    """Installed build: copy the bundled yt-dlp.exe to %LOCALAPPDATA%/MeowBox/bin once, where `yt-dlp -U` can
    update it (Program Files is read-only for the user). ffmpeg is used straight from the install folder."""
    if not FROZEN:
        return
    src = Path(sys.executable).parent / "bin" / "yt-dlp.exe"
    dst = Path(os.environ.get("LOCALAPPDATA", "")) / "MeowBox" / "bin" / "yt-dlp.exe"
    if src.exists() and not dst.exists():
        try:
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
            log.info("yt-dlp copied to %s", dst)
        except OSError as e:
            log.warning("could not copy yt-dlp: %s", e)


def ytdlp_args() -> list[str]:
    return [tool("yt-dlp"), "--no-warnings", "--quiet", "--no-progress"]


async def ytjson(url: str, single: bool) -> dict:
    key = (url, single)
    hit = _info_cache.get(key)
    if hit and time.monotonic() - hit[0] < 1800:
        return hit[1]
    args = [*ytdlp_args(), "-J", "--flat-playlist", *(["--no-playlist"] if single else []), url]
    p = await asyncio.create_subprocess_exec(*args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, creationflags=NOWIN)
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
           "duration": info.get("duration"), "is_live": bool(info.get("is_live")),
           "uploader": info.get("uploader") or info.get("channel")}
    if res["type"] == "playlist":
        res["entries"] = [{"index": i, "title": e.get("title"), "duration": e.get("duration"), "url": entry_url(e)}
                          for i, e in enumerate(info.get("entries") or [])]
    return web.json_response(res)


def kill_tree(p: asyncio.subprocess.Process) -> None:
    if p.returncode is None:  # yt-dlp.exe (PyInstaller) has a child process, so kill the whole tree
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(p.pid)], capture_output=True, creationflags=NOWIN)


RUNNING: set[asyncio.subprocess.Process] = set()  # killed on exit (tray "Quit")


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
    ytdlp = await asyncio.create_subprocess_exec(
        *ytdlp_args(), "--load-info-json", info_path, "-f", "bestaudio/best", "-o", "-",
        stdout=wfd, stderr=subprocess.DEVNULL, creationflags=NOWIN)
    ffmpeg = await asyncio.create_subprocess_exec(
        tool("ffmpeg"), "-hide_banner", "-loglevel", "error", "-i", "pipe:0", "-vn", "-c:a", "libmp3lame",
        "-b:a", "128k", "-ar", "44100", "-ac", "2", "-f", "mp3", "pipe:1",
        stdin=rfd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, creationflags=NOWIN)
    os.close(rfd)
    os.close(wfd)
    RUNNING.update((ytdlp, ffmpeg))
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
        RUNNING.difference_update((ytdlp, ffmpeg))
        Path(info_path).unlink(missing_ok=True)
        log.info("relay stop: %s, %d KB sent", title, sent // 1024)
    if resp is None:
        raise web.HTTPBadGateway(text="no audio from yt-dlp/ffmpeg")
    return resp


async def yt_ping(req: web.Request) -> web.Response:
    return web.json_response({"ok": True})


def kill_all() -> None:
    for p in list(RUNNING):
        kill_tree(p)


async def start_relay(port: int, allowed) -> web.AppRunner:
    """allowed() -> set of IPs that may use the relay (the connected speaker, plus one being tested)."""

    @web.middleware
    async def only_speaker(req: web.Request, handler):
        if req.remote not in allowed():
            log.warning("relay: refused %s %s (not the connected speaker)", req.remote, req.path)
            raise web.HTTPForbidden(text="MeowBox relay: only the connected speaker may use it")
        return await handler(req)

    app = web.Application(middlewares=[only_speaker])
    app.add_routes([web.get("/yt/info", yt_info), web.get("/yt/stream", yt_stream), web.get("/yt/ping", yt_ping)])
    runner = web.AppRunner(app, access_log=None)
    await runner.setup()
    await web.TCPSite(runner, "0.0.0.0", port).start()
    log.info("YouTube relay on http://0.0.0.0:%d/yt/stream?url=...", port)
    return runner


async def update_ytdlp() -> None:
    """yt-dlp -U at most once per day; failures (e.g. winget/pip installs) are ignored."""
    stamp = APP_DIR / "ytdlp_update.stamp"
    try:
        if time.time() - stamp.stat().st_mtime < 86400:
            return
    except FileNotFoundError:
        pass
    stamp.parent.mkdir(parents=True, exist_ok=True)
    stamp.touch()
    try:
        p = await asyncio.create_subprocess_exec(tool("yt-dlp"), "-U", stdout=subprocess.PIPE,
                                                 stderr=subprocess.STDOUT, creationflags=NOWIN)
        out, _ = await asyncio.wait_for(p.communicate(), 180)
        log.info("yt-dlp -U: %s", out.decode(errors="replace").strip().splitlines()[-1:])
    except Exception as e:  # noqa: BLE001
        log.info("yt-dlp -U skipped: %s", e)
