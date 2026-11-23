#!/usr/bin/env python3
"""Build the web bundle: web/src/{index.html,themes.css,app.css,app.js,lang/*.json} -> firmware/storage/web/*.gz (light minify + gzip -9).
Nothing else is copied: fonts (storage/web/fonts/) and theme previews (storage/web/themes/) are produced by other tools.

Usage: python build.py [--no-minify]
Stdlib only. Output is deterministic (gzip mtime = 0) so rebuilding does not change the LittleFS image.
"""
import argparse, glob, gzip, json, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "src")
OUT = os.path.normpath(os.path.join(HERE, "..", "storage", "web"))
FILES = ["index.html", "themes.css", "app.css", "app.js"] + sorted(
    os.path.relpath(f, SRC).replace(os.sep, "/") for f in glob.glob(os.path.join(SRC, "lang", "*.json")))
BUDGET = 90 * 1024     # uncompressed source budget
GZ_BUDGET = 30 * 1024  # gzipped first load: every non-lang file + the largest lang file (fonts are separate)


def min_css(s):
    s = re.sub(r"/\*.*?\*/", "", s, flags=re.S)
    s = re.sub(r"\s*\n\s*", "", s)
    s = re.sub(r"\s*([{};,>])\s*", r"\1", s)
    return s.replace(";}", "}")


def min_js(s):
    # Safe, line-based only: drop indentation, blank lines and whole-line // comments.
    # (No token-level rewriting: strings/regex/template literals stay untouched.)
    out = []
    for line in s.split("\n"):
        t = line.strip()
        if not t or t.startswith("//"):
            continue
        out.append(t)
    return "\n".join(out) + "\n"


def min_html(s):
    s = re.sub(r"<!--.*?-->", "", s, flags=re.S)
    return re.sub(r">\s+<", "><", s).strip()


def min_json(s):
    return json.dumps(json.loads(s), ensure_ascii=False, separators=(",", ":"))


MIN = {".css": min_css, ".js": min_js, ".html": min_html, ".json": min_json}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-minify", action="store_true")
    a = ap.parse_args()
    os.makedirs(os.path.join(OUT, "lang"), exist_ok=True)
    sizes = {}
    print(f"{'file':<16}{'src':>9}{'min':>9}{'gz':>9}   -> {OUT}")
    for name in FILES:
        src = open(os.path.join(SRC, name), encoding="utf-8").read()
        m = src if a.no_minify else MIN[os.path.splitext(name)[1]](src)
        raw = m.encode("utf-8")
        gz = gzip.compress(raw, compresslevel=9, mtime=0)
        with open(os.path.join(OUT, name + ".gz"), "wb") as f:
            f.write(gz)
        sizes[name] = (len(src.encode("utf-8")), len(raw), len(gz))
        print(f"{name:<16}{sizes[name][0]:>9}{sizes[name][1]:>9}{sizes[name][2]:>9}")
    # first page load = every non-language file + one language file (take the largest)
    core = [n for n in FILES if not n.startswith("lang/")]
    langs = [n for n in FILES if n.startswith("lang/")]
    first = core + ([max(langs, key=lambda n: sizes[n][2])] if langs else [])
    tot = [sum(sizes[n][i] for n in first) for i in range(3)]
    print(f"{'first load':<16}{tot[0]:>9}{tot[1]:>9}{tot[2]:>9}")
    # remove stale outputs
    for d in ("", "lang"):
        for f in os.listdir(os.path.join(OUT, d)):
            rel = (d + "/" + f) if d else f
            if f.endswith(".gz") and rel[:-3] not in FILES:
                os.remove(os.path.join(OUT, d, f))
                print("removed stale", rel)
    rc = 0
    if tot[0] > BUDGET:
        print(f"WARNING: first-load sources {tot[0]} B exceed budget {BUDGET} B", file=sys.stderr)
        rc = 1
    if tot[2] > GZ_BUDGET:
        print(f"ERROR: gzipped first load {tot[2]} B exceeds {GZ_BUDGET} B", file=sys.stderr)
        rc = 1
    return rc

if __name__ == "__main__":
    sys.exit(main())
