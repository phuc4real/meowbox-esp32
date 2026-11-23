"""yt-dlp lives outside the agent (docs/firmware_v0.2.0.md, 2.10): the zipapp build `yt-dlp` (~3 MB, pure Python)
from github.com/yt-dlp/yt-dlp releases, kept in %LOCALAPPDATA%\\MeowBox\\bin\\yt-dlp.pyz. The installer fetches it
(`meowbox tools update`), the agent fetches it on first run if missing, and Settings (tray) checks/updates it.
YouTube changes often, so yt-dlp must be updatable without a new agent release.

It runs in a child process: `meowbox.exe ytdlp <args>` (installed) or `python yt-dlp.pyz <args>` (from source),
so an update takes effect on the next video without restarting the agent.
"""
import hashlib
import json
import logging
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import urllib.request
import zipfile
from pathlib import Path

from . import FROZEN

log = logging.getLogger("meowbox")
BIN = Path(os.environ.get("LOCALAPPDATA", Path.home())) / "MeowBox" / "bin"
YTDLP = BIN / "yt-dlp.pyz"
YTDLP_REPO = "yt-dlp/yt-dlp"
UA = {"User-Agent": "MeowBoxAgent", "Accept": "application/vnd.github+json"}


def ytdlp_version() -> str | None:
    """Version of the installed zip (reads yt_dlp/version.py inside it), None if missing/broken."""
    try:
        with zipfile.ZipFile(YTDLP) as z:
            src = z.read("yt_dlp/version.py").decode()
        for line in src.splitlines():
            if line.startswith("__version__"):
                return line.split("=", 1)[1].strip().strip("'\"")
    except (OSError, KeyError, zipfile.BadZipFile):
        pass
    return None


def ytdlp_latest(timeout: float = 10) -> str | None:
    req = urllib.request.Request(f"https://api.github.com/repos/{YTDLP_REPO}/releases/latest", headers=UA)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read()).get("tag_name") or None
    except Exception as e:  # noqa: BLE001
        log.info("yt-dlp latest check failed: %s", e)
        return None


def _get(url: str, timeout: float = 60) -> bytes:
    with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "MeowBoxAgent"}), timeout=timeout) as r:
        return r.read()


def ytdlp_update(progress=None) -> str:
    """Download the latest yt-dlp zipapp, check it against the release's SHA2-256SUMS, swap it in atomically.
    Returns the installed version; raises RuntimeError with a short reason on failure."""
    base = f"https://github.com/{YTDLP_REPO}/releases/latest/download/"
    if progress:
        progress("sums")
    try:
        sums = _get(base + "SHA2-256SUMS", 20).decode()
    except OSError as e:
        raise RuntimeError(f"cannot reach GitHub ({e})") from None
    want = next((ln.split()[0] for ln in sums.splitlines() if ln.split()[-1:] == ["yt-dlp"]), None)
    if not want:
        raise RuntimeError("no checksum for the yt-dlp zipapp in this release")
    if progress:
        progress("download")
    try:
        data = _get(base + "yt-dlp", 120)
    except OSError as e:
        raise RuntimeError(f"download failed ({e})") from None
    if hashlib.sha256(data).hexdigest() != want:
        raise RuntimeError("checksum mismatch, not installed")
    BIN.mkdir(parents=True, exist_ok=True)
    tmp = YTDLP.with_suffix(".tmp")
    tmp.write_bytes(data)
    os.replace(tmp, YTDLP)
    v = ytdlp_version() or "?"
    log.info("yt-dlp %s installed (%d KB)", v, len(data) // 1024)
    return v


def maintain_ytdlp(daily_check: bool = True) -> None:
    """Agent start: install yt-dlp if missing (the installer may have been offline); with daily_check, update it at
    most once a day when GitHub has a newer release. Errors are logged, never raised."""
    stamp = BIN / "yt-dlp.checked"
    try:
        if not YTDLP.exists() or ytdlp_version() is None:
            ytdlp_update()
            stamp.touch()
            return
        if not daily_check:
            return
        import time

        if stamp.exists() and time.time() - stamp.stat().st_mtime < 86400:
            return
        latest = ytdlp_latest()
        stamp.touch()
        if latest and latest != ytdlp_version():
            ytdlp_update()
    except Exception as e:  # noqa: BLE001
        log.warning("yt-dlp maintenance: %s", e)


def ytdlp_command() -> list[str]:
    """argv prefix that runs yt-dlp in a child process (no console window when started by the agent)."""
    if FROZEN:
        console = Path(sys.executable).with_name("meowbox.exe")  # the windowed exe has no stdout to pipe
        return [str(console), "ytdlp"]
    return [sys.executable, str(YTDLP)]


def run_ytdlp_inprocess(argv: list[str]) -> int:
    """`meowbox ytdlp ...`: import yt_dlp from the zip and run its CLI (installed build only)."""
    if not YTDLP.exists():
        print("yt-dlp is not installed: meowbox tools update", file=sys.stderr)
        return 2
    sys.path.insert(0, str(YTDLP))
    import yt_dlp  # noqa: PLC0415  (from the zip, on purpose)

    try:
        yt_dlp.main(argv)
    except SystemExit as e:
        return int(e.code or 0) if not isinstance(e.code, str) else 1
    return 0


# ---------------------------------------------------------------- ffmpeg (only for YouTube live)
# yt-dlp will not download live HLS without ffmpeg. The agent fetches an LGPL ffmpeg the first time a live stream is
# played (or from Settings): BtbN FFmpeg-Builds, "win64-lgpl-shared" (~75 MB zip: ffmpeg.exe + its DLLs; ffprobe and
# ffplay are left out), checked against the release's checksums.sha256. Live audio is only copied (AAC in MPEG-TS),
# never re-encoded, so no GPL/nonfree encoder is needed.

FFMPEG_DIR = BIN / "ffmpeg"
FFMPEG_REPO = "BtbN/FFmpeg-Builds"
_ff_lock = threading.Lock()
_ff_state = {"busy": False, "error": "", "pct": 0}


def ffmpeg_path() -> str | None:
    """ffmpeg in PATH first (the user's own), else the one the agent downloaded."""
    found = shutil.which("ffmpeg")
    if found:
        return found
    p = FFMPEG_DIR / "ffmpeg.exe"
    return str(p) if p.exists() else None


def ffmpeg_version() -> str | None:
    p = ffmpeg_path()
    if not p:
        return None
    try:
        out = subprocess.run([p, "-version"], capture_output=True, text=True, timeout=10,
                      creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)).stdout
        m = re.match(r"ffmpeg version (\S+)", out)
        return m.group(1) if m else "?"
    except Exception:  # noqa: BLE001
        return None


