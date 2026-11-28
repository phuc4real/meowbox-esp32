# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

MeowBox: our own ESP-IDF firmware for the OSTB xiaozhi-3st speaker (ESP32-S3), plus a Windows PC agent.
Overview and layout: @README.md. Build, flash, asset generators, partitions: @firmware/README.md.

## Environment

- ESP-IDF must be the **`release/v6.1` branch** at or after commit `c43d665a` (what 0.2.3 was built with), **not** the
  `v6.1.0` / `v6.1.1` tags: identical code built with either tag leaves ~12 KB internal RAM free when idle instead of
  ~48 KB (measured 2026-11-26). Check the `ESP-IDF:` line of `python -m esptool --chip esp32s3 image-info
  build\meowbox.bin` (a commit hash on the branch, `v6.1.1` for the tag).
- That IDF is in `C:\esp\release-v6.1\esp-idf` (a shallow clone at `c43d665a` that shares EIM's tools in
  `C:\Espressif\tools`, with its own venv in `C:\Espressif\tools\python\release-v6.1`). It is **not** on PATH in a fresh
  shell; activate it in the same PowerShell command that uses it:
  `. C:\Espressif\tools\Microsoft.release-v6.1.PowerShell_profile.ps1 *> $null; cd firmware; idf.py build`
  EIM's `v6.1` / `v6.1.1` installs next to it are the tags: don't build with them.
- Without that IDF (another machine): `firmware\docker\idf.ps1` (or `idf.sh`) runs `idf.py` in Kiri or Docker with
  the same pinned IDF (`firmware/docker/Dockerfile`), output in `firmware/build-docker/`. Flash from the host with
  esptool (`firmware/README.md`, "Build in Docker"): containers can't reach COM10.
- The `build/` folder remembers the project path and the IDF it was configured with. After the repo moves or IDF
  changes, run `idf.py fullclean` first (it keeps `sdkconfig`).
- Run repo scripts with `py -3.14` (it has Pillow, pyserial, ruff and aiohttp for `pc-agent/mock_speaker.py`).
- The speaker is on **COM10** (native USB-Serial/JTAG, no buttons needed). COM13 is an unrelated ESP8266.
- You may build and flash / OTA the speaker without asking (`/flash-speaker`). `backup/` holds the vendor flash
  dump; never overwrite or delete it. Drive the running speaker (screenshots, radio, notifications, memory, frozen-UI
  check) with `/run-meowbox`; before a release, bring the docs up to date with `/release-docs`.
- After an update over the air the speaker boots from `ota_1`; `idf.py app-flash` writes `ota_0` and changes nothing
  (check `build` in `/api/status`). Use `idf.py flash`, which also resets `otadata` to boot `ota_0`.
- `firmware/sdkconfig` is generated and gitignored; put config changes in `firmware/sdkconfig.defaults`
  (an existing `sdkconfig` keeps old values for keys already in it; delete it or run `idf.py reconfigure`).

## Generated files: edit the source, then run the generator

| Don't hand-edit | Edit instead | Run (from `firmware/`) |
|---|---|---|
| `components/ui/themes_gen.c`, `web/src/themes.css`, `web/themes.json`, `pc-agent/src/ui/themes_gen.rs` | `themes/*.json` | `python tools/gen_themes.py` |
| `components/fonts/*.c`, `web/assets/fonts/*`, `pc-agent/assets/fonts/*` | `tools/fonts/` | `python tools/fonts/gen_theme_fonts.py` |
| `pc-agent/assets/meowbox.ico`, `pc-agent/packaging/wizard-*.bmp` | `pc-agent/assets/make_art.py` (tray icons: the hand-drawn `pc-agent/assets/icons/*.png` are the sources) | `py -3.14 ../pc-agent/assets/make_art.py` |
| `components/cat_sprites/*` | `tools/cat_sprites/sprites.py` | `python tools/cat_sprites/sprites.py` |
| `components/ui/ui_assets.c` | `tools/ui_assets/` | `python tools/ui_assets/gen_ui_assets.py` |
| `components/webui/web.pack` | `web/src/`, `web/assets/` | `python web/build.py` (fails if the gzipped first page load exceeds 30 KB); the web UI is embedded in the app |

