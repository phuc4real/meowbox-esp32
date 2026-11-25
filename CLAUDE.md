# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

MeowBox: our own ESP-IDF firmware for the OSTB xiaozhi-3st speaker (ESP32-S3), plus a Windows PC agent.
Overview and layout: @README.md. Build, flash, asset generators, partitions: @firmware/README.md.

## Environment

- ESP-IDF 6.1 is **not** on PATH in a fresh shell. Activate it in the same PowerShell command that uses it. The
  default `python` is 3.15 with no packages and IDF's venv is for 3.14, so point both at 3.14 first:
  `$env:IDF_TOOLS_PATH='D:\esp\.espressif'; $env:IDF_PYTHON_ENV_PATH='D:\esp\.espressif\python_env\idf6.1_py3.14_env'; $env:PATH="C:\Users\phuc\AppData\Local\Python\pythoncore-3.14-64;$env:PATH"; . D:\esp\esp-idf\export.ps1 *> $null; cd firmware; idf.py build`
- Run repo scripts with `py -3.14` (it has Pillow, pyserial, ruff and the agent's packages).
- The speaker is on **COM10** (native USB-Serial/JTAG, no buttons needed). COM13 is an unrelated ESP8266.
- You may build and flash / OTA the speaker without asking (`/flash-speaker`). `backup/` holds the vendor flash
  dump; never overwrite or delete it. Drive the running speaker (screenshots, radio, notifications, memory, frozen-UI
  check) with `/run-meowbox`; before a release, bring the docs up to date with `/release-docs`.
- `firmware/sdkconfig` is generated and gitignored; put config changes in `firmware/sdkconfig.defaults`
  (an existing `sdkconfig` keeps old values for keys already in it; delete it or run `idf.py reconfigure`).

## Generated files: edit the source, then run the generator

| Don't hand-edit | Edit instead | Run (from `firmware/`) |
|---|---|---|
| `components/ui/themes_gen.c`, `web/src/themes.css`, `web/themes.json` | `themes/*.json` | `python tools/gen_themes.py` |
| `components/fonts/*.c`, `web/assets/fonts/*` | `tools/fonts/` | `python tools/fonts/gen_theme_fonts.py` |
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

- Internal RAM: lowest free 39.5 KB under the heaviest soak (0.2.2: five stations, two web sessions, screenshots,
  notifications, theme rebuilds; 40.8 KB idle; `docs/perf/results-0.2.2.md`); keep it ≥ 40 KB, there is no margin.
  Put big buffers and task stacks in PSRAM (never for a task that writes flash). After changes that allocate, run
  `tools/memwatch.py` and look at the lowest `int_free`; `heap_min` in `/api/status` always shows ~7.8 KB from a short
  dip during boot.
- When a hardware fact changes, update `hw/ostb_xiaozhi_3st.h` and `docs/pinmap.md` together.
- Lunar calendar changes: verify with `components/lunar/tests/` (host build of `host_dump.c` vs `lunar_ref.py compare`).

## Lint

- Python: `py -3.14 -m ruff check .` (config `ruff.toml`). Don't add new findings; the existing B904s are known.
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
- `pc-agent/` is Windows-only (Media Session, pycaw, SAPI); Python ≥ 3.11.
