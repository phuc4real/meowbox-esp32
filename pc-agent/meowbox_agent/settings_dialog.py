"""Settings window (tray "Cài đặt…"): versions and updates of the agent, yt-dlp and ffmpeg, daily check, run at logon,
relay port, log folder. Network work runs in threads; results come back to the Tk thread through a queue."""
import os
import queue
import subprocess
import tempfile
import threading
import tkinter as tk
import urllib.request
import webbrowser
from tkinter import ttk

from . import APP_DIR, CONFIG_PATH, FROZEN, REPO, __version__
from .config import save_config
from .tools import (ffmpeg_install, ffmpeg_status, ffmpeg_version, ytdlp_latest, ytdlp_update, ytdlp_version)
from .update import RELEASES_PAGE, latest_version, newer_than_running
from .winutil import autostart_enabled, set_autostart


class SettingsDialog:
    def __init__(self, root: tk.Tk, cfg: dict, on_quit=None, on_done=None):
        self.cfg, self.on_quit, self.on_done = cfg, on_quit, on_done
        self.win = tk.Toplevel(root)
        self.win.title("MeowBox Agent: Cài đặt")
        self.win.geometry("560x470")
        self.win.resizable(False, False)
        self.win.protocol("WM_DELETE_WINDOW", self.close)
        try:
            from PIL import ImageTk

            from .tray import cat_icon
            self._icon = ImageTk.PhotoImage(cat_icon(32))
            self.win.iconphoto(False, self._icon)
        except Exception:  # noqa: BLE001
            pass
        self._q: queue.Queue = queue.Queue()
        self._drain()
        pad = {"padx": 14, "pady": (10, 0)}

        # -- agent
        f = ttk.LabelFrame(self.win, text="MeowBox Agent")
        f.pack(fill="x", **pad)
        self.agent_lbl = ttk.Label(f, text=f"Phiên bản {__version__}")
        self.agent_lbl.grid(row=0, column=0, sticky="w", padx=8, pady=6)
        self.agent_btn = ttk.Button(f, text="Kiểm tra", command=self.check_agent)
        self.agent_btn.grid(row=0, column=1, padx=8)
        f.columnconfigure(0, weight=1)

        # -- yt-dlp
        f = ttk.LabelFrame(self.win, text="yt-dlp (YouTube)")
        f.pack(fill="x", **pad)
        self.yt_lbl = ttk.Label(f, text="…")
        self.yt_lbl.grid(row=0, column=0, sticky="w", padx=8, pady=6)
        self.yt_btn = ttk.Button(f, text="Kiểm tra", command=self.check_ytdlp)
        self.yt_btn.grid(row=0, column=1, padx=8)
        f.columnconfigure(0, weight=1)

        # -- ffmpeg
        f = ttk.LabelFrame(self.win, text="ffmpeg (chỉ cho YouTube live)")
        f.pack(fill="x", **pad)
        self.ff_lbl = ttk.Label(f, text="…", wraplength=380, justify="left")
        self.ff_lbl.grid(row=0, column=0, sticky="w", padx=8, pady=6)
        self.ff_btn = ttk.Button(f, text="Tải ngay", command=self.install_ffmpeg)
        self.ff_btn.grid(row=0, column=1, padx=8)
        f.columnconfigure(0, weight=1)

        # -- options
        f = ttk.LabelFrame(self.win, text="Tùy chọn")
        f.pack(fill="x", **pad)
        self.auto = tk.BooleanVar(value=bool(cfg.get("auto_update", True)))
        ttk.Checkbutton(f, text="Tự kiểm tra bản mới mỗi ngày (agent, yt-dlp)", variable=self.auto,
                        command=self.save_auto).grid(row=0, column=0, columnspan=3, sticky="w", padx=8, pady=(6, 0))
        self.logon = tk.BooleanVar(value=autostart_enabled())
        ttk.Checkbutton(f, text="Chạy cùng Windows", variable=self.logon,
                        command=lambda: set_autostart(self.logon.get())).grid(row=1, column=0, columnspan=3,
                                                                              sticky="w", padx=8)
        ttk.Label(f, text="Cổng YouTube relay:").grid(row=2, column=0, sticky="w", padx=8, pady=6)
        self.port = tk.StringVar(value=str(cfg.get("relay_port", 8765)))
        ttk.Entry(f, textvariable=self.port, width=8).grid(row=2, column=1, sticky="w")
        ttk.Button(f, text="Lưu", command=self.save_port).grid(row=2, column=2, sticky="w", padx=6)
        self.port_lbl = ttk.Label(f, foreground="#666")
        self.port_lbl.grid(row=3, column=0, columnspan=3, sticky="w", padx=8)

        bottom = ttk.Frame(self.win)
        bottom.pack(fill="x", padx=14, pady=12, side="bottom")
        ttk.Button(bottom, text="Mở thư mục log", command=lambda: os.startfile(APP_DIR)).pack(side="left")
        ttk.Button(bottom, text="GitHub ↗", command=lambda: webbrowser.open(f"https://github.com/{REPO}")).pack(side="left", padx=6)
        ttk.Button(bottom, text="Đóng", command=self.close).pack(side="right")

        self.refresh_local()
        self.win.lift()
        self.win.focus_force()

    # -- plumbing
    def ui(self, fn):
        self._q.put(fn)

    def _drain(self):
        if not self.win.winfo_exists():
            return
        try:
            while True:
                self._q.get_nowait()()
        except queue.Empty:
            pass
        self.win.after(100, self._drain)

    def bg(self, fn, done):
        def run():
            try:
                r = fn()
            except Exception as e:  # noqa: BLE001
                r = e
            self.ui(lambda: self.win.winfo_exists() and done(r))
        threading.Thread(target=run, daemon=True).start()

    def close(self):
        self.win.destroy()
        if self.on_done:
            self.on_done()

    # -- local state
    def refresh_local(self):
        def work():
            return ytdlp_version(), ffmpeg_version(), ffmpeg_status()

        def done(r):
            if isinstance(r, Exception):
                return
            yv, fv, fs = r
            self.yt_lbl.config(text=f"Phiên bản {yv}" if yv else "Chưa cài: bấm Cập nhật để tải (~3 MB)")
            if not yv:
                self.yt_btn.config(text="Cập nhật", command=self.update_ytdlp)
            if fs["busy"]:
                self.ff_lbl.config(text=f"Đang tải… {fs['pct']}%")
                self.ff_btn.state(["disabled"])
                self.win.after(1000, self.refresh_local)
            elif fv and not fs["own"]:
                self.ff_lbl.config(text=f"Dùng ffmpeg có sẵn trong PATH ({fv})")
                self.ff_btn.state(["disabled"])
            elif fv:
                self.ff_lbl.config(text=f"Phiên bản {fv} (bản LGPL agent đã tải)")
                self.ff_btn.config(text="Tải bản mới")
                self.ff_btn.state(["!disabled"])
            else:
                err = f"\nLần trước lỗi: {fs['error']}" if fs["error"] else ""
                self.ff_lbl.config(text="Chưa có. Agent tự tải lần đầu phát YouTube live (~75 MB)." + err)
                self.ff_btn.state(["!disabled"])
        self.bg(work, done)

    # -- agent
    def check_agent(self):
        self.agent_btn.state(["disabled"])
        self.agent_lbl.config(text=f"Phiên bản {__version__} · đang kiểm tra…")

        def done(v):
            self.agent_btn.state(["!disabled"])
            if isinstance(v, Exception) or not v:
                self.agent_lbl.config(text=f"Phiên bản {__version__} · không kiểm tra được (chưa có bản phát hành?)")
            elif newer_than_running(v):
                self.agent_lbl.config(text=f"Phiên bản {__version__} · có bản {v}")
                self.agent_btn.config(text="Tải & cài", command=lambda: self.install_agent(v))
            else:
                self.agent_lbl.config(text=f"Phiên bản {__version__} · đang là bản mới nhất")
        self.bg(latest_version, done)

    def install_agent(self, v: str):
        """Installed build: download MeowBoxAgent-Setup-<v>.exe from the release, run it, quit (the installer
        replaces the files and starts the new agent). From source: open the release page."""
        if not FROZEN:
            webbrowser.open(RELEASES_PAGE)
            return
        self.agent_btn.state(["disabled"])
        self.agent_lbl.config(text=f"Đang tải bản {v}…")
        name = f"MeowBoxAgent-Setup-{v}.exe"
        url = f"https://github.com/{REPO}/releases/download/v{v}/{name}"

        def work():
            dst = os.path.join(tempfile.gettempdir(), name)
            with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "MeowBoxAgent"}),
                                        timeout=60) as r, open(dst, "wb") as f:
                while chunk := r.read(1 << 16):
                    f.write(chunk)
            return dst

        def done(r):
            if isinstance(r, Exception):
                self.agent_lbl.config(text=f"Không tải được ({r}). Mở trang GitHub để tải tay.")
                self.agent_btn.config(text="GitHub ↗", command=lambda: webbrowser.open(RELEASES_PAGE))
                self.agent_btn.state(["!disabled"])
                return
            subprocess.Popen([r], close_fds=True)
            if self.on_quit:
                self.on_quit()
        self.bg(work, done)

    # -- yt-dlp
    def check_ytdlp(self):
        self.yt_btn.state(["disabled"])

        def done(r):
            self.yt_btn.state(["!disabled"])
            have = ytdlp_version()
            if isinstance(r, Exception) or not r:
                self.yt_lbl.config(text=f"Phiên bản {have or '—'} · không kiểm tra được")
            elif r != have:
                self.yt_lbl.config(text=f"Phiên bản {have or '—'} · có bản {r}")
                self.yt_btn.config(text="Cập nhật", command=self.update_ytdlp)
            else:
                self.yt_lbl.config(text=f"Phiên bản {have} · đang là bản mới nhất")
        self.bg(ytdlp_latest, done)

    def update_ytdlp(self):
        self.yt_btn.state(["disabled"])
        self.yt_lbl.config(text="Đang tải yt-dlp…")

        def done(r):
            self.yt_btn.state(["!disabled"])
            self.yt_btn.config(text="Kiểm tra", command=self.check_ytdlp)
            self.yt_lbl.config(text=f"Không cập nhật được: {r}" if isinstance(r, Exception) else f"Đã cài yt-dlp {r}")
        self.bg(ytdlp_update, done)

    # -- ffmpeg
    def install_ffmpeg(self):
        self.ff_btn.state(["disabled"])
        self.ff_lbl.config(text="Đang tải ffmpeg (~75 MB)…")
        self.win.after(1000, self.refresh_local)

        def done(r):
            self.refresh_local()
        self.bg(ffmpeg_install, done)

    # -- options
    def save_auto(self):
        self.cfg["auto_update"] = bool(self.auto.get())
        save_config(self.cfg, CONFIG_PATH)

    def save_port(self):
        p = self.port.get().strip()
        if not p.isdigit() or not 1024 < int(p) < 65536:
            self.port_lbl.config(text="Cổng phải từ 1025 đến 65535.")
            return
        self.cfg["relay_port"] = int(p)
        save_config(self.cfg, CONFIG_PATH)
        self.port_lbl.config(text="Đã lưu. Thoát và mở lại MeowBox Agent để dùng cổng mới.")
