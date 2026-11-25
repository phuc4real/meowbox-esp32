---
name: flash-speaker
description: Build the MeowBox firmware and put it on the speaker (USB on COM10 or OTA over Wi-Fi), then check it came back up. Use after firmware or web UI changes that should be tried on the device.
---

Build, flash and verify MeowBox on the speaker. `$ARGUMENTS` may say `ota`, `usb`, `app` or `full`;
otherwise pick from what changed.

## 1. Regenerate what changed

From `firmware/`, run only the generators whose sources changed (see the table in CLAUDE.md):
themes → `python tools/gen_themes.py`; web UI → `node --check web/src/app.js`, `python web/test_api.py`,
then `python web/build.py`.

## 2. Build

One PowerShell command (IDF isn't on PATH otherwise):

```powershell
$env:IDF_TOOLS_PATH='D:\esp\.espressif'; . D:\esp\esp-idf\export.ps1 *> $null; cd firmware; idf.py build
```

Stop and fix any build error before flashing. Note the app size: it must fit `ota_0`/`ota_1` (5.25 MB).

## 3. Pick the flash path

| Changed | Command (same activation prefix, in `firmware/`) |
|---|---|
| C code or web UI | `idf.py -p COM10 -b 921600 app-flash` |
| partitions, bootloader, or sdkconfig | `idf.py -p COM10 -b 921600 flash` |
| no USB / user asked for OTA (app only) | login + `POST /api/ota`, below |

None of these touch the `data` partition, so settings, Wi-Fi and password survive. Never write over `backup/`
and never run `erase-flash` unless the user asks.

If COM10 is busy (a monitor or another tool holds it) say so instead of retrying in a loop.

OTA:

```bash
curl -s -c "$TMP/mb.txt" -H "Content-Type: application/json" -d '{"user":"meowbox","pass":"m3owb0x2026"}' http://meowbox-xxxx.local/api/login
curl -s -b "$TMP/mb.txt" -H "Content-Type: application/octet-stream" --data-binary @build/meowbox.bin http://meowbox-xxxx.local/api/ota
```

If login returns 401 the user changed the password: ask for it, don't guess. Keep the cookie file out of the repo.

## 4. Verify

If the speaker last updated over the air it boots from `ota_1` and `app-flash` (which writes `ota_0`) has no effect:
check `elf` changed, otherwise use OTA.

Wait for the reboot (~10 s), then log in and `GET http://meowbox-xxxx.local/api/status` (xxxx = the 4 hex digits on the System tile, or use the IP shown there) (session cookie). Check:
`build` matches this build's time, `mode` is `sta`, `heap_min` hasn't dropped meaningfully (internal RAM is tight;
it dips to ~7.8 KB during boot by design (`docs/perf/results-0.2.0.md`); lower than that is a regression). A failed OTA start rolls back to the previous slot, so an
unchanged `build` means the new image didn't boot: read the serial log with
`idf.py -p COM10 monitor` (stop it after capturing the boot, it holds the port).

Report what was flashed, how, and the status values you checked. Don't claim it works on the device for
anything you didn't actually observe (screen content, sound): say what remains for the user to look at.
