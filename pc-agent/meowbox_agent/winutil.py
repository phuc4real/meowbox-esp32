"""Windows bits: single instance, run at logon, firewall rule for the relay, network category, Wi-Fi name."""
import ctypes
import logging
import os
import re
import subprocess
import sys
import winreg

log = logging.getLogger("meowbox")
RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
RUN_NAME = "MeowBox Agent"
NOWIN = subprocess.CREATE_NO_WINDOW
_mutex = None


def single_instance(name: str = "Local\\MeowBoxAgent") -> bool:
    """True if this is the only running agent (the named mutex stays held until exit)."""
    global _mutex
    _mutex = ctypes.windll.kernel32.CreateMutexW(None, False, name)
    return ctypes.windll.kernel32.GetLastError() != 183  # ERROR_ALREADY_EXISTS


def exe_command() -> str:
    """What to start at logon: the installed exe, or pythonw -m meowbox_agent when running from source."""
    if getattr(sys, "frozen", False):
        return f'"{sys.executable}"'
    pyw = os.path.join(os.path.dirname(sys.executable), "pythonw.exe")
    agent = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "agent.py")
    return f'"{pyw}" "{agent}" tray'


def autostart_enabled() -> bool:
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as k:
            winreg.QueryValueEx(k, RUN_NAME)
            return True
    except OSError:
        return False


def set_autostart(on: bool) -> None:
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_SET_VALUE) as k:
        if on:
            winreg.SetValueEx(k, RUN_NAME, 0, winreg.REG_SZ, exe_command())
        else:
            try:
                winreg.DeleteValue(k, RUN_NAME)
            except OSError:
                pass


# No PowerShell here: endpoint protection (Sophos Intercept X "BrowserAncestorPowershell") blocks powershell.exe
# when the agent was started by an installer downloaded in a browser. Use COM (netlist.py) and netsh instead.

def network_category(local_ip: str) -> str:
    """Public | Private | DomainAuthenticated for the adapter that has local_ip ('' if unknown)."""
    if not local_ip:
        return ""
    import psutil

    from .netlist import connections

    aliases = {name for name, addrs in psutil.net_if_addrs().items() if any(a.address == local_ip for a in addrs)}
    return next((cat for alias, _, cat in connections() if alias in aliases), "")


def wifi_name() -> str:
    """Name of the Wi-Fi this PC is on. netsh needs the Location permission on Windows 11, so fall back to the network
    profile name of the Wi-Fi adapter (Windows names the profile after the SSID)."""
    out = subprocess.run(["netsh", "wlan", "show", "interfaces"], capture_output=True, text=True, errors="replace",
                         creationflags=NOWIN).stdout
    for line in out.splitlines():
        k, _, v = line.partition(":")
        if k.strip() == "SSID" and v.strip():
            return v.strip()
    from .netlist import connections

    return next((name for alias, name, _ in connections() if re.search(r"Wi-?Fi|WLAN|Wireless", alias, re.I)), "")


def allow_relay_on_this_network(port: int, profile: str) -> bool:
    """Firewall rule for the relay on one profile (Public/Private), via an elevated netsh (UAC prompt).
    Only offered when relay_test failed and that profile has no rule (docs/firmware_v0.2.0.md, 2.2 step 3)."""
    prog = f' program="{sys.executable}"' if getattr(sys, "frozen", False) else ""
    args = (f'advfirewall firewall add rule name="MeowBox Agent relay ({profile})" dir=in action=allow protocol=TCP '
            f"localport={port} profile={profile.lower()}{prog}")
    r = ctypes.windll.shell32.ShellExecuteW(None, "runas", "netsh.exe", args, None, 0)
    return r > 32


def local_ip_towards(ip: str) -> str:
    import socket

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((ip, 80))
        return s.getsockname()[0]
    except OSError:
        return ""
    finally:
        s.close()
