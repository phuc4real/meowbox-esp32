---
name: run-meowbox
description: Run, drive and check the MeowBox speaker and its PC agent - take screenshots of the speaker's screen, switch theme or night mode, play/stop the radio, send notifications and audio, read memory/stack/CPU, detect a frozen UI, capture the serial log, or test against the mock speaker/mock web server. Use after flashing (see /flash-speaker) or to reproduce a bug on the device.
---

# Run and drive MeowBox

MeowBox is ESP32-S3 firmware with a REST API (`firmware/web/API.md`) plus a Windows PC agent. The firmware has no
desktop build: it runs on the speaker (USB on COM10 on this PC), and you drive it over Wi-Fi with
`.claude/skills/run-meowbox/driver.py` (stdlib Python; Pillow optional for PNG). Paths below are relative to the repo
root. To build and flash first, use `/flash-speaker`.

## Prerequisites (this Windows PC)
- `py -3.14` (has Pillow, pyserial, comtypes, psutil). The default `python` is 3.15 with no packages: don't use it.
- The speaker's IP is on its System tile (5th screen). Put it in `MEOWBOX_HOST`; the web password defaults to
  `m3owb0x2026` (`MEOWBOX_PASS` overrides).

## Run (agent path): the driver

```bash
export MEOWBOX_HOST=<speaker ip>  D=.claude/skills/run-meowbox/driver.py
py -3.14 $D wait-up                    # after flash / reboot: waits until login works, prints the version
py -3.14 $D status                     # ver, build, mode, rssi, pc, vol, play {st,id,name,title}
py -3.14 $D shot --tile 1 --mask --out radio.png   # PNG at 2x; --mask tints what the case hides
py -3.14 $D theme zen                  # swiss | cyberpunk | zen | brutal (rebuilds every tile)
py -3.14 $D night on                   # on | off | auto
py -3.14 $D radio play 9               # play [station id] | stop | next | prev | vol N
py -3.14 $D notify "Driver test" --title "run-meowbox"
py -3.14 $D tone --secs 1              # audio notification, same path as `meowbox say`
py -3.14 $D api GET api/config/alarms  # any endpoint; JSON body as 3rd arg: api PUT api/config/display '{"theme":"brutal"}'
py -3.14 $D mem                        # heap + lowest free stack of every task
py -3.14 $D cpu --secs 3               # per-task CPU, one core = 100 %
py -3.14 $D ui-alive                   # "ok: taskLVGL x%" or "FROZEN" (exit 1) - the screen-freeze check
py -3.14 $D serial --secs 20 --grep "starting|pc:|player"   # USB log; REBOOTS the speaker, see Gotchas
```

Station ids come from `py -3.14 $D api GET api/config/radio`. Tiles: 0 clock, 1 radio, 2 PC, 3 cat, 4 system.

Whole-UI checks (all themes x tiles, day or `--night`, hidden area tinted) and memory logging:

```bash
py -3.14 tools/ui_shots.py --host $MEOWBOX_HOST --out shots/            # writes shots/sheet.png + one PNG per tile
py -3.14 firmware/tools/memwatch.py --host $MEOWBOX_HOST --every 5 --minutes 6 --out mem.csv
```

## Run without the speaker
- **Web API / web page:** `py -3.14 firmware/web/mock_server.py --port 8099`, then the driver with
  `MEOWBOX_HOST=localhost:8099` (`wait-up`, `status`, `theme`, `radio`, `notify`, `shot`, `api` work). The web page is
  at http://localhost:8099.
- **PC agent:** from `pc-agent/` in Git Bash (it maps `/tmp` to the Windows temp folder; in PowerShell use a real
  path), a mock speaker plus the agent from source with a throw-away config:
  ```bash
  py -3.14 mock_speaker.py --port 8098 --token test --quiet          # background; prints every [ws] message
  py -3.14 -m meowbox_agent --config /tmp/agent.json config --set-host 127.0.0.1:8098 --token test --relay-port 8797
  py -3.14 -m meowbox_agent --config /tmp/agent.json notify "Agent test" "from run skill"   # -> 200 queued
  (timeout 15 py -3.14 -m meowbox_agent --config /tmp/agent.json run > /tmp/agent.log 2>&1 &); sleep 6
  curl -s "http://127.0.0.1:8098/mock/cmd?cmd=vol_up"   # -> {"sent":"vol_up","agent":"<PC name>"} while run is up
  grep -a "connected\|media_cmd" /tmp/agent.log         # "connected to MeowBox-MOCK", "media_cmd vol_up"
  ```
- Web UI tests: from `firmware/web/`, `node --check src/app.js` and `py -3.14 test_api.py` (prints `ALL PASSED`).

## Gotchas
- **Opening COM10 reboots the speaker** (USB-Serial/JTAG reset on open, even with DTR/RTS low). Every `serial` run
  starts with a boot log and drops all web sessions; run `wait-up` afterwards. Only one process can hold COM10 -
  stop a running `serial` before `idf.py ... flash`.
- **The speaker keeps 4 login sessions** and drops the oldest. The driver caches its cookie in
  `%TEMP%/meowbox-driver-<host>.cookie`; a script that logs in from many threads gets random 401s - share one session.
- **`shot --tile N` switches the visible tile** on the device (it is not an off-screen render), waits 0.7 s, then
  captures. The capture **excludes the top layer**: alarm/notification cards and the radio station list don't show.
  The clock's battery label fills in up to 1 s after a theme/night rebuild, so an immediate shot can show it empty.
- **mock_server.py's screenshot is a fake** blocky clock, and it has no `/api/ping` or `/api/debug` (`mem`, `cpu`,
  `ui-alive` say "speaker-only"). Use the real speaker for anything visual or memory related.
- **Git Bash rewrites `/api/...` arguments** into `C:/Program Files/Git/api/...`. The driver undoes it for `api`, and
  accepts `api/...` without the slash; for other tools use `MSYS_NO_PATHCONV=1` or PowerShell.
- **Vietnamese text through `curl` in Git Bash gets mangled** (a cat name became "B Mu?p ng Yu Qu"). Send JSON with
  the driver (`api PUT ... '{...}'`) or Python, never curl, when it contains non-ASCII.
- **The console is cp1252**: `print()` of Vietnamese raises UnicodeEncodeError in ad-hoc scripts. The driver
  reconfigures stdout to UTF-8; in your own one-liners print `ascii(x)`.
- **A real PC agent already runs on this PC** and owns relay port 8765: give a test agent `--relay-port 8797`, or it
  logs `relay port 8765 busy`.
- Radio start takes 2-4 s (`st` goes `buf` -> `play`); poll `status` instead of sleeping a fixed time. The alarm card
  and screen-off are invisible to `shot`: check them by eye or in the serial log (`clock: alarm HH:MM ringing`,
  `ui: screen sleep`).

## Troubleshooting
- `GET /api/... -> HTTP 503 {"err":"busy"}` from `radio play <youtube id>`: the speaker has no PC relay. Check
  `status` shows `"pc": true`; the agent must be running on the PC.
- `wait-up` prints `not reachable`: wrong IP (it changes with DHCP; read it on the System tile) or the speaker is
  still booting / on its setup hotspot `MeowBox-XXXX`.
- `ui-alive` says `FROZEN` with `decode` near 100 %: the 0.2.1 decoder spin (fixed in 0.2.2). `radio stop` brings
  the screen back.
- `pyserial missing`: use `D:\esp\.espressif\python_env\idf6.1_py3.14_env\Scripts\python.exe` for `serial`.
