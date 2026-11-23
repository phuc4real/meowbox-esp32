# Legacy: xiaozhi-esp32 port for the OSTB board

> **Not in use any more.** The speaker now runs our own firmware, [MeowBox](../../firmware/README.md).
> This folder keeps the board port for upstream [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)
> (AI voice assistant) in case someone wants that firmware on this hardware. It was built and booted
> on the device on 2026-11-22 against upstream commit `92d400d` (v2.6.0).

| File | |
|---|---|
| `xiaozhi-esp32-ostb-xiaozhi-3st.patch` | board `ostb/ostb-xiaozhi-3st` + partition table, apply on a clean clone |
| `first_boot.txt` | first boot log of that build |

Note: the xiaozhi board code still treats GPIO0 as the main button. On this case the reachable middle
button is **GPIO1, active high** (see [../../docs/pinmap.md](../../docs/pinmap.md)). To use it, set
`BOOT_BUTTON_GPIO` to `GPIO_NUM_1` in `config.h` and create that `Button` as active-high.

## Toolchain

| Thing | Where / version |
|---|---|
| ESP-IDF | branch `release/v6.1` (v6.1.1, commit `c43d665a`) |
| Tools | xtensa gcc 15.2 (installed by ESP-IDF) |
| Project | `xiaozhi-esp32/` at the repo root (clone of github.com/78/xiaozhi-esp32, not in git) |
| Our changes | [`xiaozhi-esp32-ostb-xiaozhi-3st.patch`](xiaozhi-esp32-ostb-xiaozhi-3st.patch) |
| Python | 3.14, esptool 5.6.0 |

xiaozhi-esp32 now **requires ESP-IDF ≥ 6.0.1** and recommends the `release/v6.1` branch over the `v6.1` tag.

### Load the environment (PowerShell)

```powershell
. $env:IDF_PATH\export.ps1      # ESP-IDF >= 6.0.1
```

## Build

```powershell
cd xiaozhi-esp32
python scripts/build.py ostb/ostb-xiaozhi-3st --language vi-VN
```

- The board key is `<manufacturer>/<type>` (`ostb/ostb-xiaozhi-3st`), not the bare type.
- Pass the language with `--language`. A `CONFIG_LANGUAGE_*` in `config.json` gets overridden
  (build.py adds `en-US` by default).
- Outputs in `build/`: `bootloader/bootloader.bin`, `partition_table/partition-table.bin`,
  `xiaozhi.bin` (app, about 2.6 MB), `generated_assets.bin` (about 1.6 MB), `merged-binary.bin`.

### Re-apply our board onto a fresh clone

```bash
git clone https://github.com/78/xiaozhi-esp32.git && cd xiaozhi-esp32
git checkout 92d400d          # the commit the patch was made against
git apply ../legacy/xiaozhi/xiaozhi-esp32-ostb-xiaozhi-3st.patch
```

If upstream has changed `main/CMakeLists.txt` / `Kconfig.projbuild` too much, add the two
small hunks by hand. They sit next to the `XINGZHI_CUBE_0_85TFT_WIFI` entries.

## Effective sdkconfig (the parts that matter)

| Option | Value | Why |
|---|---|---|
| `CONFIG_BOARD_TYPE_OSTB_XIAOZHI_3ST` | y | selects our board dir |
| `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME` | `partitions/v2/16m_ostb.csv` | keep the vendor layout |
| `CONFIG_ESPTOOLPY_FLASHSIZE_16MB` / `FLASHMODE_QIO` / `FLASHFREQ_80M` | y | matches the chip |
| `CONFIG_SPIRAM_MODE_OCT` / `SPIRAM_SPEED_80M` | y | 8 MB octal PSRAM |
| `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ` | 240 | |
| `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED` | y | log on the USB COM port |
| `CONFIG_LANGUAGE_VI_VN` | y | UI strings + voice prompts |
| `CONFIG_USE_AUDIO_PROCESSOR` | y | AFE: AEC/NS on the ES7210 input |
| `CONFIG_USE_AFE_WAKE_WORD` + `CONFIG_SR_WN_WN9_HIESP` | y | wake word "Hi ESP" |
| `CONFIG_OTA_URL` | `https://api.tenclass.net/xiaozhi/ota/` | change it to point at a self-hosted server |
| `CONFIG_LV_COLOR_DEPTH` | 16 | RGB565 panel |
| Fonts (CMake) | `font_noto_sans_basic_20_4` builtin + `font_noto_sans_common_20_4` in assets | Vietnamese glyphs come from the "common" font in assets |

