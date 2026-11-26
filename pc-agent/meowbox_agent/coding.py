"""Meow Coding: what Claude Code is doing, for the speaker's cat screen.

Claude Code sends every hook event to http://127.0.0.1:<coding_port>/hook: command hooks in ~/.claude/settings.json
(written by `meowbox coding install`) that run curl in the background (`async`) and always exit 0, so Claude Code
never waits for them and shows no "hook failed" when the agent is not running. Background hooks can arrive out of
order; event() keeps a late PostToolUse from undoing "done" or a late PreToolUse from hiding a permission prompt.
The tracker keeps one entry per session (state, kind of action, title) and reads the session's transcript for
tokens and context use. The core sends a summary to the speaker as {"t":"coding","s":[...]} whenever it changes (docs: firmware/web/API.md, /ws/pc protocol).

Claude Code's status line (also written by `meowbox coding install`, only when no other status line is set) POSTs
its input to /status: it carries the plan's usage limits (5-hour and weekly windows, in %) and the exact context use.
The answer is the status line text. API-key sessions have no plan limits.

Only short labels leave the PC: the session title, the project folder, the git branch and the kind of action
("read", "bash", "image"...; the speaker words it). No prompt text, commands, file names or contents.
"""
import json
import logging
import os
import re
import threading
import time
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path

from aiohttp import web

log = logging.getLogger("meowbox")

CLAUDE_SETTINGS = Path.home() / ".claude" / "settings.json"
DEFAULT_PORT = 8767
MAX_SESSIONS = 4          # sent to the speaker, most important first
WORK_STALE_S = 300        # "working" with no event for 5 min (interrupted with Esc: no Stop hook) -> idle
DONE_STALE_S = 600        # finished turn shown as done for 10 min, then idle
DROP_S = 3600             # no event for an hour -> forgotten
EVENTS = ["SessionStart", "UserPromptSubmit", "PreToolUse", "PostToolUse", "PostToolUseFailure", "PermissionRequest",
          "PermissionDenied", "Notification", "PreCompact", "Stop", "StopFailure", "SessionEnd"]
STATE_ORDER = {"wait": 0, "work": 1, "err": 2, "done": 3, "idle": 4}

SMALL_WINDOW = ("claude-haiku-4", "claude-3", "claude-opus-4-5", "claude-sonnet-4-5", "claude-opus-4-1",
                "claude-opus-4-2", "claude-sonnet-4-2")  # 200K context; the rest have 1M


def window(model: str) -> int:
    return 200_000 if model.startswith(SMALL_WINDOW) else 1_000_000


def model_label(model: str) -> str:
    """claude-opus-5-5 -> Opus 5.5"""
    m = re.match(r"claude-([a-z]+)-(\d+)(?:-(\d))?(?:\D|$)", model or "")
    if not m:
        return ""
    return m.group(1).capitalize() + " " + m.group(2) + ("." + m.group(3) if m.group(3) else "")


def basename(p: str) -> str:
    return os.path.basename(p.rstrip("/\\"))


def short(s: str, n: int) -> str:
    s = " ".join(str(s or "").split())
    return s if len(s) <= n else s[: n - 1] + "…"


IMAGE = (".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp", ".svg", ".ico")
KIND = {"Read": "read", "Edit": "edit", "MultiEdit": "edit", "NotebookEdit": "edit", "Write": "write",
        "Bash": "bash", "PowerShell": "bash", "Monitor": "bash", "Grep": "search", "Glob": "search", "LSP": "search",
        "WebFetch": "web", "WebSearch": "web", "Agent": "agent", "Task": "agent", "SendMessage": "agent",
        "TodoWrite": "plan", "EnterPlanMode": "plan", "ExitPlanMode": "plan", "AskUserQuestion": "ask",
        "Skill": "skill", "ToolSearch": "skill"}


def action(tool: str, inp: dict) -> str:
    """The kind of thing the agent is doing, for the speaker to word ("read" -> "Đang đọc file")."""
    inp = inp if isinstance(inp, dict) else {}
    if tool == "Read" and str(inp.get("file_path") or "").lower().endswith(IMAGE):
        return "image"
    if tool.startswith("mcp__"):
        return "web" if "browser" in tool or "chrome" in tool else "tool"
    return KIND.get(tool, "tool")


def git_branch(cwd: str) -> str:
    p = Path(cwd) if cwd else None
    for _ in range(8):
        if not p:
            return ""
        head = p / ".git"
        if head.is_file():  # worktree / submodule: "gitdir: <path>"
            try:
                head = Path(head.read_text(encoding="utf-8").split("gitdir:", 1)[1].strip())
            except (OSError, IndexError):
                return ""
        if (head / "HEAD").is_file():
            try:
                ref = (head / "HEAD").read_text(encoding="utf-8").strip()
            except OSError:
                return ""
            return ref.rsplit("/", 1)[-1] if ref.startswith("ref:") else ref[:7]
        if p.parent == p:
            return ""
        p = p.parent
    return ""


