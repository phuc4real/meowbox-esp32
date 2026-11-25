# Performance results, v0.2.2 (2026-11-25)

Branch `firmware-v0.2.2` at `1b970cc` (all review fixes), measured with `firmware/tools/memwatch.py` every 5 s and a
load script. Home Wi-Fi, RSSI -59 to -65 dBm, PC agent connected, power management on (CPU 80-240 MHz). Same targets
as 0.2.0: internal RAM free ≥ 40 KB, largest block ≥ 16 KB, every stack ≥ 25 % or 768 B free.

## Heavy scenario, 12.6 min (`v022-heavy.csv`)

At the same time:
- radio, 2.5 min each: MANGORADIO (MP3 HTTPS), Lofi 24/7 (MP3 HTTP), Gensokyo (MP3 256k HTTPS), Jazz Sakura (MP3
  HTTP), a YouTube station through the PC relay (AAC); on each, a burst of 5 stop/play within 5 s;
- two web sessions polling `/api/status` every 2 s, one of them also taking `/api/screenshot` of a different tile
  every 10 s;
- `POST /api/notify` every 10 s and an audio notification (1 s WAV) every 60 s while the radio plays;
- a weather refresh every 60 s, a theme change (full UI rebuild) every 75 s, `POST /api/update/check` on the last
  station;
- every 5 s: did `taskLVGL` get CPU time in the last window (the decoder hang of 0.2.1 starved it)?

| | 0.2.0 | 0.2.2 |
|---|---|---|
| Internal RAM free (lowest) | 40.8 KB | **39.5 KB** (40.1 KB buffering, 41.3 KB stopped) |
| Largest free block (lowest) | 31 KB | **30 KB** |
| DMA free (lowest) | 32.7 KB | 31.7 KB |
| PSRAM free (lowest) | 7.34 MB | 7.49 MB |
| Underruns / reconnects | 0 / 0 | **0 / 0** on all five stations |
| Request errors | 0 | **0** of 674 status, 66 screenshots, 68 notifications, 11 audio notifications |
| UI starved (`taskLVGL` without CPU for 5 s) | (not checked) | **0** |
| Watchdog / panic / reset | none | **none** |

- Every audio notification played in full and the radio came back within 6 s.
- Slowest `/api/status`: 3.0 s, during a TLS handshake right after a stop/play burst; the rest answered in well under
  a second.
- A UI rebuild (theme change) takes 136-186 ms.
- Internal RAM is 1.3 KB under the 40 KB target at its lowest, while a stream plays with everything else running
  together; 0.2.2 adds power management, screen sleep and the UTF-8/overlay fixes. It recovers to 41 KB when the
  radio stops (below).

Peak CPU (one core = 100 %): `fetch` 65 % (TLS handshakes during the stop/play bursts), `taskLVGL` 53 % (UI rebuilds),
`decode` 16 %, `httpd` 11 %, `weather` 7 %.

## Idle with screen sleep, 6 min (`v022-idle-sleep.csv`)

`display.sleep_s` = 60, radio stopped. The screen went to sleep after 60 s (`screen sleep`, `amplifier off (idle)`,
`low power on` in the log). Internal RAM stayed at **40.8 KB** for the whole 6 minutes (no leak), largest block
32 KB; `taskLVGL` peaked at 5 %.

## Lowest free stack (bytes), whole run

| Task | Stack | Lowest free |
|---|---|---|
| `httpd` | 8192 | 1792 → **4640** after moving screenshots to the LVGL task (`5248862`) |
| `decode` | 4096 | 1844 (45 %) |
| `taskLVGL` | 8192 | 2300 (2348 after 40 screenshots of every theme and tile, day and night) |
| `weather` | 6144 | 2380 |
| `mic` | 4096 | 2404 |
| `sfx` | 4096 | 2472 |
| `battery` | 4096 | 2940 |
| `wifi_conn` | 4096 | 2144 |
| `discovery` (PSRAM) | 3072 | 1492 |
| `tcpip` | (IDF) | 1732 |

## Found and fixed by this run
- YouTube stations were refused (`503 busy`): a leftover `else return false` from the relay change in `6b4fc72`
  (`953c530`).
- Stop/play pressed rapidly on an HTTPS station ran several TLS handshakes at once on core 0 and tripped the idle
  task watchdog: one stream connects at a time now (`1b970cc`).
- An audio notification over the radio stopped halfway with `WAV_Parser: Not a WAV file`: a send started before the
  ring was flushed landed after it, putting old MP3 bytes in front of the clip. Sends now happen under the session
  lock (`1b970cc`). This also cut `meowbox say` from the PC agent.
- `/api/screenshot` drew the tile on the httpd stack (lowest free 1792 B, under the 25 % rule) (`5248862`).

The final firmware differs from the measured one only by `5248862` (screenshot handler), checked separately with
40 screenshots as above.
