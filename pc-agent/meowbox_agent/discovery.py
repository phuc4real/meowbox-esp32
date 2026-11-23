"""Find MeowBox speakers on the LAN (docs/firmware_v0.2.0.md, 2.2 and 2.8).

Order used by find_speaker(): last IP / IP typed by hand (checked with /api/ping), mDNS _meowbox._tcp, UDP broadcast
"MEOWBOX?" to port 8766. No LAN scan: if both mDNS and broadcast are filtered, the user types the IP shown on the
speaker's System screen.
"""
import ipaddress
import json
import logging
import socket
import threading
import time
import urllib.error
import urllib.request
from dataclasses import dataclass

log = logging.getLogger("meowbox")
UDP_PORT = 8766       # query "MEOWBOX?" -> answer
ANNOUNCE_PORT = 8767  # speakers broadcast the same answer every 2 s (they miss many broadcasts in Wi-Fi power save)


@dataclass
class Speaker:
    name: str
    host: str          # "ip" or "ip:port"
    ver: str = ""
    proto: int = 0
    how: str = ""      # mdns | broadcast | ping

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


def discover_mdns(timeout: float, on_found=None, stop: threading.Event | None = None) -> list[Speaker]:
    found: dict[str, Speaker] = {}
    try:
        from zeroconf import ServiceBrowser, Zeroconf
    except ImportError:
        return []
    zc = Zeroconf()

    class Listener:
        def add_service(self, zc_, type_, name):
            info = zc_.get_service_info(type_, name, 2000)
            if not info:
                return
            addrs = sorted(info.parsed_addresses(), key=lambda a: (":" in a, a.startswith("169.254.")))
            if not addrs:
                return
            props = {k.decode(): (v or b"").decode() for k, v in (info.properties or {}).items()}
            nm = name.split("._meowbox")[0]
            host = addrs[0] if info.port == 80 else f"{addrs[0]}:{info.port}"
            sp = Speaker(nm, host, props.get("ver", ""), int(props.get("proto") or 0), "mdns")
            if nm not in found:
                found[nm] = sp
                if on_found:
                    on_found(sp)

        update_service = remove_service = lambda *a: None

    try:
        ServiceBrowser(zc, "_meowbox._tcp.local.", Listener())
        end = time.monotonic() + timeout
        while time.monotonic() < end and not (stop and stop.is_set()):
            time.sleep(0.1)
    finally:
        zc.close()
    return list(found.values())


def broadcast_addrs() -> list[str]:
    out = {"255.255.255.255"}
    for n in local_networks():
        out.add(str(n.network.broadcast_address))
    return sorted(out)


def _listener() -> socket.socket | None:
    """Socket on ANNOUNCE_PORT (shared: several agents / the dialog may listen at once)."""
    a = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        a.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        a.bind(("", ANNOUNCE_PORT))
        a.setblocking(False)
        return a
    except OSError:
        a.close()
        return None


def discover_udp(timeout: float, on_found=None) -> list[Speaker]:
    """Broadcast queries and listen for the speakers' own announcements (port 8767), whichever comes first."""
    found: dict[str, Speaker] = {}
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    ann = _listener()
    try:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        s.bind(("", 0))
        s.settimeout(0.3)
        end, next_send = time.monotonic() + timeout, 0.0
        while time.monotonic() < end:
            if time.monotonic() >= next_send:  # resend a few times: UDP may be dropped
                for b in broadcast_addrs():
                    try:
                        s.sendto(b"MEOWBOX?", (b, UDP_PORT))
                    except OSError:
                        pass
                next_send = time.monotonic() + 1.0
            for sock in (s, ann):
                if sock is None:
                    continue
                try:
                    data, (ip, _) = sock.recvfrom(512)
                    j = json.loads(data)
                    nm = j.get("name", "")
                    if nm.startswith("MeowBox") and nm not in found:
                        found[nm] = Speaker(nm, j.get("ip") or ip, j.get("ver", ""), int(j.get("proto", 0)), "broadcast")
                        if on_found:
                            on_found(found[nm])
                except (TimeoutError, socket.timeout, BlockingIOError, ValueError, OSError):
                    pass
    finally:
        s.close()
        if ann:
            ann.close()
    return list(found.values())


def discover_all(timeout: float = 3.0, on_found=None) -> list[Speaker]:
    """mDNS and broadcast at the same time; each speaker reported once (first answer wins)."""
    seen: dict[str, Speaker] = {}
    lock = threading.Lock()

    def add(sp: Speaker):
        with lock:
            if sp.name in seen:
                return
            seen[sp.name] = sp
        if on_found:
            on_found(sp)

    t = threading.Thread(target=discover_udp, args=(timeout, add), daemon=True)
    t.start()
    discover_mdns(timeout, add)
    t.join(timeout + 1)
    return list(seen.values())


def find_speaker(cfg: dict, report=None) -> Speaker | None:
    """The speaker to use: cfg['speaker'] (a name) if set, else the first one found. report(step) says what is tried."""
    want = cfg.get("speaker") or ""
    for key, how in (("last_host", "last"), ("host", "manual")):
        h = cfg.get(key)
        if h:
            if report:
                report(f"ip {h}")
            sp = ping(h)
            if sp and (not want or sp.name == want):
                sp.how = how
                return sp
    for how, fn in (("mdns", discover_mdns), ("broadcast", discover_udp)):
        if report:
            report(how)
        res = []

        def got(sp, res=res):
            res.append(sp)

        fn(3.0, got)
        for sp in res:
            if not want or sp.name == want:
                return sp
    return None


def explain_failure(host: str) -> str:
    """Best guess why a typed IP does not answer: other_network | no_answer."""
    if host and not same_subnet(host):
        return "other_network"
    return "no_answer"
