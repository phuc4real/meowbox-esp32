"""Mock MeowBox for testing the PC agent without hardware.

  python mock_speaker.py [--port 8080] [--token test] [--name MeowBox-MOCK] [--proto 2]
  curl "http://127.0.0.1:8080/mock/cmd?cmd=vol_up"   -> sends a media_cmd to the connected agent

Like the firmware (docs/firmware_v0.2.0.md, 2.4-2.8): GET /api/ping, the hello must carry the token (else
{"t":"auth"}), one agent at a time (another hello gets {"t":"busy"} unless it says "take":true),
POST /api/pc/relay_test. The agent finds no speaker by itself: give it the
mock's address (`--host 127.0.0.1:8080`). Two mocks on different ports + --name test the speaker-name check.
No mDNS / UDP discovery: the agent doesn't use them.
"""
import argparse
import asyncio
import json
import sys
import time
from pathlib import Path

import aiohttp
from aiohttp import WSMsgType, web
current: dict = {"ws": None, "name": "", "relay": ""}


def check_token(req: web.Request) -> None:
    if req.headers.get("Authorization") != f"Bearer {req.app['token']}" and req.query.get("token") != req.app["token"]:
        raise web.HTTPUnauthorized(text="bad token")


async def ws_pc(req: web.Request) -> web.WebSocketResponse:
    ws = web.WebSocketResponse(heartbeat=15)
    await ws.prepare(req)
    print(f"[ws] agent connected from {req.remote}", flush=True)
    admitted = False
    try:
        async for msg in ws:
            if msg.type != WSMsgType.TEXT:
                continue
            data = json.loads(msg.data)
            if not admitted:
                if data.get("t") != "hello":
                    break
                if int(data.get("proto", 0)) < req.app["proto_min"]:
                    await ws.send_json({"t": "proto", "need": req.app["proto_min"], "have": req.app["proto"]})
                    break
                if data.get("token") != req.app["token"]:  # like the firmware: the hello carries the notify token
                    print(f"[ws] auth: refusing {data.get('name')} (no or wrong token)", flush=True)
                    await ws.send_json({"t": "auth"})
                    break
                old = current["ws"]
                same = data.get("name") and data.get("name") == current["name"]
                if old is not None and not old.closed and not same:
                    if not data.get("take"):
                        print(f"[ws] busy: refusing {data.get('name')} (held by {current['name']})", flush=True)
                        await ws.send_json({"t": "busy", "pc": current["name"]})
                        break
                    print(f"[ws] {data.get('name')} takes over from {current['name']}", flush=True)
                    await old.send_json({"t": "busy", "pc": data.get("name", "?")})
                    await old.close()
                current.update(ws=ws, name=data.get("name", ""))
                admitted = True
            if data.get("t") == "hello":
                current["relay"] = data.get("relay", "")
            if data.get("t") != "stats" or not req.app["quiet"]:
                print(f"[ws] {msg.data}", flush=True)
    finally:
        if current["ws"] is ws:
            current.update(ws=None, name="")
        print("[ws] agent disconnected", flush=True)
    return ws


async def ping(req: web.Request) -> web.Response:
    a = req.app
    return web.json_response({"name": a["name"], "host": a["name"].lower(), "ver": "mock", "proto": a["proto"],
                              "proto_min": a["proto_min"], "mac": "02:00:00:00:00:01"})


async def relay_test(req: web.Request) -> web.Response:
    check_token(req)
    relay = current["relay"].split("://")[-1]
    if not relay:
        return web.json_response({"ok": False, "err": "no_relay"})
    t0 = time.monotonic()
    try:
        async with aiohttp.ClientSession() as s, s.get(f"http://{relay}/yt/ping", timeout=aiohttp.ClientTimeout(total=3)) as r:
            if r.status == 200:
                return web.json_response({"ok": True, "relay": relay, "ms": round((time.monotonic() - t0) * 1000)})
            return web.json_response({"ok": False, "relay": relay, "err": f"http_{r.status}"})
    except asyncio.TimeoutError:
        return web.json_response({"ok": False, "relay": relay, "err": "timeout"})
    except aiohttp.ClientError:
        return web.json_response({"ok": False, "relay": relay, "err": "unreachable"})


async def notify(req: web.Request) -> web.Response:
    check_token(req)
    body = {"text": req.query.get("text", "")} if req.method == "GET" else await req.json()
    print(f"[notify] {json.dumps(body, ensure_ascii=False)}", flush=True)
    return web.json_response({"ok": True, "queued": 1})


async def notify_history(req: web.Request) -> web.Response:
    check_token(req)
    return web.json_response({"items": []})


async def notify_audio(req: web.Request) -> web.Response:
    check_token(req)
    data = await req.read()
    if len(data) > 1024 * 1024:
        raise web.HTTPRequestEntityTooLarge(max_size=1024 * 1024, actual_size=len(data))
    out = Path(req.app["audio_dir"]) / f"notify_{int(time.time())}.{'wav' if data[:4] == b'RIFF' else 'mp3'}"
    out.write_bytes(data)
    print(f"[notify/audio] {len(data)} bytes {req.content_type} title={req.query.get('title')!r} -> {out}", flush=True)
    return web.json_response({"ok": True, "bytes": len(data)})


async def mock_cmd(req: web.Request) -> web.Response:
    cmd = req.query.get("cmd", "play_pause")
    ws = current["ws"]
    if ws is not None and not ws.closed:
        await ws.send_json({"t": "media_cmd", "cmd": cmd})
    print(f"[mock] sent media_cmd {cmd}", flush=True)
    return web.json_response({"sent": cmd, "agent": current["name"]})


def main() -> None:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--token", default="test")
    ap.add_argument("--name", default="MeowBox-MOCK")
    ap.add_argument("--proto", type=int, default=2, help="protocol the mock speaks")
    ap.add_argument("--proto-min", type=int, default=2, help="oldest agent protocol accepted")
    ap.add_argument("--quiet", action="store_true", help="don't print stats messages")
    ap.add_argument("--audio-dir", default=".")
    a = ap.parse_args()
    app = web.Application()
    app.update(port=a.port, token=a.token, quiet=a.quiet, audio_dir=a.audio_dir, name=a.name, proto=a.proto,
               proto_min=a.proto_min)
    app.add_routes([web.get("/ws/pc", ws_pc), web.get("/api/ping", ping), web.post("/api/notify", notify),
                    web.get("/api/notify", notify), web.get("/api/notify/history", notify_history),
                    web.post("/api/notify/audio", notify_audio), web.post("/api/pc/relay_test", relay_test),
                    web.get("/mock/cmd", mock_cmd)])
    web.run_app(app, port=a.port, print=lambda s: print(s, flush=True))


if __name__ == "__main__":
    main()
