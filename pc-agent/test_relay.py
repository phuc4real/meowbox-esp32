"""Checks for the YouTube relay's input handling (no network, no yt-dlp run). Run: python test_relay.py
Prints ALL PASSED. Needs the agent's requirements (aiohttp)."""
import asyncio
import json
import sys
from unittest import mock

from aiohttp import web

from meowbox_agent import core, relay

FAIL = []


def check(name: str, ok: bool) -> None:
    print(("  ok   " if ok else "  FAIL ") + name)
    if not ok:
        FAIL.append(name)


print("relay url check")
for u in ("https://www.youtube.com/watch?v=abc", "http://youtu.be/abc", "https://music.youtube.com/playlist?list=x",
          "HTTPS://M.YOUTUBE.COM/watch?v=abc", "https://youtube.com"):
    try:
        relay.check_url(u)
        check(f"accepts {u}", True)
    except web.HTTPBadRequest:
        check(f"accepts {u}", False)
for u in ("", "-x", "--version", "youtu.be/abc", "ftp://youtube.com/x", "https://youtube.com.example.org/x",
          "https://evil.example/youtube.com", "https://notyoutube.com/x", "https://youtube.com/x y", "file:///c:/x"):
    try:
        relay.check_url(u)
        check(f"refuses {u!r}", False)
    except web.HTTPBadRequest:
        check(f"refuses {u!r}", True)


print("yt-dlp argv")
seen = []


async def fake_exec(*args, **kw):
    seen.append(list(args))
    raise RuntimeError("stop")  # the test only looks at the argv


async def run_ytjson():
    with mock.patch.object(relay.asyncio, "create_subprocess_exec", fake_exec):
        for single in (True, False):
            try:
                await relay.ytjson("https://youtu.be/abc", single)
            except RuntimeError:
                pass
        for bad in ("-x", "--version"):
            try:
                await relay.ytjson(bad, True)
            except web.HTTPBadRequest:
                pass

asyncio.run(run_ytjson())
check("two yt-dlp runs, both ending with ['--', url]", len(seen) == 2 and all(a[-2:] == ["--", "https://youtu.be/abc"]
                                                                            for a in seen))
check("no process started for refused urls", len(seen) == 2)


print("agent messages from the speaker")
check("normal message parses", json.loads('{"t":"media_cmd","cmd":"next"}', object_pairs_hook=core.unique_keys)
      == {"t": "media_cmd", "cmd": "next"})
try:
    json.loads('{"t":"a","x":1,"t":"b"}', object_pairs_hook=core.unique_keys)
    check("a repeated key is refused", False)
except ValueError:
    check("a repeated key is refused", True)

print("\nRESULT:", "ALL PASSED" if not FAIL else f"{len(FAIL)} FAILED: {FAIL}")
sys.exit(1 if FAIL else 0)
