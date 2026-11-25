# Performance results, v0.2.3 (2026-11-26)

Same load script and targets as [results-0.2.2.md](results-0.2.2.md), on branch `firmware-v0.2.3` before the version
bump. New in this build: the `diag` log hook on every error/warning line and a 2 s `esp_timer` check, the QR setup
card, the cat tile that redraws only on change.

## Heavy scenario, 12.6 min (`v023-heavy.csv`)

Five stations (MP3 HTTPS / HTTP, 256k MP3, YouTube AAC through the PC relay) with a stop/play burst on each, two web
sessions polling `/api/status` (one taking a screenshot every 10 s), a notification every 10 s and an audio
notification every 60 s, a weather refresh every 60 s, a theme change every 75 s, an update check.

| | 0.2.2 | 0.2.3 |
|---|---|---|
| Internal RAM free (lowest) | 39.5 KB | **39.9 KB** (40.1 KB buffering, 40.6 KB stopped) |
| Largest free block (lowest) | 30 KB | 31 KB |
| DMA free (lowest) | 31.7 KB | 32.1 KB |
| Underruns / reconnects at the end of each station | 0 / 0 | **0 / 0** |
| Request errors | 0 | **0** of 684 status, 66 screenshots, 68 notifications, 11 audio notifications |
| UI starved for 5 s | 0 | **0** |
| Watchdog / panic / reset | none | **none** |

Peak CPU: `fetch` 62 % (TLS during the stop/play bursts), `taskLVGL` 41 % (theme rebuilds), `decode` 15 %.
The serial log has a single `W player: underrun` (while switching stations) besides the two boot warnings; the new
error log (`GET /api/log`) recorded exactly that line.

## Idle with screen sleep, 6 min (`v023-idle-sleep.csv`)

Internal RAM flat at **40.8 KB**, largest block 32 KB, `taskLVGL` ≤ 6 %.

## Lowest free stack (bytes) after the run

| Task | 0.2.2 | 0.2.3 |
|---|---|---|
| `httpd` | 4640 | 3424 |
| `decode` | 1844 | 2388 |
| `taskLVGL` | 2300 | 2316 |
| `esp_timer` (now also runs the diag check) | 2848 | 2512 |
| `mic` | 2404 | 2196 |
| `weather` | 2380 | 2380 |
| `wifi_conn` | 2144 | 1960 |
| `tcpip` | 1732 | 1740 |

## UI cost (taskLVGL CPU, 15 s per tile, day mode)
Clock tile 2-5 % in every theme. Cat tile 14.5-18.3 % before the redraw fix, 4.4-5.6 % after. `fx` full vs eco
changes it by ≤ 1.5 points except Cyberpunk (+1.4 on the clock, +1 on the cat). Swipe speed while the radio plays:
14-20 FPS (Cyberpunk 12-17), see [../backlog.md](../backlog.md).
