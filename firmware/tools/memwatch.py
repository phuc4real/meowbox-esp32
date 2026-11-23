#!/usr/bin/env python3
"""Poll GET /api/debug on the speaker, log memory and per-task numbers to CSV, print a summary at the end.

Usage:
  python tools/memwatch.py --host 192.168.1.130 --pass <web password> [--every 10] [--minutes 30] [--out perf.csv]
  MEOWBOX_PASS=<web password> python tools/memwatch.py --host meowbox-1a2b.local

Ctrl+C stops early and still prints the summary. Stdlib only. See docs/firmware_v0.2.0.md, section 5.1.
"""
import argparse, csv, http.cookiejar, json, os, sys, time, urllib.request


def opener(host, user, pw):
    jar = http.cookiejar.CookieJar()
    op = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar))
    req = urllib.request.Request(f"http://{host}/api/login", data=json.dumps({"user": user, "pass": pw}).encode(),
                                 headers={"Content-Type": "application/json"})
    op.open(req, timeout=5).read()
    return op


def get(op, host, path):
    with op.open(f"http://{host}{path}", timeout=5) as r:
        return json.loads(r.read())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--user", default="meowbox")
    ap.add_argument("--pass", dest="pw", default=os.environ.get("MEOWBOX_PASS", "m3owb0x2026"))
    ap.add_argument("--every", type=float, default=10)
    ap.add_argument("--minutes", type=float, default=0, help="0 = until Ctrl+C")
    ap.add_argument("--out", default="memwatch.csv")
    a = ap.parse_args()

    op = opener(a.host, a.user, a.pw)
    end = time.time() + a.minutes * 60 if a.minutes else None
    rows, prev, task_min, task_cpu = [], None, {}, {}
    cols = ["t", "int_free", "int_min", "int_big", "dma_free", "dma_big", "ps_free", "ps_min", "ps_big", "rssi", "play"]
    with open(a.out, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(cols + ["top_cpu"])
        try:
            while end is None or time.time() < end:
                try:
                    d = get(op, a.host, "/api/debug")
                    s = get(op, a.host, "/api/status")
                except urllib.error.HTTPError as e:
                    if e.code == 401:  # session expired or speaker rebooted
                        op = opener(a.host, a.user, a.pw)
                        continue
                    raise
                except OSError as e:
                    print("unreachable:", e, file=sys.stderr)
                    time.sleep(a.every)
                    continue
                cpu = {}
                if prev and d["rt_total"] > prev["rt_total"]:
                    pr = {t["name"]: t["rt"] for t in prev["tasks"]}
                    dt = d["rt_total"] - prev["rt_total"]
                    # rt_total counts one core; tasks on both cores share it, so 2 cores = up to 200 %
                    cpu = {t["name"]: 100.0 * (t["rt"] - pr.get(t["name"], t["rt"])) / dt for t in d["tasks"]}
                for t in d["tasks"]:
                    n = t["name"]
                    task_min[n] = min(task_min.get(n, 1 << 30), t["stack_free"])
                    if n in cpu:
                        task_cpu[n] = max(task_cpu.get(n, 0.0), cpu[n])
                top = sorted(((v, k) for k, v in cpu.items() if not k.startswith("IDLE")), reverse=True)[:3]
                row = [round(d["up_ms"] / 1000), d["int"]["free"], d["int"]["min"], d["int"]["big"], d["dma"]["free"],
                       d["dma"]["big"], d["psram"]["free"], d["psram"]["min"], d["psram"]["big"], s.get("rssi"),
                       s.get("play", {}).get("st")]
                w.writerow(row + [" ".join(f"{k}:{v:.0f}%" for v, k in top)])
                f.flush()
                rows.append(dict(zip(cols, row)))
                print(" ".join(f"{c}={v}" for c, v in zip(cols, row)), flush=True)
                prev = d
                time.sleep(a.every)
        except KeyboardInterrupt:
            pass

    if not rows:
        return 1
    print(f"\n== {len(rows)} samples -> {a.out}")
    for c in cols[1:9]:
        v = [r[c] for r in rows]
        print(f"{c:<9} min {min(v):>9}  max {max(v):>9}")
    print("\n== tasks: lowest free stack (bytes), peak CPU %")
    stacks = {t["name"]: t for t in prev["tasks"]}
    for n in sorted(task_min, key=task_min.get):
        t = stacks.get(n, {})
        print(f"{n:<16} core {t.get('core', '?'):>2}  stack_free {task_min[n]:>6}{'  (PSRAM)' if t.get('stack_ext') else ''}"
              f"  cpu_peak {task_cpu.get(n, 0):5.1f}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
