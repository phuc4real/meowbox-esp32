# MeowBox firmware

Our own firmware for the OSTB xiaozhi-3st speaker (ESP32-S3): clock + lunar calendar + weather + alarms,
internet radio (MP3/AAC/HLS, YouTube via the PC agent), notification speaker, PC companion screen, a
virtual cat, battery runtime logging and four UI themes. Everything is configured from a web page on
the speaker. Plan and status: [../docs/plan.md](../docs/plan.md). Hardware: [../docs/pinmap.md](../docs/pinmap.md).

## Requirements

- ESP-IDF **6.1** (`release/v6.1`). Component versions are pinned in `main/idf_component.yml` and
  `dependencies.lock`; `idf.py build` downloads them into `managed_components/`.
- Python 3 with Pillow for the asset generators (`tools/`), Node.js only to regenerate fonts.

## Build & flash

```powershell
# in an ESP-IDF 6.1 shell (or: . $env:IDF_PATH\export.ps1 / source $IDF_PATH/export.sh)
cd firmware
idf.py build
idf.py -p COM10 -b 921600 flash          # first time: bootloader + partitions + app + web assets
# later, app only:   idf.py -p COM10 -b 921600 app-flash
```

After that, update the app without a cable from the web UI (Cài đặt → Hệ thống → firmware upload), or:

```bash
curl -c c.txt -H "Content-Type: application/json" -d '{"user":"meowbox","pass":"m3owb0x2026"}' http://meowbox.local/api/login
curl -b c.txt -H "Content-Type: application/octet-stream" --data-binary @build/meowbox.bin http://meowbox.local/api/ota
```

Web UI changes: `python web/build.py` (writes `storage/web/*.gz`), then `idf.py -p COM10 storage-flash`
(or a full `idf.py flash`). User settings live in the separate `data` partition and survive this.

## Using it

| Where | What |
|---|---|
| Screen | swipe left/right: **Clock → Radio → PC → Cat → System** |
| Middle button (GPIO1) | click: stop alarm / close notification / play-stop radio · double click: back to the clock. Holding it powers off (hardware) |
| Volume buttons | click ±5, hold to repeat. During an alarm: snooze |
| Cat corner on the clock | tap to open the Cat tile |
| System tile | long-press the title to show the safe-area calibration frames |
| Web | `http://meowbox.local` (or the IP on the System tile), `meowbox` / `m3owb0x2026`; change the password |
| First setup / new Wi-Fi | with no known network the speaker opens the hotspot `MeowBox-XXXX` (WPA2, password `meowbox2026`, also shown on screen) by itself; otherwise System tile → "Wi-Fi", hold VOL+ and VOL− together for 3 s, the web Wi-Fi page or `agent.py wifi-setup`. Join it and the setup page opens. It closes 10 s after the speaker gets online, or after 10 min with nobody on it |

Notifications from anything on the LAN (token on the web page → Thông báo):

```bash
curl "http://meowbox.local/api/notify?token=TOKEN&title=Build&text=Xong%20roi"
curl -H "Authorization: Bearer TOKEN" -H "Content-Type: application/json" \
     -d '{"title":"Server","text":"CPU 95%","sound":"alert","priority":"high"}' http://meowbox.local/api/notify
```

Battery runtime: unplug and use it normally. MeowBox logs a sample every minute while on battery, shows
the estimated time left (clock + System tile + web Overview) and keeps the last 10 battery sessions
(duration, % used, projected full-charge runtime). Raw data: `/api/battery/log.csv`.

Themes: **Swiss** (default), Cyberpunk, Zen, Brutalism, each with its own night variant. Pick one on the
web page under Cài đặt → Giao diện; the speaker rebuilds its UI in ~0.2 s and the web page follows.
Design notes: [../docs/ui_redesign.md](../docs/ui_redesign.md).

## Regenerating assets

| What | Source | Command | Output |
|---|---|---|---|
| Theme tables | `themes/*.json` | `python tools/gen_themes.py` | `components/ui/themes_gen.c`, `web/src/themes.css`, `web/themes.json` |
| Fonts (LVGL + web) | `tools/fonts/src/` (OFL fonts) | `python tools/fonts/gen_theme_fonts.py` (base font: `tools/fonts/gen_fonts.ps1`) | `components/fonts/*.c`, `storage/web/fonts/*.woff2` |
| Cat sprites | `tools/cat_sprites/sprites.py` (text art) | `python tools/cat_sprites/sprites.py` | `components/cat_sprites/`, previews |
| Ensō image | `tools/ui_assets/enso.png` | `python tools/ui_assets/gen_ui_assets.py` | `components/ui/ui_assets.c` |
| Web UI | `web/src/` | `python web/build.py` | `storage/web/*.gz` |
| Theme previews | the speaker itself | screenshot `GET /api/screenshot?tile=0` per theme, save as PNG | `storage/web/themes/<id>.png` |

Web UI development without the speaker: `python web/mock_server.py --no-auth` and open http://localhost:8080.

PC screen, media keys, YouTube relay and spoken notifications: see [../pc-agent/README.md](../pc-agent/README.md).

## Layout

| Path | |
|---|---|
| `main/main.c` | start-up order, buttons, `/api/status` hook |
| `components/board` | pins (from `../hw/ostb_xiaozhi_3st.h`), LCD + LVGL, touch, buttons, backlight, battery |
| `components/audio` | ES8311/ES7210 duplex I2S (`audio_hw`), synthesised UI sounds (`audio_sfx`) |
| `components/player` | HTTP/HLS streaming player (esp_audio_codec), resampling to 44.1 kHz mono |
| `components/net` | Wi-Fi manager + captive portal, mDNS `meowbox.local`, SNTP, HTTP server, auth, OTA |
| `components/settings` | JSON settings per group in `/data/cfg/*.json` (LittleFS) |
| `components/ui` | tileview shell, theme engine (`theme.h`, generated `themes_gen.c`), themed widgets, overlay cards, backlight / night mode |
| `themes/` | one JSON per UI theme (colours, fonts, shapes, effects, cat palette, web fonts) |
| `components/apps` | clock, radio, notify, pc, cat, battery, system (tiles + their web endpoints) |
| `components/lunar` | Vietnamese lunar calendar (tested against 1900–2199) |
| `components/fonts` | LVGL fonts with Vietnamese glyphs: base Be Vietnam Pro + 4 per theme (generated) |
| `components/cat_sprites` | pixel-art cat (generated, see `tools/cat_sprites`) |
| `web/` | web UI source, REST contract [`API.md`](web/API.md), mock server, tests |
| `storage/` | assets image flashed to the `storage` partition (web bundle, web fonts, theme previews) |
| `tools/` | asset generators (themes, fonts, sprites, ensō) |

## Partitions

| Name | Offset | Size | Use |
|---|---|---|---|
| nvs | 0x9000 | 16 KB | Wi-Fi driver / PHY |
| otadata | 0xd000 | 8 KB | |
| ota_0 / ota_1 | 0x20000 / 0x560000 | 5.25 MB each | app (OTA, rollback enabled) |
| storage | 0xaa0000 | 4.4 MB | read-only assets (web UI, fonts, previews) |
| data | 0xf00000 | 1 MB | settings, Wi-Fi list, password hash, cat save, battery log |

On first boot MeowBox imports the Wi-Fi networks that xiaozhi saved in NVS.
