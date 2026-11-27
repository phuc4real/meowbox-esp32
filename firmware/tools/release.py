#!/usr/bin/env python3
"""Prepare a MeowBox release (docs/firmware_v0.2.0.md, 3.2): manifest.json + meowbox-<ver>.bin [+ agent installer].

Run from firmware/ after `python web/build.py` and `idf.py build`:

  python tools/release.py --notes release_notes.json                 # writes dist/release/<ver>/, nothing published
  python tools/release.py --notes release_notes.json --publish       # + gh release create v<ver> (repo must be clean)
  python tools/release.py --notes n.json --base-url http://192.168.1.155:8000/   # local test server instead of GitHub

release_notes.json: {"vi": ["Giao diện web nhẹ hơn", ...], "en": ["Lighter web UI", ...], "min_ver": "0.1.0"}
(short lines shown on the web page; the long notes go in the GitHub release body, see --body).

The version comes from PROJECT_VER in CMakeLists.txt and must match the one inside build/meowbox.bin. The speaker
reads releases/latest/download/manifest.json, checks app.size and app.sha256 of what it wrote to flash, and only then
switches to the new image. Stdlib only (+ the gh CLI for --publish).
"""
import argparse, hashlib, json, os, re, shutil, struct, subprocess, sys, time

FW = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
REPO = "phuc4real/meowbox-esp32"
PROTO = 2  # PC agent <-> speaker protocol (docs/firmware_v0.2.0.md, 2.6); bump together with the agent (MB_PROTO)


def project_ver():
    m = re.search(r'set\(PROJECT_VER\s+"([^"]+)"\)', open(os.path.join(FW, "CMakeLists.txt"), encoding="utf-8").read())
    if not m:
        sys.exit("PROJECT_VER not found in CMakeLists.txt")
    return m.group(1)


def bin_version(path):
    """esp_app_desc_t sits right after the image header + first segment header (offset 0x20); version at +16."""
    b = open(path, "rb").read(0x20 + 256)
    magic = struct.unpack_from("<I", b, 0x20)[0]
    if b[0] != 0xE9 or magic != 0xABCD5432:
        sys.exit(f"{path}: not an ESP app image with an app descriptor")
    return b[0x20 + 16:0x20 + 48].split(b"\0")[0].decode()


def git(*a):
    return subprocess.run(["git", *a], cwd=FW, capture_output=True, text=True).stdout.strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--notes", required=True, help="release_notes.json with vi/en short lines (and optional min_ver)")
    ap.add_argument("--bin", default=os.path.join(FW, "build", "meowbox.bin"))
    ap.add_argument("--agent", help="MeowBoxAgent-Setup-<ver>.exe to attach")
    ap.add_argument("--base-url", help="where the files will be served (default: the GitHub release download URL)")
    ap.add_argument("--body", help="markdown file for the GitHub release body")
    ap.add_argument("--publish", action="store_true", help="create the GitHub release with gh (asks nothing)")
    a = ap.parse_args()

    ver = project_ver()
    bver = bin_version(a.bin)
    if bver != ver:
        sys.exit(f"build/meowbox.bin is {bver!r} but PROJECT_VER is {ver!r}: rebuild")
    notes = json.load(open(a.notes, encoding="utf-8"))
    if not (isinstance(notes.get("vi"), list) and isinstance(notes.get("en"), list)):
        sys.exit("notes need 'vi' and 'en' lists")
    tag = f"v{ver}"
    base = a.base_url or f"https://github.com/{REPO}/releases/download/{tag}/"
    if not base.endswith("/"):
        base += "/"

    data = open(a.bin, "rb").read()
    name = f"meowbox-{ver}.bin"
    manifest = {
        "ver": ver, "proto": PROTO, "date": time.strftime("%Y-%m-%d"),
        "notes": {"vi": notes["vi"], "en": notes["en"]},
        "app": {"file": name, "url": base + name, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()},
        "min_ver": notes.get("min_ver", "0.1.0"),
        "page": f"https://github.com/{REPO}/releases/tag/{tag}",
        "commit": git("rev-parse", "--short", "HEAD"),
    }
    out = os.path.join(FW, "dist", "release", ver)
    os.makedirs(out, exist_ok=True)
    shutil.copyfile(a.bin, os.path.join(out, name))
    files = [os.path.join(out, name), os.path.join(out, "manifest.json")]
    if a.agent:
        shutil.copy(a.agent, out)
        files.append(os.path.join(out, os.path.basename(a.agent)))
    json.dump(manifest, open(files[1], "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    print(f"{tag}: {name} {len(data)} B sha256 {manifest['app']['sha256'][:16]}... -> {out}")

    if a.publish:
        if git("status", "--porcelain", "--untracked-files=no"):
            sys.exit("working tree has uncommitted changes: commit first")
        if git("tag", "-l", tag):
            sys.exit(f"tag {tag} already exists")
        cmd = ["gh", "release", "create", tag, *files, "--repo", REPO, "--title", f"MeowBox {ver}"]
        cmd += ["--notes-file", a.body] if a.body else ["--notes", "\n".join("- " + x for x in notes["vi"])]
        subprocess.run(cmd, check=True)
        print("published", manifest["page"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
