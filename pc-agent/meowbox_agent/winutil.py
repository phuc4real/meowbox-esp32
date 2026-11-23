"""Windows bits: single instance, run at logon, firewall rule for the relay, network category, Wi-Fi name."""
import ctypes
import logging
import os
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


def _ps(cmd: str, timeout: float = 10) -> str:
    try:
        return subprocess.run(["powershell", "-NoProfile", "-Command", cmd], capture_output=True, text=True,
                              timeout=timeout, creationflags=NOWIN).stdout.strip()
    except Exception:  # noqa: BLE001
        return ""


def network_category(local_ip: str) -> str:
    """Public | Private | DomainAuthenticated for the adapter that has local_ip ('' if unknown)."""
    if not local_ip:
        return ""
    idx = _ps(f"(Get-NetIPAddress -IPAddress '{local_ip}' -ErrorAction SilentlyContinue).InterfaceIndex")
    if not idx.isdigit():
        return ""
    return _ps(f"(Get-NetConnectionProfile -InterfaceIndex {idx} -ErrorAction SilentlyContinue).NetworkCategory")


def wifi_name() -> str:
    """Name of the Wi-Fi this PC is on. netsh needs the Location permission on Windows 11, so fall back to the network
    profile name of the Wi-Fi adapter (Windows names the profile after the SSID)."""
    out = subprocess.run(["netsh", "wlan", "show", "interfaces"], capture_output=True, text=True, errors="replace",
                         creationflags=NOWIN).stdout
    for line in out.splitlines():
        k, _, v = line.partition(":")
        if k.strip() == "SSID" and v.strip():
            return v.strip()
    return _ps("(Get-NetConnectionProfile | Where-Object { $_.InterfaceAlias -match 'Wi-?Fi|WLAN|Wireless' } "
               "| Select-Object -First 1).Name")


def allow_relay_on_this_network(port: int, profile: str) -> bool:
    """Firewall rule for the relay on one profile (Public/Private), via an elevated PowerShell (UAC prompt).
    Only offered when relay_test failed and that profile has no rule (docs/firmware_v0.2.0.md, 2.2 step 3)."""
    exe = sys.executable
    prog = f"-Program '{exe}' " if getattr(sys, "frozen", False) else ""
    cmd = (f"New-NetFirewallRule -DisplayName 'MeowBox Agent relay ({profile})' -Direction Inbound -Action Allow "
           f"-Protocol TCP -LocalPort {port} {prog}-Profile {profile}")
    r = ctypes.windll.shell32.ShellExecuteW(None, "runas", "powershell.exe",
                                            f'-NoProfile -WindowStyle Hidden -Command "{cmd}"', None, 0)
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
