"""Is there a newer agent on GitHub Releases? (docs/firmware_v0.2.0.md, 2.1). Only tells; never downloads."""
import json
import logging
import urllib.request

from . import REPO, __version__

log = logging.getLogger("meowbox")
RELEASES_PAGE = f"https://github.com/{REPO}/releases"


def _num(v: str) -> tuple:
    out = []
    for part in v.lstrip("v").split(".")[:3]:
        digits = "".join(c for c in part if c.isdigit())
        out.append(int(digits or 0))
    return tuple(out + [0] * (3 - len(out)))


def latest_version(timeout: float = 8) -> str | None:
    """Tag of the latest (non-draft, non-prerelease) release, e.g. '0.2.1'; None on any error."""
    req = urllib.request.Request(f"https://api.github.com/repos/{REPO}/releases/latest",
                                 headers={"Accept": "application/vnd.github+json", "User-Agent": "MeowBoxAgent"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read()).get("tag_name", "").lstrip("v") or None
    except Exception as e:  # noqa: BLE001  (offline, rate limit, no release yet: stay quiet)
        log.debug("update check failed: %s", e)
        return None


def newer_than_running(v: str | None) -> bool:
    return bool(v) and _num(v) > _num(__version__)
