"""Command line. With no command: the tray app (MeowBoxAgent.exe); `run` = headless agent (old behaviour)."""
import argparse
import asyncio
import json
import sys
import urllib.parse
from pathlib import Path

from . import CONFIG_PATH, __version__
from .config import AgentState, load_config, save_config, setup_logging


def _host(cfg: dict) -> str:
    from .discovery import find_speaker

    sp = find_speaker(cfg)
    if not sp:
        raise SystemExit("Không tìm thấy loa. Dùng --host IP hoặc: meowbox config --set-host IP")
    return sp.host


def _post(cfg: dict, path: str, body: bytes, ctype: str = "application/json") -> None:
    from .core import speaker_request

    try:
        code, text = speaker_request(_host(cfg), path, cfg.get("token", ""), body, ctype)
    except OSError as e:
        raise SystemExit(f"Lỗi kết nối: {e}") from None
    print(code, text)
    if code >= 400:
        raise SystemExit(1)


def main(argv: list[str] | None = None) -> None:
    for s in (sys.stdout, sys.stderr):
        if s:  # None under pythonw.exe / the windowed exe
            s.reconfigure(encoding="utf-8", errors="replace")
    argv = sys.argv[1:] if argv is None else argv
    if argv[:1] == ["ytdlp"]:  # child process of the relay (installed build): yt-dlp from its zip, args untouched
        from .tools import run_ytdlp_inprocess

        raise SystemExit(run_ytdlp_inprocess(argv[1:]))
    ap = argparse.ArgumentParser(prog="meowbox", description=f"MeowBox PC agent {__version__}")
    ap.add_argument("--config", type=Path, default=CONFIG_PATH, help=f"file cấu hình (mặc định {CONFIG_PATH})")
    ap.add_argument("--host", help="IP[:port] của loa (bỏ qua tìm tự động)")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--version", action="version", version=__version__)
    sub = ap.add_subparsers(dest="cmd")
    sub.add_parser("tray", help="icon ở khay hệ thống (mặc định của MeowBoxAgent.exe)")
    r = sub.add_parser("run", help="chạy agent không có giao diện")
    r.add_argument("--no-relay", action="store_true", help="không mở YouTube relay")
    r.add_argument("--no-update", action="store_true", help="không chạy yt-dlp -U")
    n = sub.add_parser("notify", help="gửi thông báo")
    n.add_argument("title")
    n.add_argument("text", nargs="?", default="")
    n.add_argument("--sound", choices=["chime", "alert", "none"], default="chime")
    n.add_argument("--priority", choices=["low", "normal", "high"], default="normal")
    n.add_argument("--duration", type=int, default=10)
    s = sub.add_parser("say", help="đọc câu bằng TTS Windows và gửi lên loa")
    s.add_argument("text")
    s.add_argument("--voice", help="một phần tên/ngôn ngữ giọng, vd 'An' hoặc 'vi-VN'")
    s.add_argument("--save", type=Path, help="chỉ lưu WAV ra file, không gửi")
    w = sub.add_parser("wifi-setup", help="bật/tắt Wi-Fi cài đặt MeowBox-XXXX trên loa (tự tắt sau 10 phút không ai vào)")
    w.add_argument("action", nargs="?", choices=["on", "off", "status"], default="on")
    sub.add_parser("discover", help="liệt kê các loa tìm được qua mDNS và broadcast")
    tl = sub.add_parser("tools", help="yt-dlp dùng cho YouTube relay: xem phiên bản / tải bản mới nhất")
    tl.add_argument("action", nargs="?", choices=["status", "update"], default="status")
    c = sub.add_parser("config", help="xem/sửa cấu hình")
    c.add_argument("--set-host", dest="set_host", metavar="HOST", help="IP[:port] loa, '' = tự tìm")
    c.add_argument("--speaker", help="tên loa sẽ dùng, vd MeowBox-1A2B ('' = loa đầu tiên tìm thấy)")
    c.add_argument("--token")
    c.add_argument("--relay-port", type=int)
    args, extra = ap.parse_known_args(argv)
    if args.cmd is None:  # old habit: `agent.py --no-relay` meant run
        args = ap.parse_args([*(a for a in argv if a not in extra), "run", *extra]) if extra else \
            ap.parse_args([*argv, "run"])
    elif extra:
        ap.error(f"unrecognized arguments: {' '.join(extra)}")

    if args.cmd == "tray":
        from .tray import main as tray_main

        tray_main(verbose=args.verbose)
        return

    setup_logging(args.verbose, to_file=sys.stderr is None)
    cfg = load_config(args.config)
    if args.cmd == "config":
        for k, v in (("host", args.set_host), ("speaker", args.speaker), ("token", args.token),
                     ("relay_port", args.relay_port)):
            if v is not None:
                cfg[k] = v
        save_config(cfg, args.config)
        print(args.config, json.dumps(cfg, indent=2, ensure_ascii=False), sep="\n")
        return
    if args.host:
        cfg["host"], cfg["last_host"], cfg["speaker"] = args.host, "", ""

    if args.cmd == "run":
        from .core import Core

        core = Core(cfg, AgentState(), relay=not args.no_relay, ytdlp_update=not args.no_update, cfg_path=args.config)
        try:
            core.run_forever()
        except KeyboardInterrupt:
            pass
    elif args.cmd == "tools":
        from .tools import YTDLP, ytdlp_latest, ytdlp_update, ytdlp_version

        if args.action == "update":
            try:
                print("yt-dlp", ytdlp_update(lambda step: print("…", step, flush=True)), "->", YTDLP)
            except RuntimeError as e:
                raise SystemExit(f"yt-dlp: {e}") from None
        else:
            print(f"yt-dlp: {ytdlp_version() or 'chưa cài'} ({YTDLP}), mới nhất trên GitHub: {ytdlp_latest() or '?'}")
    elif args.cmd == "discover":
        from .discovery import discover_all

        found = discover_all(3.0, lambda sp: print(f"{sp.name:<14} {sp.host:<21} v{sp.ver or '?':<8} ({sp.how})"))
        if not found:
            print("Không tìm thấy loa nào (mạng chặn mDNS và broadcast?). Nhập IP hiện trên màn hình loa.")
    elif args.cmd == "notify":
        body = {"title": args.title, "text": args.text, "sound": args.sound,
                "priority": args.priority, "duration": args.duration}
        _post(cfg, "/api/notify", json.dumps(body).encode())
    elif args.cmd == "wifi-setup":
        if args.action == "status":
            from .core import speaker_request

            print(*speaker_request(_host(cfg), "/api/wifi/portal", cfg.get("token", "")))
        else:
            _post(cfg, "/api/wifi/portal", json.dumps({"on": args.action == "on"}).encode())
    elif args.cmd == "say":
        from .core import tts_wav

        wav = asyncio.run(tts_wav(args.text, args.voice))
        if args.save:
            args.save.write_bytes(wav)
            print(f"saved {len(wav)} bytes -> {args.save}")
        else:
            _post(cfg, "/api/notify/audio?" + urllib.parse.urlencode({"title": args.text[:60]}), wav, "audio/wav")
