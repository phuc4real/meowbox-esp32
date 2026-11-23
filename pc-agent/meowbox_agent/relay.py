"""YouTube relay: the speaker asks http://<pc>:8765/yt/stream?url=..., yt-dlp downloads the AAC audio and the relay
sends it as ADTS (m4a container stripped, no re-encoding, no ffmpeg). Live streams need ffmpeg (yt-dlp will not
download live HLS without it): the agent downloads an LGPL build the first time, and the AAC is only copied.

Only the connected speaker may use it (docs/firmware_v0.2.0.md, 2.3): any other client gets 403, so a PC on a shared
Wi-Fi cannot be used by others to run yt-dlp/ffmpeg on arbitrary URLs. /yt/ping answers the speaker's relay_test.
"""
import asyncio
import json
import logging
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import aiohttp
from aiohttp import web

from .mp4adts import Mp4ToAdts, Unsupported
from .tools import ffmpeg_install_background, ffmpeg_path, ffmpeg_status, ytdlp_command

log = logging.getLogger("meowbox")
_info_cache: dict[tuple, tuple[float, dict]] = {}
NOWIN = subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0


NOTIFY: list = []  # callbacks(text) for the tray, e.g. "downloading ffmpeg"


def _notify(text: str) -> None:
    for fn in NOTIFY:
        try:
            fn(text)
        except Exception:  # noqa: BLE001
            pass


def ytdlp_args() -> list[str]:
    return [*ytdlp_command(), "--no-warnings", "--quiet", "--no-progress"]


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


def pick_aac(info: dict) -> dict | None:
    """Best AAC-in-MP4 audio-only format (YouTube itag 140 is ~128 kbps), not above 192 kbps."""
    fmts = [f for f in info.get("formats") or []
            if f.get("vcodec") == "none" and str(f.get("acodec", "")).startswith("mp4a")
            and f.get("protocol") in ("https", "http")]
    good = [f for f in fmts if (f.get("abr") or 0) <= 192] or fmts
    return max(good, key=lambda f: f.get("abr") or 0) if good else None


def pick_hls(info: dict) -> dict | None:
    """Live: YouTube's audio-only HLS formats (233 ~48 kbps, 234 ~128 kbps; acodec reported as None): the best one."""
    fmts = [f for f in info.get("formats") or [] if str(f.get("protocol", "")).startswith("m3u8")
            and f.get("vcodec") == "none" and f.get("acodec") != "none"]
    known = {"233": 48, "234": 128}  # YouTube gives no bitrate for these two
    return max(fmts, key=lambda f: f.get("tbr") or f.get("abr") or known.get(str(f.get("format_id")), 0)) if fmts else None


def base_headers(info: dict, title: str, i, count, ctype: str, br: int | None) -> dict:
    h = {"Content-Type": ctype, "Cache-Control": "no-cache", "icy-name": title,
         "X-Is-Live": str(bool(info.get("is_live"))).lower()}
    if br:
        h["icy-br"] = str(br)
    if info.get("duration"):
        h["X-Duration"] = str(round(info["duration"]))
    if i is not None:
        h.update({"X-Playlist-Index": str(i), "X-Playlist-Count": str(count)})
    return h


async def pipe(req, procs: list, src, convert, headers: dict, stat: dict) -> web.StreamResponse | None:
    """Read src (a process stdout), convert, send; headers go out with the first audio (so errors can still be 502)."""
    resp = None
    RUNNING.update(procs)
    try:
        while chunk := await src.read(16384):
            frames = convert(chunk)
            if not frames:
                continue
            if resp is None:
                resp = web.StreamResponse(headers=headers)
                resp.enable_chunked_encoding()
                await resp.prepare(req)
            data = b"".join(frames)
            await resp.write(data)
            stat["sent"] += len(data)
    finally:
        for p in procs:
            kill_tree(p)
        RUNNING.difference_update(procs)
    return resp


async def need_ffmpeg() -> str | None:
    """ffmpeg for a live stream: the one in PATH / already downloaded, else start the one-time download and wait a
    few seconds (the speaker reconnects by itself, so a slow download just takes a few of its retries)."""
    p = ffmpeg_path()
    if p:
        return p
    if not ffmpeg_status()["busy"]:
        _notify("Đang tải ffmpeg cho YouTube live (một lần, khoảng 75 MB)…")
        ffmpeg_install_background(lambda: _notify("Đã tải xong ffmpeg: YouTube live phát được." if ffmpeg_path()
                                                  else "Không tải được ffmpeg: " + ffmpeg_status()["error"]))
    for _ in range(12):
        await asyncio.sleep(0.5)
        if ffmpeg_path():
            return ffmpeg_path()
    return None


