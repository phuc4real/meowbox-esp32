"""MeowBox PC agent: PC stats + media -> speaker, notify/say CLI, YouTube relay, tray icon.

Layout (docs/firmware_v0.2.0.md, 2.9):
  config.py          agent.json (+ AgentState shared by the asyncio core and the tray/dialogs)
  discovery.py       reach the speaker at the last / typed IP (/api/ping); no mDNS or broadcast
  relay.py           YouTube relay (yt-dlp + ffmpeg -> MP3), only serves the connected speaker
  core.py            stats, media (SMTC), the WebSocket link, TTS, yt-dlp self-update
  tray.py            tray icon + menu (pystray), runs the core in a thread
  connect_dialog.py  "Connect speaker" window (tkinter)
  update.py          checks GitHub Releases for a newer agent
  cli.py             command line: run / tray / notify / say / wifi-setup / config / tools
"""
import os
import sys
from pathlib import Path

__version__ = "0.2.3"
PROTO = 1  # speaker <-> agent protocol, see MB_PROTO in firmware/components/net/include/net.h
REPO = "phuc4real/meowbox-esp32"
APP_DIR = Path(os.environ.get("APPDATA", Path.home())) / "MeowBox"
CONFIG_PATH = APP_DIR / "agent.json"
FROZEN = getattr(sys, "frozen", False)  # running from the PyInstaller build
