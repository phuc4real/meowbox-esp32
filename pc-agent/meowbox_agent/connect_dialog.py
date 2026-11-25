"""'Connect speaker' window (docs/firmware_v0.2.0.md, 2.2): 1 speaker IP, 2 notify token, 3 YouTube relay check.

Runs on the tkinter (main) thread as a Toplevel of the tray's hidden root. Network work happens in worker threads and
comes back through root.after(). On Finish it saves speaker/token/last_host and asks the core to reconnect now.
"""
import json
import queue
import threading
import tkinter as tk
import urllib.error
import urllib.request
import webbrowser
from tkinter import ttk

from . import CONFIG_PATH
from .config import save_config
from .discovery import Speaker, explain_failure, ping_detail
from .winutil import allow_relay_on_this_network, local_ip_towards, network_category, wifi_name

WIFI_HELP = "https://github.com/phuc4real/meowbox-esp32#readme"

T = {
    "title": "Kết nối loa MeowBox",
    "steps": ["1 IP loa", "2 Token", "3 Kiểm tra"],
    "ip": "Nhập IP hiện trên màn hình loa (tile Hệ thống), vd 192.168.1.50:",
    "ip_tip": "Agent không tự dò loa trong mạng. Loa đổi IP thì nhập lại ở đây; nên đặt IP cố định cho loa "
              "trên router (DHCP reservation).",
    "try": "Thử",
    "ok_ip": "✓ {name} (firmware {ver})",
    "not_meowbox": "IP này không phải MeowBox (hoặc loa đang chạy firmware cũ).",
    "no_answer": "Không nối được tới IP này.",
    "bad_address": "IP không hợp lệ.",
    "other_network": "IP này không cùng mạng với máy tính.",
    "reasons": "Lý do hay gặp:\n• Máy tính và loa không cùng mạng Wi-Fi. Máy tính đang ở: {wifi}\n"
               "• Mạng khách / quán cà phê chặn các máy nói chuyện với nhau (client isolation): không dùng được.\n"
               "• Loa chưa có Wi-Fi: vào Wi-Fi cài đặt MeowBox-XXXX của loa bằng điện thoại.",
    "wifi_help": "Hướng dẫn cài Wi-Fi cho loa",
    "next": "Tiếp ›", "back": "‹ Quay lại", "finish": "Xong", "skip": "Bỏ qua",
    "token_intro": "Token dùng để gửi thông báo, đọc chữ (say) và bật Wi-Fi cài đặt từ máy tính.\n"
                   "Số liệu máy và điều khiển nhạc không cần token.",
    "open_notify": "Mở trang Thông báo trên loa",
    "token": "Token:", "check": "Kiểm tra",
    "token_ok": "✓ Token đúng", "token_bad": "Token sai", "token_err": "Không kiểm tra được ({e})",
    "relay_intro": "Kiểm tra loa có gọi được YouTube relay trên máy này không (cổng {port}).",
    "relay_port_busy": "Cổng relay đang bị app khác dùng. Chọn cổng khác rồi khởi động lại agent:",
    "relay_run": "Kiểm tra relay",
    "relay_wait": "Đang chờ loa kết nối…",
    "relay_ok": "✓ Loa gọi được relay ({ms} ms). YouTube sẽ phát được.",
    "relay_fail": "Loa không gọi được relay ({err}).",
    "relay_need_token": "Cần token (bước 2) để kiểm tra relay.",
    "public": "Windows đang xếp mạng này là Public, tường lửa chặn relay.",
    "allow": "Cho phép trên mạng này (cần quyền admin)",
    "allowed": "Đã gửi yêu cầu thêm rule tường lửa. Bấm Kiểm tra relay lại.",
    "saved_port": "Đã lưu cổng {port}. Thoát và mở lại MeowBox Agent để dùng cổng mới.",
}


