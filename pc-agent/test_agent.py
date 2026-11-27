"""Checks for the agent's input handling: YouTube relay, speaker messages, Meow Coding hooks (no network, no yt-dlp
run, nothing written outside a temporary folder). Run: python test_agent.py. Prints ALL PASSED. Needs the agent's
requirements (aiohttp)."""
import asyncio
import json
import shutil
import sys
import tempfile
from pathlib import Path
from unittest import mock

from aiohttp import web
from aiohttp.test_utils import TestClient, TestServer

from meowbox_agent import coding, core, relay

TMP = Path(tempfile.mkdtemp(prefix="meowbox-test-"))
coding.HOOK_KEY_PATH = TMP / "hook.key"  # never the real %APPDATA%\MeowBox
coding.CLAUDE_DIR = TMP / ".claude"

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

print("Meow Coding paths")
proj = coding.CLAUDE_DIR / "projects" / "p"
proj.mkdir(parents=True)
(proj / "s.jsonl").write_text("", encoding="utf-8")
check("transcript under the Claude folder: read", coding.transcript_ok(str(proj / "s.jsonl")))
for p in (r"\\lab-host\share\t.jsonl", "//lab-host/share/t.jsonl", r"\\?\UNC\lab-host\share\t.jsonl",
          str(TMP / "other.jsonl"), str(proj / "s.txt"), str(proj / ".." / ".." / ".." / "x.jsonl"), ""):
    check(f"transcript {p!r}: not read", not coding.transcript_ok(p))
with mock.patch.object(coding.Path, "is_file", side_effect=AssertionError("touched the path")):
    check("git branch of a UNC cwd: no file access", coding.git_branch(r"\\lab-host\share\proj") == "")


print("Meow Coding hook server")
KEY = coding.hook_key()
check("hook key made once, 32 hex", len(KEY) == 32 and coding.hook_key() == KEY)
check("installed commands carry the key", KEY in coding.hook_command(8767) and KEY in coding.status_command(8767))
GOOD = {coding.HOOK_HEADER: KEY, "Content-Type": "application/json"}
EV = {"session_id": "t1", "hook_event_name": "SessionStart", "cwd": str(TMP)}


async def hook_server():
    tracker = coding.Tracker()
    with mock.patch.object(coding.web, "TCPSite") as site:  # no real port: drive the app through TestServer instead
        site.return_value.start = mock.AsyncMock()
        runner = await coding.start_server(tracker, 0)
    async with TestClient(TestServer(runner.app)) as c:
        cases = [("no key", {"Content-Type": "application/json"}, 403),
                 ("wrong key", {coding.HOOK_HEADER: "0" * 32, "Content-Type": "application/json"}, 403),
                 ("text/plain body", {coding.HOOK_HEADER: KEY, "Content-Type": "text/plain"}, 403),
                 ("from a web page (Origin)", {**GOOD, "Origin": "https://example.org"}, 403),
                 ("the installed hook", GOOD, 200)]
        for name, hdr, want in cases:
            r = await c.post("/hook", data=json.dumps(EV), headers=hdr)
            check(f"/hook {name} -> {want}", r.status == want)
        r = await c.post("/status", data="{}", headers={"Content-Type": "application/json"})
        check("/status without the key -> 403", r.status == 403)
    await runner.cleanup()
    return tracker

tr = asyncio.run(hook_server())
check("only the real hook made a session", list(tr.sessions) == ["t1"])


print("Meow Coding: hooks from an older agent are rewritten")
settings = TMP / "settings.json"
old_cmd = "curl -s -m 3 -o /dev/null --data-binary @- http://127.0.0.1:8767/hook || exit 0"
settings.write_text(json.dumps({"hooks": {"Stop": [{"hooks": [{"type": "command", "command": old_cmd}]}]},
                                "model": "keep-me"}), encoding="utf-8")
check("old hooks: rewritten", coding.refresh_hooks(8767, settings))
cfg = json.loads(settings.read_text(encoding="utf-8"))
check("new commands carry the key, other settings kept", KEY in json.dumps(cfg["hooks"]) and cfg["model"] == "keep-me")
check("current hooks: left alone", not coding.refresh_hooks(8767, settings))
settings.write_text(json.dumps({"model": "x"}), encoding="utf-8")
check("Meow Coding off: nothing installed", not coding.refresh_hooks(8767, settings)
      and json.loads(settings.read_text(encoding="utf-8")) == {"model": "x"})

shutil.rmtree(TMP, ignore_errors=True)
print("\nRESULT:", "ALL PASSED" if not FAIL else f"{len(FAIL)} FAILED: {FAIL}")
sys.exit(1 if FAIL else 0)