@dataclass
class Session:
    id: str
    cwd: str = ""
    branch: str = ""
    title: str = ""
    state: str = "idle"
    act: str = ""
    since: float = field(default_factory=time.monotonic)   # when the state changed
    stop_at: float = 0.0                                    # last Stop: tool events right after it are late ones
    last: float = field(default_factory=time.monotonic)    # last hook event
    transcript: str = ""
    # transcript totals
    offset: int = 0
    seen: set = field(default_factory=set)
    tokens: int = 0
    ctx: int = 0
    model: str = ""
    read_at: float = 0.0
    ctx_pct: float | None = None  # from the status line (exact); else estimated from the transcript

    def set_state(self, st: str) -> None:
        if st != self.state:
            self.state, self.since = st, time.monotonic()


class Tracker:
    def __init__(self):
        self._lock = threading.Lock()
        self.sessions: dict[str, Session] = {}
        self.limits: dict = {}  # {"h5": %, "h5r": reset epoch, "d7": %, "d7r": ...} from the status line

    # -- hook events (aiohttp handler thread = the core loop)
    def event(self, ev: dict) -> None:
        sid = str(ev.get("session_id") or "")
        name = ev.get("hook_event_name") or ""
        if not sid or not name:
            return
        with self._lock:
            s = self.sessions.get(sid)
            if name == "SessionEnd":
                self.sessions.pop(sid, None)
                return
            if s is None:
                s = self.sessions[sid] = Session(sid)
            s.last = time.monotonic()
            cwd = ev.get("cwd") or ""
            if cwd and (not s.cwd or name == "SessionStart"):  # the project: Bash `cd` moves the session's cwd later
                s.cwd, s.branch = cwd, git_branch(cwd)
            if ev.get("transcript_path"):
                s.transcript = ev["transcript_path"]
            tool = ev.get("tool_name") or ""
            now = time.monotonic()
            late = now - s.stop_at < 3  # a tool event that lost the race against Stop
            if name == "SessionStart":
                s.set_state("idle")
                s.act = ""
                s.branch = git_branch(s.cwd)
            elif name == "UserPromptSubmit":
                s.stop_at = 0.0
                s.set_state("work")
                s.act = ""
                s.branch = git_branch(s.cwd)  # a checkout since the last prompt
            elif name == "PreToolUse":
                if late:
                    return
                # AskUserQuestion waits for an answer; everything else is work, except right after a permission
                # prompt for this call (its PermissionRequest overtook this PreToolUse)
                if tool == "AskUserQuestion":
                    s.set_state("wait")
                elif not (s.state == "wait" and now - s.since < 2):
                    s.set_state("work")
                s.act = action(tool, ev.get("tool_input") or {})
            elif name in ("PostToolUse", "PostToolUseFailure", "PermissionDenied"):
                if late or s.state == "done":
                    return
                s.set_state("work")
            elif name == "PermissionRequest":
                s.set_state("wait")
                s.act = action(tool, ev.get("tool_input") or {}) if tool else s.act
            elif name == "Notification":
                kind = ev.get("notification_type") or ""
                if kind == "permission_prompt" or (not kind and "permission" in str(ev.get("message", "")).lower()):
                    s.set_state("wait")
            elif name == "PreCompact":
                s.set_state("work")
                s.act = "compact"
            elif name == "Stop":
                s.stop_at = now
                s.set_state("done")
                s.act = ""
            elif name == "StopFailure":
                s.set_state("err")
                s.act = ""

    # -- status line: plan usage limits, exact context use (core loop)
    def status(self, ev: dict) -> str:
        lim = {}
        rl = ev.get("rate_limits") or {}
        for key, short_key in (("five_hour", "h5"), ("seven_day", "d7")):
            w = rl.get(key) if isinstance(rl, dict) else None
            if not isinstance(w, dict):
                continue
            pct = w.get("used_percentage", w.get("utilization"))
            if isinstance(pct, (int, float)):
                lim[short_key] = round(pct)
                r = epoch(w.get("resets_at"))
                if r:
                    lim[short_key + "r"] = r
        sid = str(ev.get("session_id") or "")
        cw = ev.get("context_window") or {}
        with self._lock:
            if lim:
                self.limits = lim
            s = self.sessions.get(sid)
            if s is not None and isinstance(cw.get("used_percentage"), (int, float)):
                s.ctx_pct = cw["used_percentage"]
            parts = [f"5h {self.limits['h5']}%"] if "h5" in self.limits else []
            if "d7" in self.limits:
                parts.append(f"7d {self.limits['d7']}%")
            h5 = self.limits.get("h5", 0)
            face = "=^.^=" if h5 < 70 else "=o.o=" if h5 < 90 else "=x.x="  # the cat gets nervous near the limit
        return " ".join([face, "MeowBox", *(["·", " · ".join(parts)] if parts else [])])

    def limits_msg(self) -> dict:
        with self._lock:
            return dict(self.limits)

    # -- transcript: tokens, context, title (worker thread)
    def refresh_usage(self, force: bool = False) -> None:
        with self._lock:
            todo = [s for s in self.sessions.values() if s.transcript
                    and (force or time.monotonic() - s.read_at >= (3 if s.state in ("work", "wait") else 15))]
        for s in todo:
            s.read_at = time.monotonic()
            try:
                read_transcript(s)
            except OSError as e:
                log.debug("transcript %s: %s", s.transcript, e)

    # -- what goes to the speaker
    def snapshot(self) -> list[dict]:
        now = time.monotonic()
        out = []
        with self._lock:
            for sid in list(self.sessions):
                s = self.sessions[sid]
                quiet = now - s.last
                if quiet > DROP_S:
                    del self.sessions[sid]
                    continue
                if (s.state == "work" and quiet > WORK_STALE_S) or (s.state in ("done", "err") and quiet > DONE_STALE_S):
                    s.set_state("idle")
                    s.act = ""
            ss = sorted(self.sessions.values(), key=lambda x: (STATE_ORDER.get(x.state, 9), -x.last))
            for s in ss[:MAX_SESSIONS]:
                d = {"id": s.id[:8], "ag": "claude", "name": short(s.title, 48), "dir": short(basename(s.cwd), 32),
                     "br": short(s.branch, 32), "st": s.state, "act": s.act, "for": int(now - s.since),
                     "tok": s.tokens, "model": model_label(s.model)}
                if s.ctx_pct is not None:
                    d["ctx"] = min(100, round(s.ctx_pct))
                elif s.model:
                    d["ctx"] = min(100, round(100 * s.ctx / window(s.model)))
                out.append(d)
        return out