class ConnectDialog:
    def __init__(self, root: tk.Tk, cfg: dict, state, core, on_done=None):
        self.cfg, self.state, self.core, self.on_done = cfg, state, core, on_done
        self.sel: Speaker | None = None
        self.win = tk.Toplevel(root)
        self.win.title(T["title"])
        try:  # the cat instead of the Tk feather
            from PIL import ImageTk

            from .tray import cat_icon
            self._icon = ImageTk.PhotoImage(cat_icon(32))
            self.win.iconphoto(False, self._icon)
        except Exception:  # noqa: BLE001
            pass
        self.win.geometry("560x420")
        self.win.resizable(False, False)
        self.win.protocol("WM_DELETE_WINDOW", self.close)
        self.step = 0
        self.head = ttk.Label(self.win, font=("Segoe UI", 10))
        self.head.pack(anchor="w", padx=16, pady=(12, 4))
        self.body = ttk.Frame(self.win)
        self.body.pack(fill="both", expand=True, padx=16)
        nav = ttk.Frame(self.win)
        nav.pack(fill="x", padx=16, pady=12)
        self.b_back = ttk.Button(nav, text=T["back"], command=lambda: self.show(self.step - 1))
        self.b_next = ttk.Button(nav, text=T["next"], command=self.next)
        self.b_back.pack(side="left")
        self.b_next.pack(side="right")
        self.token = tk.StringVar(value=cfg.get("token", ""))
        self._q: queue.Queue = queue.Queue()
        self._drain()
        self.show(0)
        self.win.lift()
        self.win.focus_force()

    # -- helpers
    def ui(self, fn):
        """Run fn on the UI thread (safe from any thread: tkinter itself must only be touched from its own thread)."""
        self._q.put(fn)

    def _drain(self):
        if not self.win.winfo_exists():
            return
        try:
            while True:
                self._q.get_nowait()()
        except queue.Empty:
            pass
        self.win.after(50, self._drain)

    def bg(self, fn, done):
        """fn() in a thread, done(result) back on the UI thread."""
        def run():
            try:
                r = fn()
            except Exception as e:  # noqa: BLE001
                r = e
            self.ui(lambda: done(r))
        threading.Thread(target=run, daemon=True).start()

    def clear(self):
        for w in self.body.winfo_children():
            w.destroy()

    def show(self, i: int):
        self.step = max(0, min(2, i))
        self.head.config(text="  ›  ".join(("● " if k == self.step else "") + s for k, s in enumerate(T["steps"])))
        self.clear()
        [self.page_find, self.page_token, self.page_relay][self.step]()
        self.b_back.state(["!disabled"] if self.step else ["disabled"])
        self.b_next.config(text=T["finish"] if self.step == 2 else T["next"])

    def next(self):
        if self.step == 0 and not self.sel:
            return
        if self.step == 0:
            self.save(reconnect=True)
        if self.step == 1:
            self.save(reconnect=False)
        if self.step == 2:
            self.save(reconnect=False)
            self.close()
            return
        self.show(self.step + 1)

    def save(self, reconnect: bool):
        if self.sel:
            self.cfg["speaker"] = self.sel.name
            self.cfg["last_host"] = self.cfg["host"] = self.sel.host
        self.cfg["token"] = self.token.get().strip()
        save_config(self.cfg, getattr(self.core, "cfg_path", None) or CONFIG_PATH)
        if reconnect and self.core:
            self.core.reconnect_now()

    def close(self):
        if self.core:
            self.core.extra_allowed.clear()
        self.win.destroy()
        if self.on_done:
            self.on_done()

    # -- step 1
    def page_find(self):
        ttk.Label(self.body, text=T["ip"]).pack(anchor="w")
        r = ttk.Frame(self.body)
        r.pack(fill="x", pady=4)
        ip = self.sel.host if self.sel else self.cfg.get("last_host") or self.cfg.get("host") or ""
        self.ip = tk.StringVar(value=ip)
        e = ttk.Entry(r, textvariable=self.ip, width=22)
        e.pack(side="left")
        e.bind("<Return>", lambda _e: self.try_ip())
        e.focus_set()
        ttk.Button(r, text=T["try"], command=self.try_ip).pack(side="left", padx=6)
        self.ip_lbl = ttk.Label(self.body)
        self.ip_lbl.pack(anchor="w", pady=(4, 0))
        ttk.Label(self.body, text=T["ip_tip"], foreground="#666", wraplength=520, justify="left").pack(anchor="w", pady=(8, 0))
        self.reasons = ttk.Label(self.body, foreground="#555", wraplength=520, justify="left")
        self.ip.trace_add("write", lambda *_: self.ip_edited())
        if self.ip.get().strip():
            self.try_ip()

    def ip_edited(self):
        """A different IP than the one checked: check again before Next."""
        if self.sel and self.ip.get().strip() != self.sel.host:
            self.sel = None
            self.ip_lbl.config(text="")

    def try_ip(self):
        host = self.ip.get().strip()
        self.ip_lbl.config(text="…", foreground="#666")

        def done(r):
            sp, err = r if isinstance(r, tuple) else (None, "no_answer")
            if self.step != 0 or self.ip.get().strip() != host:
                return  # left the page or edited the IP while checking
            if sp:
                self.sel = sp
                self.ip_lbl.config(text=T["ok_ip"].format(name=sp.name, ver=sp.ver or "?"), foreground="#080")
                self.reasons.pack_forget()
            else:
                if err == "no_answer":
                    err = explain_failure(host)
                self.ip_lbl.config(text=T[err], foreground="#b00")
                self.show_reasons()
        self.bg(lambda: ping_detail(host), done)

    def show_reasons(self):
        self.reasons.config(text=T["reasons"].format(wifi=wifi_name() or "(không phải Wi-Fi / không rõ)"))
        self.reasons.pack(anchor="w", pady=(10, 4))
        if not any(isinstance(w, ttk.Button) and w.cget("text") == T["wifi_help"] for w in self.body.winfo_children()):
            ttk.Button(self.body, text=T["wifi_help"], command=lambda: webbrowser.open(WIFI_HELP)).pack(anchor="w")

    # -- step 2
    def page_token(self):
        ttk.Label(self.body, text=T["token_intro"], wraplength=520, justify="left").pack(anchor="w", pady=(0, 10))
        host = self.sel.host if self.sel else ""
        ttk.Button(self.body, text=T["open_notify"],
                   command=lambda: webbrowser.open(f"http://{host}/#/notify")).pack(anchor="w")
        r = ttk.Frame(self.body)
        r.pack(fill="x", pady=10)
        ttk.Label(r, text=T["token"]).pack(side="left")
        ttk.Entry(r, textvariable=self.token, width=40).pack(side="left", padx=6)
        ttk.Button(r, text=T["check"], command=self.check_token).pack(side="left")
        self.tok_lbl = ttk.Label(self.body)
        self.tok_lbl.pack(anchor="w")
        ttk.Button(self.body, text=T["skip"], command=lambda: self.show(2)).pack(anchor="w", pady=(16, 0))

    def check_token(self):
        host, tok = (self.sel.host if self.sel else ""), self.token.get().strip()

        def work():
            req = urllib.request.Request(f"http://{host}/api/notify/history", headers={"Authorization": f"Bearer {tok}"})
            try:
                with urllib.request.urlopen(req, timeout=5) as r:
                    return r.status
            except urllib.error.HTTPError as e:
                return e.code

        def done(r):
            if r == 200:
                self.tok_lbl.config(text=T["token_ok"], foreground="#080")
            elif r == 401:
                self.tok_lbl.config(text=T["token_bad"], foreground="#b00")
            else:
                self.tok_lbl.config(text=T["token_err"].format(e=r), foreground="#b00")
        self.bg(work, done)

    # -- step 3
    def page_relay(self):
        port = int(self.cfg.get("relay_port") or 8765)
        if not self.state.relay_port:
            ttk.Label(self.body, text=T["relay_port_busy"], wraplength=520).pack(anchor="w")
            r = ttk.Frame(self.body)
            r.pack(anchor="w", pady=6)
            pv = tk.StringVar(value=str(port + 1))
            ttk.Entry(r, textvariable=pv, width=8).pack(side="left")
            msg = ttk.Label(self.body)

            def save_port():
                if pv.get().isdigit() and 1024 < int(pv.get()) < 65536:
                    self.cfg["relay_port"] = int(pv.get())
                    self.save(reconnect=False)
                    msg.config(text=T["saved_port"].format(port=pv.get()))
            ttk.Button(r, text="OK", command=save_port).pack(side="left", padx=6)
            msg.pack(anchor="w")
            return
        ttk.Label(self.body, text=T["relay_intro"].format(port=port), wraplength=520).pack(anchor="w", pady=(0, 10))
        ttk.Button(self.body, text=T["relay_run"], command=self.relay_test).pack(anchor="w")
        self.relay_lbl = ttk.Label(self.body, wraplength=520, justify="left")
        self.relay_lbl.pack(anchor="w", pady=8)
        self.relay_btn = None
        self.relay_test()

    def relay_test(self):
        if not self.token.get().strip():
            self.relay_lbl.config(text=T["relay_need_token"], foreground="#666")
            return
        sp = self.sel
        if not sp:
            return
        if self.core:
            self.core.extra_allowed.add(sp.ip)
        self.relay_lbl.config(text=T["relay_wait"], foreground="#666")
        tok = self.token.get().strip()

        def work():
            import time

            for _ in range(20):  # the speaker learns the relay address from our hello
                if self.state.connected and self.state.name == sp.name:
                    break
                time.sleep(0.5)
            req = urllib.request.Request(f"http://{sp.host}/api/pc/relay_test", data=b"{}", method="POST",
                                         headers={"Content-Type": "application/json", "Authorization": f"Bearer {tok}"})
            with urllib.request.urlopen(req, timeout=8) as r:
                return json.loads(r.read())

        def done(r):
            if isinstance(r, Exception):
                self.relay_lbl.config(text=T["relay_fail"].format(err=r), foreground="#b00")
                return
            self.state.update(relay_ok=bool(r.get("ok")))
            if r.get("ok"):
                self.relay_lbl.config(text=T["relay_ok"].format(ms=r.get("ms", "?")), foreground="#080")
                return
            text = T["relay_fail"].format(err=r.get("err"))
            cat = network_category(local_ip_towards(sp.ip))
            if cat == "Public":
                text += "\n" + T["public"]
                if not self.relay_btn:
                    port = int(self.cfg.get("relay_port") or 8765)

                    def allow():
                        allow_relay_on_this_network(port, "Public")
                        self.relay_lbl.config(text=T["allowed"], foreground="#666")
                    self.relay_btn = ttk.Button(self.body, text=T["allow"], command=allow)
                    self.relay_btn.pack(anchor="w")
            self.relay_lbl.config(text=text, foreground="#b00")
        self.bg(work, done)
