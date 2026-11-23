"""Tray app (docs/firmware_v0.2.0.md, 2.1). Threads: tkinter owns the main thread (hidden root, dialogs), pystray runs
detached, the asyncio core has its own thread. Tray callbacks only post to a queue that the Tk loop drains."""
import logging
import os
import queue
import threading
import tkinter as tk
import webbrowser

import pystray
from PIL import Image, ImageDraw

from . import APP_DIR, CONFIG_PATH, REPO, __version__
from .config import AgentState, load_config, save_config, setup_logging
from .core import Core
from .update import RELEASES_PAGE, latest_version, newer_than_running
from .winutil import autostart_enabled, set_autostart, single_instance

log = logging.getLogger("meowbox")
GREEN, GREY, RED = (46, 160, 67), (140, 140, 140), (210, 50, 50)


def cat_icon(size: int = 64, dot=None) -> Image.Image:
    """The MeowBox cat (same outline as the web favicon) with an optional status dot."""
    s = size / 32
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    head = [(5, 3), (12, 9), (20, 9), (27, 3), (27, 18), (25.5, 24), (21, 28), (16, 29), (11, 28), (6.5, 24), (5, 18)]
    d.polygon([(x * s, y * s) for x, y in head], fill=(30, 30, 30, 255))
    for cx in (11.5, 20.5):
        d.ellipse([(cx - 1.8) * s, (17 - 2.2) * s, (cx + 1.8) * s, (17 + 2.2) * s], fill=(245, 240, 230, 255))
    d.polygon([(14.6 * s, 21.2 * s), (17.4 * s, 21.2 * s), (16 * s, 22.8 * s)], fill=(245, 240, 230, 255))
    if dot:
        r = 6.5 * s
        cx, cy = size - r - 0.5, size - r - 0.5
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(*dot, 255), outline=(255, 255, 255, 255), width=max(1, size // 32))
    return img


def status_line(st: AgentState, cfg: dict) -> tuple[str, tuple]:
    want = st.name or cfg.get("speaker") or "MeowBox"
    how = {"mdns": "mDNS", "broadcast": "broadcast", "last": "IP lần trước", "manual": "IP nhập tay"}.get(st.how, st.how)
    if st.connected:
        return f"● Đã kết nối {st.name} · {st.ip()} ({how})", GREEN
    if st.error == "busy":
        return f"✕ {want} đang nối với {st.busy_with}", RED
    if st.error == "proto_agent":
        return "✕ Loa cần MeowBox Agent mới hơn: bấm 'Có bản mới' hoặc vào GitHub", RED
    if st.error == "port":
        return f"✕ Cổng relay {cfg.get('relay_port')} đang bị app khác dùng: mở 'Kết nối loa…'", RED
    if st.searching:
        step = {"mdns": "mDNS", "broadcast": "broadcast", "start": "…"}.get(st.searching, st.searching)
        return f"○ Đang tìm loa: {step}", GREY
    if st.error == "not_found":
        retry = f" (thử lại sau {st.retry_in} s)" if st.retry_in else ""
        return f"✕ Không thấy {want} trên mạng này, mở 'Kết nối loa…' để nhập IP{retry}", RED
    return "○ Đang tìm loa…", GREY


class TrayApp:
    def __init__(self, verbose: bool = False):
        self.cfg = load_config()
        self.state = AgentState()
        self.q: queue.Queue = queue.Queue()
        self.root = tk.Tk()
        self.root.withdraw()
        self.core = Core(self.cfg, self.state, cfg_path=CONFIG_PATH)
        self.dialog = None
        self.settings = None
        self._last_line = None
        self.icon = pystray.Icon("MeowBox", cat_icon(64, GREY), "MeowBox Agent", menu=self.menu())
        self.state.on_change(lambda _s: self.q.put(self.refresh))

    # -- menu
    def menu(self) -> pystray.Menu:
        item, sep = pystray.MenuItem, pystray.Menu.SEPARATOR
        ui = self.q.put
        return pystray.Menu(
            item(lambda _i: status_line(self.state, self.cfg)[0], None, enabled=False),
            item("Giành quyền nối loa", lambda: self.core.reconnect_now(take=True),
              visible=lambda _i: self.state.error == "busy"),
            sep,
            item("Mở trang cài đặt MeowBox", lambda: ui(self.open_web), default=True),
            item("Gửi thông báo thử", lambda: threading.Thread(target=self.test_notify, daemon=True).start()),
            item("Tạm dừng gửi số liệu", self.toggle_pause, checked=lambda _i: bool(self.cfg.get("paused"))),
            sep,
            item("Kết nối loa…", lambda: ui(self.open_dialog)),
            item("Cài đặt…", lambda: ui(self.open_settings)),
            item("Tìm lại loa ngay", lambda: self.core.reconnect_now()),
            item("Chạy cùng Windows", self.toggle_autostart, checked=lambda _i: autostart_enabled()),
            item("Mở thư mục log", lambda: os.startfile(APP_DIR)),
            sep,
            item(lambda _i: f"Có bản {self.state.new_version} ↗", lambda: webbrowser.open(RELEASES_PAGE),
              visible=lambda _i: bool(self.state.new_version)),
            item("GitHub ↗", lambda: webbrowser.open(f"https://github.com/{REPO}")),
            item(f"Giới thiệu (v{__version__})", lambda: webbrowser.open(f"https://github.com/{REPO}#readme")),
            item("Thoát", lambda: ui(self.quit)),
        )

    def refresh(self):
        line, color = status_line(self.state, self.cfg)
        if line == self._last_line:
            return
        self._last_line = line
        self.icon.icon = cat_icon(64, color)
        tip = f"{self.state.name or 'MeowBox'} · {self.state.ip() or '—'} · " + \
            ("đã kết nối" if self.state.connected else line.lstrip("●○✕ "))
        self.icon.title = tip[:127]
        self.icon.update_menu()

    # -- actions
    def open_web(self):
        h = self.state.host or self.cfg.get("last_host") or self.cfg.get("host")
        if h:
            webbrowser.open(f"http://{h}/")
        else:
            self.open_dialog()

    def open_dialog(self):
        from .connect_dialog import ConnectDialog

        if self.dialog and self.dialog.win.winfo_exists():
            self.dialog.win.lift()
            return
        self.dialog = ConnectDialog(self.root, self.cfg, self.state, self.core, on_done=lambda: setattr(self, "dialog", None))

    def open_settings(self):
        from .settings_dialog import SettingsDialog

        if self.settings and self.settings.win.winfo_exists():
            self.settings.win.lift()
            return
        self.settings = SettingsDialog(self.root, self.cfg, on_quit=lambda: self.q.put(self.quit),
                                       on_done=lambda: setattr(self, "settings", None))

    def test_notify(self):
        from .core import speaker_request

        h = self.state.host
        if not h or not self.cfg.get("token"):
            self.icon.notify("Chưa nối loa hoặc chưa có token: mở 'Kết nối loa…'", "MeowBox")
            return
        body = '{"title":"MeowBox Agent","text":"Thông báo thử từ máy tính","sound":"chime"}'.encode()
        try:
            code, _ = speaker_request(h, "/api/notify", self.cfg["token"], body)
            if code >= 400:
                self.icon.notify(f"Loa trả lỗi {code} (token sai?)", "MeowBox")
        except OSError as e:
            self.icon.notify(f"Không gửi được: {e}", "MeowBox")

    def toggle_pause(self):
        self.cfg["paused"] = not self.cfg.get("paused")
        save_config(self.cfg)
        self.icon.update_menu()

    def toggle_autostart(self):
        set_autostart(not autostart_enabled())
        self.icon.update_menu()

    def check_update(self):
        """Daily, if 'auto_update' (Settings): a newer agent on GitHub (menu + toast), a newer yt-dlp (installed)."""
        if self.cfg.get("auto_update", True):
            from .tools import maintain_ytdlp

            v = latest_version()
            if newer_than_running(v):
                self.state.update(new_version=v)
                self.icon.notify(f"Có MeowBox Agent {v}: mở Cài đặt để cập nhật", "MeowBox")
            maintain_ytdlp(daily_check=True)
        self.root.after(24 * 3600 * 1000, lambda: threading.Thread(target=self.check_update, daemon=True).start())

    def quit(self):
        log.info("quit")
        self.icon.stop()
        self.core.stop()
        self.root.quit()

    # -- main loop
    def pump(self):
        try:
            while True:
                self.q.get_nowait()()
        except queue.Empty:
            pass
        self.root.after(100, self.pump)

    def run(self):
        from . import relay

        relay.NOTIFY.append(lambda text: self.icon.notify(text, "MeowBox"))
        self.core.start()
        self.icon.run_detached()
        self.refresh()
        if not self.cfg.get("speaker"):  # first run: open the connect window by itself
            self.root.after(800, self.open_dialog)
        self.root.after(5000, lambda: threading.Thread(target=self.check_update, daemon=True).start())
        self.pump()
        self.root.mainloop()


def main(verbose: bool = False) -> None:
    setup_logging(verbose, to_file=True)
    if not single_instance():  # second launch: just open the speaker's page, the running agent keeps going
        cfg = load_config()
        h = cfg.get("last_host") or cfg.get("host")
        if h:
            webbrowser.open(f"http://{h}/")
        return
    log.info("MeowBox Agent %s starting (tray)", __version__)
    TrayApp(verbose).run()
