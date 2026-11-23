"""agent.json and the state shared between the asyncio core (its own thread) and the tray / dialogs."""
import json
import logging
import logging.handlers
import os
import socket
import threading
from dataclasses import dataclass, field
from pathlib import Path

from . import APP_DIR, CONFIG_PATH

log = logging.getLogger("meowbox")

DEFAULTS = {
    "speaker": "",      # name of the speaker to use (MeowBox-1A2B); "" = the first one found
    "host": "",         # IP[:port] typed by hand
    "last_host": "",    # IP[:port] of the last successful connection
    "token": "",        # notify token (Notifications page on the speaker)
    "relay_port": 8765,
    "paused": False,    # tray: pause sending stats
}


def load_config(path: Path = CONFIG_PATH) -> dict:
    cfg = dict(DEFAULTS)
    try:
        cfg.update(json.loads(path.read_text(encoding="utf-8")))
    except (FileNotFoundError, ValueError):
        pass
    return cfg


def save_config(cfg: dict, path: Path = CONFIG_PATH) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(cfg, indent=2, ensure_ascii=False), encoding="utf-8")
    os.replace(tmp, path)


def setup_logging(verbose: bool = False, to_file: bool = False) -> Path | None:
    """Console, or %APPDATA%\\MeowBox\\agent.log rotated at 1 MB x 3 (docs/firmware_v0.2.0.md, 2.5)."""
    fmt = logging.Formatter("%(asctime)s %(levelname)s %(message)s", "%m-%d %H:%M:%S")
    root = logging.getLogger()
    root.setLevel(logging.DEBUG if verbose else logging.INFO)
    for h in list(root.handlers):
        root.removeHandler(h)
    logfile = None
    if to_file:
        APP_DIR.mkdir(parents=True, exist_ok=True)
        logfile = APP_DIR / "agent.log"
        h = logging.handlers.RotatingFileHandler(logfile, maxBytes=1_000_000, backupCount=3, encoding="utf-8")
    else:
        h = logging.StreamHandler()
    h.setFormatter(fmt)
    root.addHandler(h)
    logging.getLogger("zeroconf").setLevel(logging.WARNING)
    return logfile


def pc_name() -> str:
    return os.environ.get("COMPUTERNAME") or socket.gethostname()


@dataclass
class AgentState:
    """What the tray shows. Written by the core thread, read by the UI thread; listeners run in the writer's thread."""
    connected: bool = False
    host: str = ""               # "ip:port" in use or being tried
    name: str = ""               # speaker name (MeowBox-1A2B)
    ver: str = ""                # speaker firmware version
    how: str = ""                # mdns | broadcast | last | manual
    searching: str = ""          # what the search is doing right now ("", "mdns", "broadcast", "ip 1.2.3.4")
    error: str = ""              # token | port | not_found | busy | proto_agent | proto_speaker | ""
    busy_with: str = ""          # PC holding the speaker when error == busy
    retry_in: int = 0            # seconds until the next attempt
    reconnects: int = 0
    relay_port: int = 0          # 0 = relay off
    relay_ok: bool | None = None  # last relay_test result
    new_version: str = ""        # newer agent release found on GitHub
    _lock: threading.Lock = field(default_factory=threading.Lock, repr=False)
    _listeners: list = field(default_factory=list, repr=False)

    def update(self, **kw) -> None:
        with self._lock:
            changed = {k: v for k, v in kw.items() if getattr(self, k) != v}
            for k, v in changed.items():
                setattr(self, k, v)
            listeners = list(self._listeners)
        if changed:
            for fn in listeners:
                try:
                    fn(self)
                except Exception:  # noqa: BLE001
                    log.exception("state listener failed")

    def on_change(self, fn) -> None:
        with self._lock:
            self._listeners.append(fn)

    def ip(self) -> str:
        return self.host.split(":")[0] if self.host else ""
