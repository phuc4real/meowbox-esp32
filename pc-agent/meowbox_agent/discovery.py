"""Reach the MeowBox speaker at the IP the user typed (docs/firmware_v0.2.0.md, 2.2).

No discovery on the LAN (no mDNS, no UDP broadcast): those get flagged or filtered on managed networks and PCs. The user
types the IP shown on the speaker's System screen; find_speaker() checks the typed IP and the last working one with
GET /api/ping.
"""
import ipaddress
import json
import logging
import socket
import urllib.error
import urllib.request
from dataclasses import dataclass

log = logging.getLogger("meowbox")


@dataclass
class Speaker:
    name: str
    host: str          # "ip" or "ip:port"
    ver: str = ""
    proto: int = 0
    how: str = ""      # ping | last | manual

    @property
    def ip(self) -> str:
        return self.host.split(":")[0]


def ping(host: str, timeout: float = 2.0) -> Speaker | None:
    """GET http://host/api/ping: is it a MeowBox, and which one? None if not (or no answer)."""
    try:
        with urllib.request.urlopen(f"http://{host}/api/ping", timeout=timeout) as r:
            j = json.loads(r.read())
        if j.get("name", "").startswith("MeowBox"):
            return Speaker(j["name"], host, j.get("ver", ""), int(j.get("proto", 0)), "ping")
    except (OSError, ValueError, urllib.error.URLError):
        pass
    return None


def ping_detail(host: str, timeout: float = 2.5) -> tuple[Speaker | None, str]:
    """Like ping() but says why it failed: '' ok | not_meowbox | no_answer | bad_address."""
    try:
        ipaddress.ip_address(host.split(":")[0])
    except ValueError:
        return None, "bad_address"
    try:
        with urllib.request.urlopen(f"http://{host}/api/ping", timeout=timeout) as r:
            j = json.loads(r.read())
        if j.get("name", "").startswith("MeowBox"):
            return Speaker(j["name"], host, j.get("ver", ""), int(j.get("proto", 0)), "ping"), ""
        return None, "not_meowbox"
    except urllib.error.HTTPError:
        return None, "not_meowbox"   # something answers HTTP there, but it is not a MeowBox (or an old firmware)
    except ValueError:
        return None, "not_meowbox"
    except OSError:
        return None, "no_answer"


def local_networks() -> list[ipaddress.IPv4Interface]:
    """IPv4 interfaces of this PC (no loopback / link-local)."""
    out = []
    try:
        import psutil

        for addrs in psutil.net_if_addrs().values():
            for a in addrs:
                if a.family == socket.AF_INET and a.netmask and not a.address.startswith(("127.", "169.254.")):
                    out.append(ipaddress.IPv4Interface(f"{a.address}/{a.netmask}"))
    except Exception:  # noqa: BLE001
        pass
    return out


def same_subnet(ip: str) -> bool:
    try:
        addr = ipaddress.IPv4Address(ip.split(":")[0])
    except ValueError:
        return False
    return any(addr in n.network for n in local_networks())


def find_speaker(cfg: dict, report=None) -> Speaker | None:
    """The speaker at the typed IP, else at the last working one, if it answers (and has the name in cfg['speaker'], if
    set). The typed IP goes first so a new one wins over a stale last_host. report(step) says which IP is tried."""
    want = cfg.get("speaker") or ""
    tried = set()
    for key, how in (("host", "manual"), ("last_host", "last")):
        h = cfg.get(key)
        if h and h not in tried:
            tried.add(h)
            if report:
                report(f"ip {h}")
            sp = ping(h)
            if sp and (not want or sp.name == want):
                sp.how = how
                return sp
    return None


def explain_failure(host: str) -> str:
    """Best guess why a typed IP does not answer: other_network | no_answer."""
    if host and not same_subnet(host):
        return "other_network"
    return "no_answer"
