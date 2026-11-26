# Performance results, v0.2.4 (2026-11-26)

Release build `4d8c568` (PROJECT_VER 0.2.4), built with ESP-IDF `release/v6.1` at `c43d665a` (see CLAUDE.md:
the `v6.1.0` / `v6.1.1` tags leave only ~12 KB internal RAM free when idle). Measured with
`firmware/tools/memwatch.py` every 5 s; home Wi-Fi, RSSI -54 to -62 dBm, PC agent connected.
New in this build: the mixer (UI sounds and notification chimes mixed over the radio, which is ducked), clap
detection on the mic task, the PC tile left out of the swipe order while no agent is connected.

## Heavy scenario, 12.5 min (`v024-heavy.csv`)

Five stations (four HTTP MP3 streams and one YouTube station through the PC relay), each with a burst of three
stop/play cycles, `/api/status` every 2 s, a screenshot every 10 s, a notification every 10 s (each chime now goes
through the mixer while the radio plays), an audio notification every 60 s, a theme change every 75 s, a frozen-UI
check every 60 s, an update check. One web session (0.2.3: two).

| | 0.2.3 | 0.2.4 |
|---|---|---|
| Internal RAM free (lowest) | 39.9 KB | **36.3 KB** in one 5 s sample (station 3, playing); every other sample ≥ 39.9 KB |
| Internal RAM free (median) | 40.6 KB | 40.3 KB |
| Largest free block (lowest) | 31 KB | 27 KB (same sample as the low) |
| DMA free (lowest) | 32.1 KB | 27.8 KB (same sample) |
| Request errors | 0 | **0** of 379 status, 75 screenshots, 75 notifications, 13 audio notifications, 36 radio commands |
| UI starved (frozen-UI check) | 0 | **0** of 13 |
| Watchdog / panic / reset | none | **none** (one boot, up 820 s at the end; the error log has only the two boot warnings) |

Peak CPU: `fetch` 52 % (TLS during the stop/play bursts), `decode` 15 %, `taskLVGL` 14 %, `sfx` < 1 %.

The single low sample needs a second run to tell whether it is new (a mixer chime, a screenshot and a TLS reconnect
at the same moment) or the same kind of short dip 0.2.3 could have had between its samples: see
[../backlog.md](../backlog.md), "Internal RAM back to ≥ 40 KB".

## Idle and light load (by hand, same build)

| | 0.2.3 release | 0.2.4 |
|---|---|---|
| Idle, screen on, 30-60 s after boot | 48.5 KB | 48.1 KB |
| Radio playing + three notifications in a row | – | 43.5 KB |

## Lowest free stack (bytes) after the heavy run

| Task | 0.2.3 | 0.2.4 |
|---|---|---|
| `httpd` | 3424 | 3716 |
| `decode` (now also mixes) | 2388 | 2344 |
| `taskLVGL` | 2316 | 2868 |
| `esp_timer` | 2512 | 2656 |
| `mic` (now also detects claps) | 2196 | 2088 |
| `sfx` | – | 2316 |
| `weather` | 2380 | 2544 |
| `wifi_conn` | 1960 | 1956 |
| `tcpip` | 1740 | 1736 |