async def yt_stream(req: web.Request) -> web.StreamResponse:
    """yt-dlp downloads (it handles YouTube's throttling, retries and link refresh) and writes to stdout; the relay
    only changes the container. Video: m4a -> ADTS (Mp4ToAdts), no ffmpeg. Live: yt-dlp + ffmpeg copy the AAC into
    MPEG-TS (ffmpeg downloaded on first use). A video without AAC (rare): ffmpeg encodes AAC."""
    url = req.query.get("url") or ""
    if not url:
        raise web.HTTPBadRequest(text="missing url")
    index = int(req.query["index"]) if req.query.get("index", "").isdigit() else None
    info, i, count = await resolve_video(url, index)
    title = re.sub(r"[\r\n]+", " ", info.get("fulltitle") or info.get("title") or "YouTube")
    live = bool(info.get("is_live"))
    fmt = pick_hls(info) if live else pick_aac(info)
    ff = None
    if live or not fmt:
        ff = await need_ffmpeg()
        if not ff:
            raise web.HTTPServiceUnavailable(text="downloading ffmpeg (first live stream), the speaker will retry")
    stat, how, resp = {"sent": 0}, "", None
    fd, info_path = tempfile.mkstemp(suffix=".info.json")
    with os.fdopen(fd, "w", encoding="utf-8") as fh:
        json.dump(info, fh)
    try:
        if live:
            how = f"live itag {fmt.get('format_id') if fmt else 'best'}, AAC copied into MPEG-TS"
            log.info("relay start: %s (%s, %s)", title, how, req.remote)
            yt = await asyncio.create_subprocess_exec(
                *ytdlp_args(), "--ffmpeg-location", ff, "--load-info-json", info_path,
                "-f", fmt["format_id"] if fmt else "bestaudio/best", "-o", "-",
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, creationflags=NOWIN)
            resp = await pipe(req, [yt], yt.stdout, lambda c: [c],
                              base_headers(info, title, i, count, "video/mp2t", None), stat)
        elif fmt:
            br = round(fmt.get("abr") or 128)
            how = f"AAC itag {fmt.get('format_id')}, {br} kbps"
            log.info("relay start: %s (%s, %s)", title, how, req.remote)
            yt = await asyncio.create_subprocess_exec(
                *ytdlp_args(), "--load-info-json", info_path, "-f", fmt["format_id"], "-o", "-",
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, creationflags=NOWIN)
            resp = await pipe(req, [yt], yt.stdout, Mp4ToAdts().feed,
                              base_headers(info, title, i, count, "audio/aac", br), stat)
        else:
            how = "no AAC: ffmpeg -> AAC 128k"
            log.info("relay start: %s (%s, %s)", title, how, req.remote)
            rfd, wfd = os.pipe()
            yt = await asyncio.create_subprocess_exec(
                *ytdlp_args(), "--load-info-json", info_path, "-f", "bestaudio/best", "-o", "-",
                stdout=wfd, stderr=subprocess.DEVNULL, creationflags=NOWIN)
            enc = await asyncio.create_subprocess_exec(
                ff, "-hide_banner", "-loglevel", "error", "-i", "pipe:0", "-vn", "-c:a", "aac",
                "-b:a", "128k", "-ar", "44100", "-ac", "2", "-f", "adts", "pipe:1",
                stdin=rfd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, creationflags=NOWIN)
            os.close(rfd)
            os.close(wfd)
            resp = await pipe(req, [yt, enc], enc.stdout, lambda c: [c],
                              base_headers(info, title, i, count, "audio/aac", 128), stat)
    except (ConnectionError, aiohttp.ClientConnectionResetError):
        log.info("relay client gone")
    except Unsupported as e:
        log.warning("relay: %s", e)
        if resp is None:
            raise web.HTTPBadGateway(text=f"cannot convert this video: {e}") from None
    finally:
        Path(info_path).unlink(missing_ok=True)
        log.info("relay stop: %s (%s), %d KB sent", title, how, stat["sent"] // 1024)
    if resp is None:
        raise web.HTTPBadGateway(text="no audio from yt-dlp")
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
