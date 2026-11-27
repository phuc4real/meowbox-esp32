# MeowBox firmware

Our own firmware for the OSTB xiaozhi-3st speaker (ESP32-S3): clock + lunar calendar + weather + alarms,
internet radio (MP3/AAC/HLS, YouTube via the PC agent), notification speaker, PC companion screen, a
cat that codes along with Claude Code (Meow Coding), battery runtime logging and four UI themes. Everything is configured from a web page on
the speaker. Plan and status: [../docs/firmware_v0.2.0.md](../docs/firmware_v0.2.0.md) (0.1.0: [../docs/plan.md](../docs/plan.md)). Hardware: [../docs/pinmap.md](../docs/pinmap.md).

## Requirements

- ESP-IDF **6.1** from the `release/v6.1` branch (0.2.3 was built at commit `c43d665a`). The `v6.1.0` and `v6.1.1`
  tags build and run, but leave far less internal RAM (~12 KB free when idle instead of ~48 KB), which is not enough
  for streaming. Component versions are pinned in `main/idf_component.yml` and `dependencies.lock`; `idf.py build`
  downloads them into `managed_components/`.
- Python 3 with Pillow for the asset generators (`tools/`), Node.js only to regenerate fonts.
- Or, instead of installing ESP-IDF: Docker or Kiri (see [Build in Docker](#build-in-docker)).

## Build & flash

```powershell
# in an ESP-IDF 6.1 shell (or: . $env:IDF_PATH\export.ps1 / source $IDF_PATH/export.sh)
cd firmware
idf.py build
idf.py -p COM10 -b 921600 flash          # first time: bootloader + partition table + app (web UI inside)
# later, app only:   idf.py -p COM10 -b 921600 app-flash
```

After that, update without a cable: on the web page, **System → Update from a .bin file**, or:

```bash
curl -c c.txt -H "Content-Type: application/json" -d '{"user":"meowbox","pass":"m3owb0x2026"}' http://meowbox-xxxx.local/api/login
curl -b c.txt -H "Content-Type: application/octet-stream" --data-binary @build/meowbox.bin http://meowbox-xxxx.local/api/ota
```

Web UI changes: `python web/build.py` (writes `components/webui/web.pack`, which is embedded in the app; fails if
the gzipped first page load is over 30 KB), then rebuild and flash the **app**. User settings live in the separate
`data` partition and survive every kind of flash above.

If the speaker last updated itself over the air it boots from `ota_1`, and `app-flash` writes `ota_0`: use the OTA
upload above (or a full `flash`) so the new image is the one that boots.

### Build in Docker

On a machine without ESP-IDF, `docker/` builds with the same IDF as the releases: the `espressif/idf:release-v6.1`
image, checked out at the commit pinned in `docker/Dockerfile` (amd64 and arm64). The scripts use
[Kiri](https://github.com/libracoder/kiri) when it is installed, otherwise `docker`; set `MEOWBOX_DOCKER` to pick
another Docker-compatible CLI. The first run downloads about 4 GB and builds the image `meowbox-idf:<hash of the
Dockerfile>`; later runs reuse it, and a changed Dockerfile builds a new one (remove old tags with `image rm`).

```powershell
docker\idf.ps1               # Windows PowerShell; any idf.py arguments work: docker\idf.ps1 fullclean
./docker/idf.sh              # Linux, macOS, Git Bash
```

The output goes to `build-docker/` (with its own `sdkconfig`), so it never mixes with a native `build/`. Docker can't
reach the serial port on Windows or macOS, so flash from the host with esptool (`pip install esptool`), or upload
`build-docker/meowbox.bin` over the air as above:

```powershell
cd build-docker
python -m esptool --chip esp32s3 -p COM10 -b 921600 write_flash "@flash_args"   # bootloader + partitions + app, boots ota_0
```

For a release, pass `--bin build-docker/meowbox.bin` to `tools/release.py`.

## Releases and updates from GitHub

The speaker reads `releases/latest/download/manifest.json` of this repo (`CONFIG_MEOWBOX_UPDATE_MANIFEST_URL`) when
you press **System → Check now** on the web page (a daily check is optional, off by default). Installing downloads the
`.bin` with `esp_https_ota`, checks its size and SHA-256 against the manifest, then reboots. Rollback is enabled: an
image that does not mark itself valid falls back to the previous one, and the web page says so.

To publish one (version from `PROJECT_VER` in `CMakeLists.txt`, shared with the PC agent):

```powershell
python web/build.py; idf.py build
python tools/release.py --notes notes.json --body body.md --agent ../pc-agent/packaging/build/MeowBoxAgent-Setup-<ver>.exe --publish
```

`notes.json` holds the short lines the web page shows (`{"vi":[...],"en":[...],"min_ver":"0.1.0"}`). Without
`--publish` it only writes `dist/release/<ver>/`; `--base-url http://<pc>:8000/` makes a manifest for a local test
server. API: [web/API.md](web/API.md) "Update".

## Using it

| Where | What |
|---|---|
| Screen | swipe left/right: **Clock → Radio → PC → Cat → System** (PC is skipped while no agent is connected). Turns off after 5 idle minutes (web: Appearance → "Screen off after", 0 = never), but not while on USB power; a button, a touch, an alarm or a notification wakes it, and that press does nothing else |
| Middle button | power only: holding it switches the speaker off (hardware). The firmware can't see it |
| Volume buttons | click ±5, hold to repeat. During an alarm: click = snooze, hold ~1 s = stop |
| Cat corner on the clock | tap to open the Cat tile |
| System tile | long-press the title to show the safe-area calibration frames |
| Web | `http://meowbox-xxxx.local` (or the IP on the System tile), `meowbox` / `m3owb0x2026`; the page asks for a new password first |
| First setup / new Wi-Fi | with no known network the speaker opens the hotspot `MeowBox-XXXX` (WPA2, a password made per speaker and shown on its screen) by itself and shows a Wi-Fi QR code: a phone camera joins by scanning it, then the card switches to a QR code for `http://192.168.4.1` (tap to hide the card); otherwise System tile → "Wi-Fi", hold VOL+ and VOL− together for 3 s, the web Wi-Fi page or `meowbox wifi-setup` (PC agent). Join it and the setup page opens. With saved networks it opens by itself only after 5 min without Wi-Fi. It closes 10 s after the speaker gets online, or after 10 min with nobody on it |

Notifications from anything on the LAN (token on the web page → Notifications):

```bash
curl "http://meowbox-xxxx.local/api/notify?token=TOKEN&title=Build&text=Xong%20roi"
curl -H "Authorization: Bearer TOKEN" -H "Content-Type: application/json" \
     -d '{"title":"Server","text":"CPU 95%","sound":"alert","priority":"high"}' http://meowbox-xxxx.local/api/notify
```

Battery runtime: unplug and use it normally. MeowBox logs a sample every minute while on battery, shows
the estimated time left (clock + System tile + web Overview) and keeps the last 10 battery sessions
(duration, % used, projected full-charge runtime). Raw data: `/api/battery/log.csv`.

Themes: **Swiss** (default), Cyberpunk, Zen, Brutalism, each with its own night variant. Pick one on the
web page under Theme; the speaker rebuilds its UI in ~0.2 s and the web page follows.
Design notes: [../docs/ui_redesign.md](../docs/ui_redesign.md).

## Regenerating assets

| What | Source | Command | Output |
|---|---|---|---|
| Theme tables | `themes/*.json` | `python tools/gen_themes.py` | `components/ui/themes_gen.c`, `web/src/themes.css`, `web/themes.json` |
| Fonts (LVGL + web) | `tools/fonts/src/` (OFL fonts) | `python tools/fonts/gen_theme_fonts.py` (base font: `tools/fonts/gen_fonts.ps1`) | `components/fonts/*.c`, `web/assets/fonts/*.woff2` |
| Cat sprites | `tools/cat_sprites/sprites.py` (text art) | `python tools/cat_sprites/sprites.py` | `components/cat_sprites/`, previews |
| Ensō image | `tools/ui_assets/enso.png` | `python tools/ui_assets/gen_ui_assets.py` | `components/ui/ui_assets.c` |
| Web UI | `web/src/`, `web/assets/` | `python web/build.py` | `components/webui/web.pack` |
| Theme previews | the speaker itself | screenshot `GET /api/screenshot?tile=0` per theme, save as PNG | `web/assets/themes/<id>.png` |

Web UI development without the speaker: `python web/mock_server.py --no-auth` and open http://localhost:8080.

PC screen, media keys, YouTube relay and spoken notifications: see [../pc-agent/README.md](../pc-agent/README.md).

## Layout

| Path | |
|---|---|
| `main/main.c` | start-up order, buttons, `/api/status` hook |
| `components/board` | pins (from `../hw/ostb_xiaozhi_3st.h`), LCD + LVGL, touch, buttons, backlight, battery |
| `components/audio` | ES8311/ES7210 duplex I2S (`audio_hw`), synthesised UI sounds (`audio_sfx`) |
| `components/player` | HTTP/HLS streaming player (esp_audio_codec), resampling to 44.1 kHz mono |
| `components/net` | Wi-Fi manager + captive portal, mDNS `meowbox-xxxx.local`, UDP discovery, SNTP, HTTP server, auth, OTA |
| `components/webui` | the web UI pack (`web.pack`, generated) embedded in the app, file lookup with ETags |
| `components/settings` | JSON settings per group in `/data/cfg/*.json` (LittleFS) |
| `components/ui` | tileview shell, theme engine (`theme.h`, generated `themes_gen.c`), themed widgets, overlay cards, backlight / night mode |
| `themes/` | one JSON per UI theme (colours, fonts, shapes, effects, cat palette, web fonts) |
| `components/apps` | clock, radio, notify, pc, cat, battery, system, update (tiles + their web endpoints) |
| `components/diag` | error log kept across restarts in RTC memory, reset reason, last 10 starts, frozen-UI check (`GET /api/log`) |
| `components/lunar` | Vietnamese lunar calendar (tested against 1900–2199) |
| `components/fonts` | LVGL fonts with Vietnamese glyphs: base Be Vietnam Pro + 4 per theme (generated) |
| `components/cat_sprites` | pixel-art cat (generated, see `tools/cat_sprites`) |
| `web/` | web UI source, REST contract [`API.md`](web/API.md), mock server, tests |
| `tools/` | asset generators (themes, fonts, sprites, ensō), `memwatch.py` (memory/CPU log), `release.py` |

## Partitions

| Name | Offset | Size | Use |
|---|---|---|---|
| nvs | 0x9000 | 16 KB | Wi-Fi driver settings and PHY calibration (about 10 of 263 entries used) |
| otadata | 0xd000 | 8 KB | which app slot boots |
| ota_0 / ota_1 | 0x20000 / 0x560000 | 5.25 MB each | app with the web UI inside (3.00 MB in 0.2.5; OTA, rollback enabled) |
| (free) | 0xaa0000 | 4.4 MB | unallocated, see below |
| data | 0xf00000 | 1 MB | LittleFS: settings, Wi-Fi list, password hash, battery log (about 30 KB used, the battery log grows to ~200 KB) |

Up to 0.2.0 the table also had `phy_init` (unused: calibration is kept in NVS) and `storage`, a 4.4 MB assets
partition that held the web UI before it moved into the app. Both are gone from `partitions.csv`; every other offset
is unchanged. What that means:

- **Updating over the air** (web page or GitHub) never rewrites the partition table. A speaker updated that way keeps
  the old table with an unused `storage` partition: harmless, the firmware no longer mounts it.
- **A full USB flash** (`idf.py flash`) writes the new table. Settings in `data` survive because its offset did not
  move. The old `storage` bytes stay in flash, unused.
- The free 4.4 MB can become a new partition later (sounds, recordings, a core dump) without moving anything else.

On first boot MeowBox imports the Wi-Fi networks that xiaozhi saved in NVS.

Forgot the web password? Erase only the `data` partition over USB. That resets settings, Wi-Fi list, stations,
alarms, cat, password and token to the defaults, and keeps the firmware:

```powershell
python -m esptool --port COM10 erase-region 0xf00000 0x100000
```
