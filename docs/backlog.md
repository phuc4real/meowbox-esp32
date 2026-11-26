# Backlog

Work that is known and wanted but not planned for a version yet. When one is picked for a release, move it into that
release's notes and delete it here. Bigger product ideas (MeowBox Cloud, PC agent UI/UX, the rest of the theme pass)
live in [roadmap.md](roadmap.md).

## Features
- **Mini game with the cat.** Nothing exists yet (the cat tile has feed / play / bath / lights only).

## Memory
- **Internal RAM back to ≥ 40 KB free at the lowest.** The goal in CLAUDE.md is 40 KB; the heaviest soak measured
  39.5 KB in 0.2.2 and 39.9 KB in 0.2.3 (`docs/perf/results-0.2.3.md`), so there is no margin, and larger draw buffers
  for the swipe FPS item below would need more. Look at what still sits in internal RAM (task stacks that could move
  to PSRAM, the decoder's 8 KB input buffer, LVGL draw buffers) with `tools/memwatch.py`.
  Also (measured 2026-11-26, same code and `sdkconfig`): the 0.2.3 release, built from IDF `release/v6.1` at
  `c43d665a`, has ~48 KB free when idle; built with the `v6.1.0` or `v6.1.1` tag it has ~12-14 KB (lowest 1.5-2.4 KB,
  DMA-capable free ~7 KB instead of ~40 KB). Find which of the 568 commits between `v6.1.1` and `c43d665a` makes the
  difference, so a later IDF update does not lose it again, and make `tools/release.py` refuse a tagged `v6.1.x` IDF.
  At `c43d665a` the build with the mixer, clap and PC-tile changes has 48.1 KB free when idle and 43.5 KB while
  streaming with notifications (0.2.3: 48.5 KB idle).

## Performance
- **Swipe at ≥ 25 FPS while the radio plays.** Measured 2026-11-26 (0.2.3, animated tile change every 2 s, MP3
  stream playing, no underrun): about 14-20 FPS in Swiss / Zen / Brutal, 12-17 in Cyberpunk; the slowest frames take
  50-85 ms. The ceiling is about 35 FPS: a full frame is 142 KB over SPI at 40 MHz (~28 ms), and 40 MHz is the limit
  because SCLK / MOSI (GPIO 9 / 10) are not the SPI IO MUX pins. Ideas: larger draw buffers (costs internal RAM,
  which has no margin; PSRAM + DMA is slower), drawing less while the tileview scrolls (skip the Cyberpunk scanlines
  and the cat during the animation), a shorter scroll animation.
- **Minify the web UI** (esbuild or similar) to get more room under the 30 KB first-load limit of `web/build.py`
  (29.6 KB in 0.2.3).

## Checks on the device
- **24 h soak run** with `/run-meowbox` (radio, notifications, theme changes) plus the 0.2.0 hands-on checklist:
  buttons, battery, alarms, the setup hotspot, an unfamiliar Wi-Fi.
- **Battery: run it flat once** to calibrate the % scale (`components/apps/battery_app.c`, table in
  `hw/ostb_xiaozhi_3st.h`).
- **Battery with Wi-Fi power save** on vs off while streaming (0.2.0 section 4.1), and the radio in a place with
  weak Wi-Fi.
- **PC agent installer** on a clean PC: install, uninstall, reinstall.
- **Firmware update edge cases** not tried yet (`docs/firmware_v0.2.0.md`, 3.2-3.5): a real rollback (an image that
  fails to start), Wi-Fi lost in the middle of the download, and the update refused with a low battery
  (`low_battery`, `components/apps/update_app.c`).
- **Clap level for the cat** (`clap_detect()` in `components/apps/system_app.c`): the thresholds (66 dB on the System
  tile scale, 20 dB over the background) are first guesses. Check that a clap at arm's length triggers it and that
  talking, a door or typing do not.

## Hardware
- **Which "free" GPIOs reach pads on the PCB** (`docs/pinmap.md`, pins marked `free*`).
