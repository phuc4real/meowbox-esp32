"""Mock MeowBox for testing the PC agent without hardware.

  python mock_speaker.py [--port 8080] [--token test]
  curl "http://127.0.0.1:8080/mock/cmd?cmd=vol_up"   -> sends a media_cmd to every connected agent
"""
import argparse
import json
import socket
import sys
import time
from pathlib import Path

from aiohttp import WSMsgType, web
from zeroconf import IPVersion, ServiceInfo
from zeroconf.asyncio import AsyncZeroconf

clients: set[web.WebSocketResponse] = set()


def check_token(req: web.Request) -> None:
    if req.headers.get("Authorization") != f"Bearer {req.app['token']}" and req.query.get("token") != req.app["token"]:
        raise web.HTTPUnauthorized(text="bad token")


async def ws_pc(req: web.Request) -> web.WebSocketResponse:
    ws = web.WebSocketResponse(heartbeat=15)
    await ws.prepare(req)
    clients.add(ws)
    print(f"[ws] agent connected from {req.remote}", flush=True)
    try:
        async for msg in ws:
            if msg.type == WSMsgType.TEXT:
                data = json.loads(msg.data)
                if data.get("t") != "stats" or not req.app["quiet"]:
                    print(f"[ws] {msg.data}", flush=True)
    finally:
        clients.discard(ws)
        print("[ws] agent disconnected", flush=True)
    return ws


async def notify(req: web.Request) -> web.Response:
    if req.method == "GET":
        check_token(req)
        body = {"text": req.query.get("text", "")}
    else:
        check_token(req)
        body = await req.json()
    print(f"[notify] {json.dumps(body, ensure_ascii=False)}", flush=True)
    return web.json_response({"ok": True, "queued": 1})


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
    for ws in list(clients):
        await ws.send_json({"t": "media_cmd", "cmd": cmd})
    print(f"[mock] sent media_cmd {cmd} to {len(clients)} agent(s)", flush=True)
    return web.json_response({"sent": cmd, "agents": len(clients)})


def local_ips() -> list[str]:
    ips = {a[4][0] for a in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET)}
    return sorted(ips | {"127.0.0.1"})


async def start_mdns(app: web.Application):
    azc = AsyncZeroconf(ip_version=IPVersion.V4Only)
    info = ServiceInfo("_meowbox._tcp.local.", "MeowBox-MOCK._meowbox._tcp.local.", port=app["port"],
                       parsed_addresses=local_ips(), server="meowbox-mock.local.", properties={"ver": "mock"})
    await azc.async_register_service(info)
    print(f"[mdns] advertising _meowbox._tcp on {local_ips()}:{app['port']}", flush=True)
    yield
    await azc.async_unregister_service(info)
    await azc.async_close()


def main() -> None:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--token", default="test")
    ap.add_argument("--quiet", action="store_true", help="don't print stats messages")
    ap.add_argument("--no-mdns", action="store_true")
    ap.add_argument("--audio-dir", default=".")
    a = ap.parse_args()
    app = web.Application()
    app.update(port=a.port, token=a.token, quiet=a.quiet, audio_dir=a.audio_dir)
    app.add_routes([web.get("/ws/pc", ws_pc), web.post("/api/notify", notify), web.get("/api/notify", notify),
                    web.post("/api/notify/audio", notify_audio), web.get("/mock/cmd", mock_cmd)])
    if not a.no_mdns:
        app.cleanup_ctx.append(start_mdns)
    web.run_app(app, port=a.port, print=lambda s: print(s, flush=True))


if __name__ == "__main__":
    main()
