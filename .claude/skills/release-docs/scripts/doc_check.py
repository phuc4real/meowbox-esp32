#!/usr/bin/env python3
"""Mechanical doc checks before a MeowBox release. Prints a Markdown report; exit 0 always (it informs, the agent
decides). Run from anywhere inside the repo:

  py -3.14 .claude/skills/release-docs/scripts/doc_check.py [--since <tag or commit>]

Sections:
  1. Range: commits since the last release tag, grouped by area.
  2. Areas that changed while the docs that describe them did not (candidates, not verdicts).
  3. Broken relative links in tracked Markdown files.
  4. API.md vs the firmware's web_register() routes (and mock_server.py routes).
  5. Web UI strings: keys in vi.json and en.json must match.
  6. PC agent CLI commands named in the docs vs the argparse subcommands.
  7. Versions: firmware PROJECT_VER vs agent __version__, and old version numbers named in present-tense docs.
"""
import argparse
import json
import os
import re
import subprocess
import sys

ROOT = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True).stdout.strip()

# area -> (path prefixes, docs that should follow a change there)
AREAS = {
    "firmware UI / apps (what the speaker shows and does)": (
        ["firmware/components/ui/", "firmware/components/apps/", "firmware/main/", "firmware/themes/"],
        ["README_vi.md", "firmware/README.md", "README.md"]),
    "firmware player / audio / net": (
        ["firmware/components/player/", "firmware/components/audio/", "firmware/components/net/"],
        ["firmware/README.md", "README_vi.md"]),
    "REST API handlers": (
        ["firmware/components/net/web.c", "firmware/components/apps/"],
        ["firmware/web/API.md"]),
    "web UI": (["firmware/web/src/"], ["firmware/web/README.md", "README_vi.md"]),
    "hardware facts": (["hw/", "firmware/components/board/"], ["docs/pinmap.md", "docs/hardware.md"]),
    "PC agent": (["pc-agent/"], ["pc-agent/README.md", "README_vi.md"]),
    "build / config / partitions": (
        ["firmware/sdkconfig.defaults", "firmware/partitions", "firmware/CMakeLists.txt", "firmware/main/idf_component.yml"],
        ["firmware/README.md", "CLAUDE.md"]),
    "tools": (["tools/", "firmware/tools/"], ["firmware/README.md", "CLAUDE.md", "README.md"]),
    "performance results": (["docs/perf/"], ["CLAUDE.md", "README.md"]),
}
SKIP_DIRS = ("node_modules", "managed_components", "/build/", "dist/")


def git(*a):
    return subprocess.run(["git", *a], capture_output=True, text=True, encoding="utf-8", cwd=ROOT).stdout


def read(p):
    try:
        return open(os.path.join(ROOT, p), encoding="utf-8").read()
    except OSError:
        return ""


def section(title):
    print(f"\n## {title}\n")


def range_and_areas(since):
    section(f"1. Changes since `{since}`")
    log = git("log", "--format=%h %s", f"{since}..HEAD").strip().splitlines()
    print(f"{len(log)} commits.\n")
    for line in log:
        print(f"- {line}")
    files = set(git("diff", "--name-only", f"{since}..HEAD").split())
    section("2. Areas changed, docs untouched (check each: did the change alter what the doc says?)")
    any_hit = False
    for area, (prefixes, docs) in AREAS.items():
        changed = sorted(f for f in files if any(f.startswith(p) for p in prefixes))
        if not changed:
            continue
        stale = [d for d in docs if d not in files and os.path.exists(os.path.join(ROOT, d))]
        if stale:
            any_hit = True
            shown = ", ".join(f"`{c}`" for c in changed[:6]) + (f" (+{len(changed) - 6})" if len(changed) > 6 else "")
            print(f"- **{area}**: {shown}\n  - not touched: {', '.join(f'`{d}`' for d in stale)}")
    if not any_hit:
        print("None.")
    return files


def md_files():
    return [f for f in git("ls-files", "*.md").split() if not any(s in f for s in SKIP_DIRS)]


def links():
    section("3. Broken relative links")
    bad = []
    for f in md_files():
        text = read(f)
        for m in re.finditer(r"\]\(([^)\s]+)\)", text):
            target = m.group(1).split("#")[0]
            if not target or re.match(r"^[a-z]+:", target):
                continue
            p = os.path.normpath(os.path.join(ROOT, os.path.dirname(f), target))
            if not os.path.exists(p):
                bad.append(f"- `{f}` -> `{m.group(1)}`")
    print("\n".join(bad) if bad else "None.")