def epoch(v) -> int:
    """resets_at: epoch seconds or an ISO 8601 string -> epoch seconds (0 if unknown)."""
    if isinstance(v, (int, float)):
        return int(v / 1000) if v > 1e12 else int(v)
    if isinstance(v, str) and v:
        try:
            return int(datetime.fromisoformat(v.replace("Z", "+00:00")).timestamp())
        except ValueError:
            return 0
    return 0


def read_transcript(s: Session) -> None:
    """Read what was appended since last time. Usage is per API message; a message is split over several lines with
    the same id and the same usage, so it is counted once."""
    p = Path(s.transcript)
    size = p.stat().st_size
    if size < s.offset:  # rewritten
        s.offset, s.tokens, s.seen = 0, 0, set()
    if size == s.offset:
        return
    with p.open("rb") as f:
        f.seek(s.offset)
        data = f.read(size - s.offset)
    end = data.rfind(b"\n") + 1  # a line still being written waits for the next read
    s.offset += end
    for raw in data[:end].splitlines():
        if b'"usage"' not in raw and b'"ai-title"' not in raw:
            continue
        try:
            j = json.loads(raw)
        except ValueError:
            continue
        if j.get("type") == "ai-title":
            s.title = j.get("aiTitle") or s.title
            continue
        if j.get("type") != "assistant" or j.get("isSidechain"):
            continue
        m = j.get("message") or {}
        u = m.get("usage") or {}
        mid = m.get("id")
        if not u or not mid or mid in s.seen:
            continue
        s.seen.add(mid)
        model = m.get("model") or s.model
        if model and not model.startswith("<"):  # "<synthetic>" lines carry no real usage
            s.model = model
        inp, out = u.get("input_tokens", 0) or 0, u.get("output_tokens", 0) or 0
        cr, cw = u.get("cache_read_input_tokens", 0) or 0, u.get("cache_creation_input_tokens", 0) or 0
        s.tokens += inp + out + cr + cw
        s.ctx = inp + cr + cw


# ---------------------------------------------------------------- local HTTP endpoint for the hooks