## Partition table (`16m_ostb.csv`, same as vendor)

| Name | Type | Offset | Size |
|---|---|---|---|
| nvs | data/nvs | `0x9000` | 16 KB |
| otadata | data/ota | `0xd000` | 8 KB |
| phy_init | data/phy | `0xf000` | 4 KB |
| ota_0 | app | `0x20000` | 5376 KB (`0x540000`) |
| ota_1 | app | `0x560000` | 5376 KB |
| assets | data/spiffs (0x82) | `0xaa0000` | 5504 KB (`0x560000`) |

Upstream's default `16m.csv` differs (2 × 3.9 MB app, assets at `0x800000`). We kept the vendor
layout so the vendor app could stay in `ota_0`.

## Flashing

Port: **COM10** (USB-Serial/JTAG). No buttons needed: esptool resets into the ROM loader over USB.
(COM13 on this PC is an unrelated ESP8266 board.)

### What was done (side-by-side, keeps vendor app)

```powershell
cd xiaozhi-esp32\build
python -m esptool --chip esp32s3 --port COM10 -b 921600 write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m `
  0x0 bootloader\bootloader.bin 0x8000 partition_table\partition-table.bin `
  0x560000 xiaozhi.bin 0xaa0000 generated_assets.bin
python -m esptool --chip esp32s3 --port COM10 erase-region 0x9000 0x4000                      # wipe NVS
python $env:IDF_PATH\components\app_update\otatool.py --port COM10 switch_ota_partition --slot 1
```

### Normal re-flash after code changes (app only)

```powershell
python -m esptool --chip esp32s3 --port COM10 -b 921600 write-flash 0x560000 build\xiaozhi.bin
```

Note: once xiaozhi does an OTA update over Wi-Fi, it writes the *other* slot (`ota_0`). The vendor
app is gone after that. It is still in the backup.

### Serial monitor

```powershell
idf.py -p COM10 monitor          # inside xiaozhi-esp32, env loaded
```

## Backup & full restore

| File | What |
|---|---|
| `backup/speaker_full_16MB.bin` | full 16 MB dump of the vendor state, SHA256 `4DCA7EB182560E75A5CAC00283568A760AFC5E1CF8970197BCADACCE16651C67`, verified against the chip |
| `%USERPROFILE%\Documents\speaker-backup\speaker_full_16MB.bin` | second copy |
| `backup/ota0_app.bin` | vendor app partition only (input for the RE tools) |
| `logs/vendor_boot.txt` | vendor firmware boot log |
| `legacy/xiaozhi/first_boot.txt` | first boot log of our build |

Restore the original vendor firmware exactly:

```powershell
python -m esptool --port COM10 -b 921600 write-flash 0 backup\speaker_full_16MB.bin
```

## Board code notes (`main/boards/ostb/ostb-xiaozhi-3st/`)

- Based on upstream `nologo/xingzhi-cube-0.85tft-wifi`. The vendor did the same: identical LCD pins.
  Audio is replaced by `BoxAudioCodec` (from the esp32-s3-box board).
- Reuses `nologo/xingzhi-cube-1.54tft-wifi/power_manager.h` (charge pin + ADC2 CH6 battery), as the vendor did.
- Touch: `esp_lcd_touch_cst816s`, polled every 50 ms in a task. Tap = toggle chat.
  The component's `ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG()` macro does not compile in C++
  (designator order), so the config struct is filled field by field.
- PowerSaveTimer(−1, 60 s, no shutdown): dims the backlight after 60 s idle while on battery.
- Buttons: BOOT = talk / Wi-Fi config while starting; 40 = vol+ (long = 100); 39 = vol− (long = mute).

## Vendor firmware facts (for reference)

- `xiaozhi` v2.4.0, IDF v5.5.2-dirty, built Jul 31 2026. App name in the boot log: `OSTB_XIAOZHI_3ST`.
- Extra MCP tools: VOV radio (`audio-lss.vov.vn` HLS), Zing MP3 music, SJC gold price, lottery
  results, news, open-meteo weather, alarms, storytelling.
- Wi-Fi AP `D-Solution-XXXX`. The web config page has a hidden "Device Config" tab for generic
  pin presets (`machtim_*`, `esp32s3mini_*`, `esp32s3zero_*`). This board does not use it.
  Its pins are compiled in.
- NVS at dump time held only `board/uuid`, PHY calibration, and `audio/output_volume = 0`.
