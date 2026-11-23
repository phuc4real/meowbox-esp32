#!/usr/bin/env python3
"""Build the web UI into one pack that is embedded in the firmware app (components/webui/web.pack).

Inputs:
  web/src/index.html, themes.css, app.css, app.js       core files
  web/src/lang/<code>.json                               UI strings, one file per language
  web/src/theme/<id>.css                                 per-theme flourishes, only the active one is loaded
  web/assets/fonts/*, web/assets/themes/*.png            fonts (tools/fonts/gen_theme_fonts.py) and theme previews

Text files are lightly minified. Everything except index.html and the assets gets a content hash in its name
(app.3f9a1c2b.js) so the speaker can send it with "Cache-Control: immutable"; index.html is rewritten to point at
the hashed names and gets window.MB, the language/theme -> file map used by index.html and app.js.
Files that shrink with gzip are stored gzipped. Every file carries an ETag (hash of the stored bytes).

Pack layout (little-endian), read by components/webui/webui.c and mock_server.py --dist:
  "MBWP" u32 version=1 u32 count u32 data_offset
  count x { char path[56] (no leading '/', NUL-padded); u32 offset (from data_offset); u32 length;
            u32 flags (1 = gzip, 2 = immutable); char etag[12] (NUL-padded) }
  data

Usage: python build.py [--no-minify] [--dist DIR]   (--dist also writes the served files to DIR for inspection)
Stdlib only. Output is deterministic (gzip mtime = 0), so an unchanged UI does not change the firmware image.
"""
import argparse, glob, gzip, hashlib, json, os, re, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "src")
ASSETS = os.path.join(HERE, "assets")
PACK = os.path.normpath(os.path.join(HERE, "..", "components", "webui", "web.pack"))
GZ_BUDGET = 30 * 1024   # gzipped first page load: core files + one language + one theme file
PATH_MAX = 56
F_GZIP, F_IMMUTABLE = 1, 2


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


def rel(path, root):
    return os.path.relpath(path, root).replace(os.sep, "/")


def hashed(name, data):
    base, ext = os.path.splitext(name)
    return f"{base}.{hashlib.sha256(data).hexdigest()[:8]}{ext}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-minify", action="store_true")
    ap.add_argument("--dist", help="also write the served files to this directory")
    a = ap.parse_args()

    def text(name):
        src = open(os.path.join(SRC, name), encoding="utf-8").read()
        m = src if a.no_minify else MIN[os.path.splitext(name)[1]](src)
        return len(src.encode("utf-8")), m.encode("utf-8")

    srcsize, out = {}, {}   # out: served path -> raw bytes
    rename = {}             # source name -> served (hashed) name
    names = ["themes.css", "app.css", "app.js"]
    names += sorted(rel(f, SRC) for f in glob.glob(os.path.join(SRC, "lang", "*.json")))
    names += sorted(rel(f, SRC) for f in glob.glob(os.path.join(SRC, "theme", "*.css")))
    for n in names:
        srcsize[n], raw = text(n)
        rename[n] = hashed(n, raw)
        out[rename[n]] = raw

    # index.html: point at hashed names, add the language/theme map
    srcsize["index.html"], idx = text("index.html")
    idx = idx.decode("utf-8")
    for n in ("themes.css", "app.css", "app.js"):
        idx, k = re.subn(r'(href|src)="%s"' % re.escape(n), r'\1="%s"' % rename[n], idx)
        if k != 1:
            sys.exit(f"index.html must reference {n} exactly once")
    mb = {"l": {os.path.splitext(os.path.basename(n))[0]: rename[n] for n in names if n.startswith("lang/")},
          "t": {os.path.splitext(os.path.basename(n))[0]: rename[n] for n in names if n.startswith("theme/")}}
    if idx.count("/*MB*/") != 1:
        sys.exit("index.html needs one /*MB*/ marker in its inline script")
    idx = idx.replace("/*MB*/", "window.MB=" + json.dumps(mb, separators=(",", ":")) + ";")
    out["index.html"] = idx.encode("utf-8")

    for f in sorted(glob.glob(os.path.join(ASSETS, "**", "*"), recursive=True)):
        if os.path.isfile(f):
            out[rel(f, ASSETS)] = open(f, "rb").read()

    # pack
    entries, blob = [], bytearray()
    print(f"{'served as':<34}{'src':>8}{'raw':>8}{'stored':>8}  flags")
    stored_size = {}
    for path in sorted(out, key=lambda p: (p != "index.html", p)):
        raw = out[path]
        if len(path) >= PATH_MAX:
            sys.exit(f"path too long for the pack: {path}")
        flags = F_IMMUTABLE if path in rename.values() else 0
        data = raw
        if os.path.splitext(path)[1] in (".html", ".css", ".js", ".json", ".svg"):
            gz = gzip.compress(raw, compresslevel=9, mtime=0)
            if len(gz) < len(raw):
                data, flags = gz, flags | F_GZIP
        etag = hashlib.sha256(data).hexdigest()[:11]
        entries.append((path, len(blob), len(data), flags, etag))
        stored_size[path] = len(data)
        blob += data
        while len(blob) % 4:
            blob += b"\0"
        src_n = next((srcsize[k] for k, v in rename.items() if v == path), srcsize.get(path, len(raw)))
        print(f"{path:<34}{src_n:>8}{len(raw):>8}{len(data):>8}  {'gz ' if flags & F_GZIP else '   '}"
              f"{'immutable' if flags & F_IMMUTABLE else ''}")
    head = struct.pack("<4sIII", b"MBWP", 1, len(entries), 16 + 80 * len(entries))
    table = b"".join(struct.pack("<56sIII12s", p.encode(), o, n, fl, e.encode()) for p, o, n, fl, e in entries)
    pack = head + table + bytes(blob)
    os.makedirs(os.path.dirname(PACK), exist_ok=True)
    old = open(PACK, "rb").read() if os.path.exists(PACK) else None
    if old != pack:
        open(PACK, "wb").write(pack)
    print(f"pack: {len(entries)} files, {len(pack)} B -> {PACK}{'' if old != pack else ' (unchanged)'}")

    if a.dist:
        for path, raw in out.items():
            fp = os.path.join(a.dist, path)
            os.makedirs(os.path.dirname(fp), exist_ok=True)
            open(fp, "wb").write(raw)

    # first page load: index + core + the largest language file + the largest theme file
    first = ["index.html"] + [rename[n] for n in ("themes.css", "app.css", "app.js")]
    for pre in ("lang/", "theme/"):
        cand = [rename[n] for n in names if n.startswith(pre)]
        if cand:
            first.append(max(cand, key=lambda p: stored_size[p]))
    tot = sum(stored_size[p] for p in first)
    print(f"first page load (gzip): {tot} B  ({', '.join(first)})")
    if tot > GZ_BUDGET:
        print(f"ERROR: first page load {tot} B exceeds {GZ_BUDGET} B", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
