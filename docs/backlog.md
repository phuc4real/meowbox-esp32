# Backlog

Work that is known and wanted but not planned for a version yet. When one is picked for a release, move it into that
release's notes and delete it here. Bigger product ideas (MeowBox Cloud, PC agent UI/UX, the rest of the theme pass)
live in [roadmap.md](roadmap.md).

## Features
- **Mini game with the cat.** Nothing exists yet (the cat tile has feed / play / bath / lights only).

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

## Hardware
- **Which "free" GPIOs reach pads on the PCB** (`docs/pinmap.md`, pins marked `free*`).
