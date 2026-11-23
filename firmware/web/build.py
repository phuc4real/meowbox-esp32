#!/usr/bin/env python3
"""Build the web bundle: web/src/{index.html,themes.css,app.css,app.js} -> firmware/storage/web/*.gz (light minify + gzip -9).
Nothing else is copied: fonts (storage/web/fonts/) and theme previews (storage/web/themes/) are produced by other tools.

Usage: python build.py [--no-minify]
Stdlib only. Output is deterministic (gzip mtime = 0) so rebuilding does not change the LittleFS image.
"""
import argparse, gzip, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "src")
OUT = os.path.normpath(os.path.join(HERE, "..", "storage", "web"))
FILES = ["index.html", "themes.css", "app.css", "app.js"]
BUDGET = 90 * 1024     # uncompressed source budget
GZ_BUDGET = 30 * 1024  # gzipped total of FILES (fonts are separate)


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


MIN = {".css": min_css, ".js": min_js, ".html": min_html}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-minify", action="store_true")
    a = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    tot = [0, 0, 0]
    print(f"{'file':<12}{'src':>9}{'min':>9}{'gz':>9}   -> {OUT}")
    for name in FILES:
        src = open(os.path.join(SRC, name), encoding="utf-8").read()
        m = src if a.no_minify else MIN[os.path.splitext(name)[1]](src)
        raw = m.encode("utf-8")
        gz = gzip.compress(raw, compresslevel=9, mtime=0)
        with open(os.path.join(OUT, name + ".gz"), "wb") as f:
            f.write(gz)
        n = (len(src.encode("utf-8")), len(raw), len(gz))
        tot = [x + y for x, y in zip(tot, n)]
        print(f"{name:<12}{n[0]:>9}{n[1]:>9}{n[2]:>9}")
    print(f"{'total':<12}{tot[0]:>9}{tot[1]:>9}{tot[2]:>9}")
    # remove stale outputs
    for f in os.listdir(OUT):
        if f.endswith(".gz") and f[:-3] not in FILES:
            os.remove(os.path.join(OUT, f))
            print("removed stale", f)
    rc = 0
    if tot[0] > BUDGET:
        print(f"WARNING: sources {tot[0]} B exceed budget {BUDGET} B", file=sys.stderr)
        rc = 1
    if tot[2] > GZ_BUDGET:
        print(f"ERROR: gzipped total {tot[2]} B exceeds {GZ_BUDGET} B", file=sys.stderr)
        rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