Commit the regenerated outputs together with their source.

## Web UI and REST API

- Web UI is plain HTML/CSS/JS, no framework, no CDN (SoftAP mode has no internet). No `alert()`/`confirm()`,
  only in-page modals. All strings live in `web/src/lang/vi.json` and `en.json` (same keys; VI default), read with
  `t('key', {var})`.
- After touching `firmware/web/src/`: `node --check src/app.js` and `python test_api.py` (must print `ALL PASSED`),
  both from `firmware/web/`. Preview without a device: `python mock_server.py --no-auth` → http://localhost:8080.
- `firmware/web/API.md` is the contract. When an endpoint changes, update together: the firmware handler,
  `API.md`, `web/mock_server.py`, `web/test_api.py`, and `pc-agent/mock_speaker.py` if the agent uses it.

## Firmware gotchas

- Internal RAM: under the heaviest soak (0.2.4: five stations, screenshots, notifications through the mixer, theme
  rebuilds; `docs/perf/results-0.2.4.md`) the lowest free is ~39.9 KB with one 36.3 KB dip, median 40.3 KB; ~48 KB
  idle (~53 KB idle in 0.2.5 with the weather task's stack in PSRAM; the soak was not re-run). Keep it ≥ 40 KB, there is no margin.
  Put big buffers and task stacks in PSRAM (never for a task that writes flash). After changes that allocate, run
  `tools/memwatch.py` and look at the lowest `int_free`; `heap_min` in `/api/status` always shows ~7.8 KB from a short
  dip during boot.
- When a hardware fact changes, update `hw/ostb_xiaozhi_3st.h` and `docs/pinmap.md` together.
- Lunar calendar changes: verify with `components/lunar/tests/` (host build of `host_dump.c` vs `lunar_ref.py compare`).

## Lint

- Python: `py -3.14 -m ruff check .` (config `ruff.toml`). Don't add new findings; the existing B904s are known.
- Rust (`pc-agent/`): `cargo clippy --all-targets -- -D warnings` must be clean; `cargo fmt` formats the crate.
- C: format only the lines you changed (`git clang-format`, config `.clang-format`). Never reformat whole files:
  the code packs short statements on one line on purpose. Generated files are in `.clang-format-ignore`.
  On this machine `git clang-format` is not a git command; call the script directly (Git Bash):
  `"$APPDATA/Python/Python314/Scripts/git-clang-format.exe" --binary "$APPDATA/Python/Python314/Scripts/clang-format.exe" --extensions c,h`
  (add `--diff` to preview, `main` to check a whole branch). It refuses files with both staged and unstaged changes.

## Repo conventions

- The repo is public (MIT). Never commit device identifiers: MAC, board UUID, Wi-Fi names/passwords, notify tokens,
  real IPs. `backup/` and `docs/reference-images/` are gitignored on purpose.
- Docs and code comments in English. The only Vietnamese doc is `README_vi.md`, a user guide (how to use the speaker).
  Some existing docs are still in Vietnamese; write new content in English.
- Commit messages: one plain English sentence in the imperative, describing the user-visible change.
- `pc-agent/` is Rust (stable, MSVC), Windows-only (Media Session, Core Audio, WinRT TTS). After touching it, from
  `pc-agent/`: `cargo test`, `cargo clippy --all-targets -- -D warnings` and `cargo fmt` (`rustfmt.toml`, 120
  columns). `mock_speaker.py` stays Python (`py -3.14`).
- A test or dev run of the agent must not use this PC's profile: the installed agent owns ports 8765/8767, and the
  core refreshes the Claude Code hooks in the real `~/.claude/settings.json` and downloads yt-dlp into the real
  `%LOCALAPPDATA%`. Integration tests start the core with `Options { relay, ytdlp_update, update_check, coding: false
  }`; manual runs set `APPDATA`, `LOCALAPPDATA` and `USERPROFILE` to a scratch folder (Git Bash `VAR=… cmd`; in
  PowerShell `$env:VAR = …`, not `set`).