def ffmpeg_status() -> dict:
    """busy/pct/error of a download; 'own' = the ffmpeg in use is the one the agent downloaded (not the user's)."""
    path = ffmpeg_path()
    return {**_ff_state, "path": path, "own": bool(path) and Path(path).parent == FFMPEG_DIR}


def _ffmpeg_asset() -> tuple[str, str, int]:
    """(name, download url, size) of the newest release build: ffmpeg-nX.Y-latest-win64-lgpl-shared-X.Y.zip."""
    req = urllib.request.Request(f"https://api.github.com/repos/{FFMPEG_REPO}/releases/latest", headers=UA)
    with urllib.request.urlopen(req, timeout=15) as r:
        assets = json.loads(r.read())["assets"]
    pat = re.compile(r"ffmpeg-n(\d+)\.(\d+)-latest-win64-lgpl-shared-[\d.]+\.zip$")
    best = max((a for a in assets if pat.match(a["name"])),
               key=lambda a: tuple(int(x) for x in pat.match(a["name"]).groups()), default=None)
    if not best:
        raise RuntimeError("no win64 LGPL shared build in the latest release")
    return best["name"], best["browser_download_url"], best["size"]


def ffmpeg_install(progress=None) -> str:
    """Download + verify + unpack ffmpeg into %LOCALAPPDATA%/MeowBox/bin/ffmpeg. One download at a time: a second
    caller waits for the first. Returns the version; raises RuntimeError with a short reason."""
    with _ff_lock:
        if (FFMPEG_DIR / "ffmpeg.exe").exists() and _ff_state.get("fresh"):
            return ffmpeg_version() or "?"
        _ff_state.update(busy=True, error="", pct=0)
        try:
            name, url, size = _ffmpeg_asset()
            sums = _get(f"https://github.com/{FFMPEG_REPO}/releases/latest/download/checksums.sha256", 30).decode()
            want = next((ln.split()[0] for ln in sums.splitlines() if ln.strip().endswith(name)), None)
            if not want:
                raise RuntimeError(f"no checksum for {name}")
            log.info("ffmpeg: downloading %s (%d MB)", name, size // 1048576)
            h = hashlib.sha256()
            with tempfile.TemporaryDirectory() as td:
                zpath = Path(td) / name
                with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "MeowBoxAgent"}),
                                            timeout=60) as r, open(zpath, "wb") as f:
                    got = 0
                    while chunk := r.read(1 << 16):
                        f.write(chunk)
                        h.update(chunk)
                        got += len(chunk)
                        _ff_state["pct"] = got * 100 // max(size, 1)
                        if progress:
                            progress(_ff_state["pct"])
                if h.hexdigest() != want:
                    raise RuntimeError("checksum mismatch, not installed")
                new = FFMPEG_DIR.with_name("ffmpeg.new")
                shutil.rmtree(new, ignore_errors=True)
                new.mkdir(parents=True)
                with zipfile.ZipFile(zpath) as z:
                    for m in z.infolist():
                        base = m.filename.rsplit("/", 1)[-1]
                        keep = ("/bin/" in m.filename and (base == "ffmpeg.exe" or base.endswith(".dll"))) \
                            or base == "LICENSE.txt"
                        if keep and base:
                            (new / base).write_bytes(z.read(m))
                if not (new / "ffmpeg.exe").exists():
                    raise RuntimeError("ffmpeg.exe not found in the archive")
                shutil.rmtree(FFMPEG_DIR, ignore_errors=True)
                os.replace(new, FFMPEG_DIR)
            _ff_state["fresh"] = True
            v = ffmpeg_version() or "?"
            log.info("ffmpeg %s installed in %s", v, FFMPEG_DIR)
            return v
        except Exception as e:  # noqa: BLE001
            _ff_state["error"] = str(e)[:120]
            log.warning("ffmpeg install failed: %s", e)
            raise RuntimeError(str(e)) from None
        finally:
            _ff_state["busy"] = False


def ffmpeg_install_background(on_done=None) -> bool:
    """Start an install in a thread unless one is running; True if a download is (now) in progress."""
    if _ff_state["busy"]:
        return True

    def run():
        try:
            ffmpeg_install()
        except RuntimeError:
            pass
        if on_done:
            on_done()

    threading.Thread(target=run, name="ffmpeg-install", daemon=True).start()
    return True