def api():
    section("4. API.md vs firmware routes")
    code = set()
    for f in git("ls-files", "firmware/components", "firmware/main").split():
        if f.endswith(".c"):
            for m in re.finditer(r'web_register(?:_ws)?\("(/[^"]+)"(?:,\s*HTTP_([A-Z]+))?', read(f)):
                code.add((m.group(2) or "GET", m.group(1)))
    doc = read("firmware/web/API.md")
    groups = re.findall(r'\{"(\w+)",\s*"\{', read("firmware/components/settings/settings.c"))
    documented = set(re.findall(r"`(GET|POST|PUT|DELETE) (/api/[^`?\s]+)", doc))
    missing = sorted(f"`{m} {p}`" for m, p in code if "*" not in p and p not in doc)
    for m, p in sorted(documented):
        if p.startswith("/api/config/"):
            g = p.rsplit("/", 1)[1]
            ok = any(cp == "/api/config/*" and cm == m for cm, cp in code) and (g in groups or g in ("wifi", "pc", "update"))
        else:
            ok = (m, p) in code
        if not ok:
            print(f"- documented, not registered in firmware: `{m} {p}`")
    for x in missing:
        print(f"- registered in firmware, not named in API.md: {x}")
    mock = read("firmware/web/mock_server.py")
    mock_routes = set(re.findall(r'\("(GET|POST|PUT|DELETE)", "(/api/[^"]+)"\)', mock))
    for m, p in sorted(documented):
        if "/config/" in p or (m, p) in mock_routes or p in mock:
            continue
        print(f"- documented, not in mock_server.py: `{m} {p}` (CLAUDE.md: keep the mock in step)")
    print("\n(no other differences)")


def lang():
    section("5. Web UI strings vi.json / en.json")
    try:
        vi = json.loads(read("firmware/web/src/lang/vi.json"))
        en = json.loads(read("firmware/web/src/lang/en.json"))
    except ValueError as e:
        print(f"- JSON error: {e}")
        return
    only_vi, only_en = sorted(set(vi) - set(en)), sorted(set(en) - set(vi))
    for k in only_vi:
        print(f"- only in vi.json: `{k}`")
    for k in only_en:
        print(f"- only in en.json: `{k}`")
    if not (only_vi or only_en):
        print(f"Same {len(vi)} keys.")


def cli():
    section("6. PC agent CLI in the docs")
    src = read("pc-agent/meowbox_agent/cli.py")
    cmds = set(re.findall(r'add_parser\("([\w-]+)"', src))
    cmds |= set(re.findall(r'argv\[:1\] == \["([\w-]+)"\]', src))  # hidden commands handled before argparse
    print(f"Commands: {', '.join(sorted(cmds))}\n")
    hits = []
    for f in md_files():
        if f.startswith(".claude/skills/release-docs/"):
            continue
        for n, line in enumerate(read(f).splitlines(), 1):
            for m in re.finditer(r"\bmeowbox(?:\.exe)? ([a-z][\w-]+)", line):
                c = m.group(1)
                if c not in cmds and not c.endswith((".local", "-xxxx")) and c not in ("agent", "box", "firmware"):
                    hits.append(f"- `{f}:{n}`: `meowbox {c}` is not a command")
    print("\n".join(hits) if hits else "All named commands exist.")


def versions():
    section("7. Versions")
    fw = re.search(r'PROJECT_VER\s+"([^"]+)"', read("firmware/CMakeLists.txt"))
    ag = re.search(r'__version__\s*=\s*"([^"]+)"', read("pc-agent/meowbox_agent/__init__.py"))
    fw, ag = fw.group(1) if fw else "?", ag.group(1) if ag else "?"
    print(f"- firmware PROJECT_VER `{fw}`, agent `__version__` `{ag}`" + ("" if fw == ag else "  **MISMATCH**"))
    tags = git("tag", "--sort=-v:refname").split()
    print(f"- latest tag: `{tags[0] if tags else 'none'}`")
    cur = tuple(int(x) for x in re.findall(r"\d+", fw)[:3]) if fw != "?" else None
    print("\nLines in present-tense docs naming an older version (often fine: history, links; check each):\n")
    for f in ("README.md", "README_vi.md", "firmware/README.md", "pc-agent/README.md", "CLAUDE.md", "firmware/web/README.md"):
        for n, line in enumerate(read(f).splitlines(), 1):
            for v in re.findall(r"\b(\d+\.\d+\.\d+)\b", line):
                t = tuple(int(x) for x in v.split("."))
                if cur and t < cur and t[0] == 0:
                    print(f"- `{f}:{n}`: {line.strip()[:140]}")
                    break


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--since", help="tag or commit (default: the latest tag)")
    a = ap.parse_args()
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    since = a.since or git("describe", "--tags", "--abbrev=0").strip() or git("rev-list", "--max-parents=0", "HEAD").strip()
    print(f"# Doc check ({git('rev-parse', '--short', 'HEAD').strip()})")
    range_and_areas(since)
    links()
    api()
    lang()
    cli()
    versions()


if __name__ == "__main__":
    main()