async def start_server(tracker: Tracker, port: int) -> web.AppRunner:
    async def hook(req: web.Request) -> web.Response:
        if req.remote not in ("127.0.0.1", "::1"):
            raise web.HTTPForbidden()
        try:
            ev = await req.json()
        except ValueError:
            raise web.HTTPBadRequest() from None
        if isinstance(ev, dict):
            tracker.event(ev)
        return web.json_response({})  # empty answer: the hook changes nothing in Claude Code

    async def status(req: web.Request) -> web.Response:
        if req.remote not in ("127.0.0.1", "::1"):
            raise web.HTTPForbidden()
        try:
            ev = json.loads(await req.read() or b"{}")
        except ValueError:
            raise web.HTTPBadRequest() from None
        return web.Response(text=tracker.status(ev if isinstance(ev, dict) else {}))

    app = web.Application(client_max_size=4 * 1024 * 1024)  # PostToolUse carries the tool output
    app.add_routes([web.post("/hook", hook), web.post("/status", status)])
    runner = web.AppRunner(app, access_log=None)
    await runner.setup()
    await web.TCPSite(runner, "127.0.0.1", port).start()
    log.info("Meow Coding: Claude Code hooks on http://127.0.0.1:%d/hook", port)
    return runner


# ---------------------------------------------------------------- ~/.claude/settings.json


def hook_url(port: int) -> str:
    return f"http://127.0.0.1:{port}/hook"


def status_command(port: int) -> str:
    """Claude Code runs this with its status JSON on stdin and shows what it prints. Agent not running: an empty line."""
    return f"curl -s -m 1 --data-binary @- http://127.0.0.1:{port}/status"


def _is_our_status(sl) -> bool:
    return isinstance(sl, dict) and "127.0.0.1:" in str(sl.get("command", "")) and \
        str(sl.get("command", "")).rstrip().endswith("/status")


def hook_command(port: int) -> str:
    """Run in the background by Claude Code with the event on stdin. `|| exit 0`: no error when the agent is off."""
    return f"curl -s -m 3 -o /dev/null --data-binary @- {hook_url(port)} || exit 0"


def _is_ours(h: dict) -> bool:
    """Ours: the command hook above, or the HTTP hook of the first Meow Coding builds (replaced on install)."""
    target = str(h.get("url", "")) if h.get("type") == "http" else str(h.get("command", ""))
    return "http://127.0.0.1:" in target and "/hook" in target


def _read_settings(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return {}


def hooks_installed(path: Path = CLAUDE_SETTINGS) -> bool:
    try:
        hooks = _read_settings(path).get("hooks") or {}
    except (OSError, ValueError):
        return False
    return any(_is_ours(h) for groups in hooks.values() if isinstance(groups, list)
               for g in groups if isinstance(g, dict) for h in g.get("hooks", []) if isinstance(h, dict))


def _write_settings(path: Path, cfg: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        bak = path.with_name(path.name + ".meowbox.bak")
        if not bak.exists():  # keep the oldest copy: the file as it was before MeowBox touched it
            bak.write_bytes(path.read_bytes())
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(cfg, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    os.replace(tmp, path)


def _strip(hooks: dict) -> dict:
    out = {}
    for ev, groups in hooks.items():
        if not isinstance(groups, list):
            out[ev] = groups
            continue
        kept = []
        for g in groups:
            if isinstance(g, dict) and isinstance(g.get("hooks"), list):
                hs = [h for h in g["hooks"] if not (isinstance(h, dict) and _is_ours(h))]
                if not hs:
                    continue
                g = {**g, "hooks": hs}
            kept.append(g)
        if kept:
            out[ev] = kept
    return out


def install_hooks(port: int = DEFAULT_PORT, path: Path = CLAUDE_SETTINGS) -> None:
    """Add one background command hook per event (replacing earlier MeowBox ones, HTTP ones included); other hooks
    and settings stay as they are."""
    cfg = _read_settings(path)
    hooks = _strip(cfg.get("hooks") or {})
    for ev in EVENTS:
        hooks.setdefault(ev, []).append({"hooks": [{"type": "command", "command": hook_command(port), "async": True,
                                                    "timeout": 5}]})
    cfg["hooks"] = hooks
    sl = cfg.get("statusLine")
    if not sl or _is_our_status(sl):  # never replace someone's own status line
        cfg["statusLine"] = {"type": "command", "command": status_command(port), "padding": 0}
    else:
        log.info("Claude Code already has a status line: the 5-hour limit will not reach the speaker")
    _write_settings(path, cfg)


def uninstall_hooks(path: Path = CLAUDE_SETTINGS) -> None:
    cfg = _read_settings(path)
    if "hooks" not in cfg and not _is_our_status(cfg.get("statusLine")):
        return
    hooks = _strip(cfg.get("hooks") or {})
    if hooks:
        cfg["hooks"] = hooks
    else:
        cfg.pop("hooks", None)
    if _is_our_status(cfg.get("statusLine")):
        cfg.pop("statusLine")
    _write_settings(path, cfg)
