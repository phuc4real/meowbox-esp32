# Swipe FPS while the radio plays

Goal (backlog, 0.2.3): ≥ 25 FPS on a tile swipe with an MP3 stream playing. Ceiling ~35 FPS: a full frame is 142 KB
over SPI at 40 MHz (~28 ms).

## How it is measured
1. Set `UI_SWIPE_BENCH` to 1 (top of the bench block in `components/ui/ui.c`) and build. The speaker then swipes to the next tile every 2 s and logs
   `swipe: N frames in T ms = F fps, slowest S ms, theme X, tile A-B, median M ms` per animation. F is
   1000 / median frame interval, leaving out the first frame (scroll start to first refresh). Frames ÷ total time
   was the first metric tried and it penalizes a shorter animation for its fixed first-frame cost.
   The tile pair is the *destination* twice: LVGL sets the active tile when the animation starts.
2. Capture the serial log (`/run-meowbox`, `driver.py serial --grep "swipe:|player"`; opening COM10 reboots the
   speaker), then over the API: night mode off, volume low, play an MP3 station (MANGORADIO, 128 kbps), switch theme
   every ~25 s (≈ 12 swipes each, 4 themes, `fx` full).
3. Take the median per theme of the swipes made while the player was in `play` state.

Noise: the Wi-Fi only gave 25-100 kbps of the stream's 128 on 2026-11-27, so every run had 1-3 underruns. While the
player refills it does not decode, which leaves more CPU for the UI: a single run can be off by ~3 FPS (Swiss moved
26 ↔ 30 with no change that touches it). Compare changes in the same session and repeat a run when it matters.

## Results (2026-11-27, 0.2.5 + this change, median FPS, MP3 playing)

| Theme | before | after | what changed for it |
|---|---|---|---|
| Swiss | 26-30 | 26 | (shorter animation only) |
| Cyberpunk | 15-17 | 26 | scanlines drawn as 1 px fills, only inside the chunk being drawn |
| Zen | 23-25 (clock tile 19) | 26 (clock tile 24) | ensō made at its screen size: no image scaling |
| Brutalism | 19-24 | 23-24 (eco 25) | grid drawn like the scanlines |

What made the difference:
- **Scanlines / grid** (`tile_bg_draw`): LVGL renders in 24-row chunks and called the draw event for every chunk of
  both visible tiles; the old code queued a `lv_draw_line` for all ~80 scanlines of the tile each time (~1600 draw
  tasks per frame). Now 1 px `lv_draw_fill`s, only the rows and columns inside the clip area. Same pixels.
- **Zen ensō**: `lv_image_set_scale(320)` sent the 150 px A8 image through the transform path on every frame. The
  generator now writes it at 188 px (+13 KB flash).
- **Animation 250 ms instead of 400 ms** (`SCROLL_ANIM_TIME_MAX` for the LVGL component, `firmware/CMakeLists.txt`):
  the swipe ends sooner; it does not change the per-frame cost, so it does not show in the FPS numbers.

Tried and dropped: Brutal's hard shadows drawn as two solid strips instead of LVGL's box shadow. Brutal full vs eco
was 22.7-23.8 vs 24.4-25.0 both with and without it, so the shadows are not the cost. The rest of Brutal's gap to
eco is the grid (≈ 40 small fills per chunk); the cat and the HUD corners did not show up in the per-tile numbers.

Not changed: scanlines and the cat are still drawn during the animation (hiding them would pop on and off), draw
buffers stay 296 × 24 px × 2 in internal RAM (no RAM margin for larger ones).
