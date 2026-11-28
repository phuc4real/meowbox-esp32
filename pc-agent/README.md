# MeowBox PC agent

A small Windows app that does this for the MeowBox speaker:

- Sends the PC's stats (CPU, RAM, GPU, temperatures, network, disk) to the speaker every second over the WebSocket
  `ws://<speaker>/ws/pc`.
- Sends what is playing (Spotify, Chrome, Edge...) and takes ⏮ ⏯ ⏭ 🔉 🔊 back from the speaker.
- Sends notifications from the command line (`notify`), or reads them aloud with Windows text-to-speech (`say`).
- Turns the speaker's setup Wi-Fi `MeowBox-XXXX` back on (`wifi-setup`).
- Is the **YouTube relay**: the speaker asks `http://<pc>:8765/yt/stream?url=...`, the agent gets the AAC audio with
  yt-dlp and streams it back like a radio station (no re-encoding, see YouTube below). Only the connected speaker may
  use the relay; anything else gets 403.
- Shows what Claude Code is doing on the speaker's cat screen (Meow Coding, below).

Since 0.3.0 the agent is written in Rust (it was Python up to 0.2.x; `docs/rewrite_pc_agent.md`). Same settings,
same files, same protocol: installing 0.3 over 0.2 keeps the speaker, token, autostart and Meow Coding.

## Install (recommended)

Download `MeowBoxAgent-Setup-x.y.z.exe` (~6 MB) from
[Releases](https://github.com/phuc4real/meowbox-esp32/releases), run it, click Next. The installer:

- installs into `Program Files\MeowBox Agent` (asks for admin), with a Start Menu shortcut;
- opens the firewall for the agent on **Private** networks (YouTube relay);
- options: run at Windows start (on by default), add `meowbox.exe` to PATH;
- downloads the latest yt-dlp into `%LOCALAPPDATA%\MeowBox\bin\yt-dlp` (not bundled, so it can always be updated);
- removes what the Python agent left (its `_internal` folder, `yt-dlp.pyz`), keeping the settings;
- speaks Vietnamese on a Vietnamese Windows, English otherwise.

After installing, the agent starts, shows a cat in the system tray, and the first time opens its window.

**The window** wears the speaker's theme (Swiss, Cyberpunk, Zen or Brutalism, with the theme's display font; asked
from the speaker's `GET /api/theme`), light or dark as Windows is set. On top, a mirror of the speaker's Computer
screen: the speaker's name, IP and link state, and the numbers it shows (CPU, RAM, GPU, GPU temperature) and what is
playing, exactly as the agent just sent them. When the link is not up, the mirror says why in one sentence with the
one button that fixes it (enter the IP, enter the token, retry, take the speaker over, change the relay port, ...).
Below it, two tabs:

- **Kết nối** (connect): three rows the agent checks by itself, in order, each turning ✓ when it passes. The first
  one that fails opens in place with the reason and the fix, and the cursor goes to it.
  1. **IP loa**: the IP shown on the speaker's System tile (the agent calls `/api/ping`). Saved as soon as it answers.
  2. **Token**: from the speaker's Notifications page. Required: the speaker only accepts an agent with the right
     token (stats, media, YouTube, `notify`, `say`, `wifi-setup`). Saved as soon as the speaker accepts it.
  3. **YouTube relay**: the speaker tries to call the relay on this PC. If that fails on a network Windows calls
     Public, a "Cho phép trên mạng này" button adds a firewall rule for that network type only (asks for admin).

  Under the rows: "Gửi thông báo thử" (test notification) and "Nối lại loa ngay" (reconnect now).
- **Cài đặt** (settings), one list: agent version and update (downloads the new installer with a progress bar,
  checks it against the release's `SHA2-256SUMS` and runs it; a release without that file opens the release page
  instead), yt-dlp, ffmpeg (progress bars too), daily update check, run at Windows start, relay port, language
  (follows Windows: Vietnamese, else English; or pick one); log folder, GitHub, about.

The window remembers its position and size. Every control is reachable with Tab and has a name for Narrator.

**The tray icon** shows the state in the cat's eyes: open = connected, half closed = connecting, × = error, with a
green / grey / red dot as a second cue; the tooltip and the first menu line say what is going on and what to do. A
left click opens the window (or brings it to the front). The right-click menu: open the window, open the speaker's
web page, pause stats, Meow Coding, quit. When the speaker is held by another PC, "Giành quyền nối loa" takes it
over. When GitHub has a newer agent, the menu shows "Có bản x.y.z", which opens Settings. Starting the agent again
(Start menu) opens the running agent's window.

Toasts, at most one of each kind every 10 minutes: the speaker lost for more than 2 minutes after having been
connected, the token refused, the speaker taken over by another PC, the ffmpeg download (started, done, failed), and
each new agent version once. Connecting is silent (the cat opens its eyes). Meow Coding on/off and the test
notification answer with a toast every time.

The agent does not look for the speaker on the network (no mDNS, no UDP broadcast): security software on company
PCs blocks or flags that kind of scan. It only connects to the IP you typed (`host`) or the last one that worked
(`last_host`), and checks the speaker's **name** (`MeowBox-1A2B`) through `/api/ping` so a home with two speakers
does not connect to the wrong one. When the speaker's IP changes, type it again in **Kết nối loa** (or
`meowbox config --set-host IP`); better, give the speaker a fixed IP on the router. A speaker takes one PC at a time.

Settings: `%APPDATA%\MeowBox\agent.json`. Log: `%APPDATA%\MeowBox\agent.log` (rotated at 1 MB × 3).

### YouTube

- Videos: yt-dlp downloads the AAC audio (m4a, itag 140, ~128 kbps), the agent strips the MP4 container into ADTS for
  the speaker. No re-encoding, no ffmpeg.
- Live: yt-dlp will not download live HLS without ffmpeg, so the first time a live stream is played the agent
  downloads an LGPL ffmpeg (FFmpeg-Builds `win64-lgpl-shared`, ~75 MB, SHA-256 checked) into
  `%LOCALAPPDATA%\MeowBox\bin\ffmpeg`. The audio is only copied (AAC in MPEG-TS). The speaker retries by itself
  meanwhile and the tray shows a notification. An ffmpeg already in PATH is used as is.
- yt-dlp is the official onedir build (`yt-dlp_win.zip`, ~24 MB), one folder per version under `bin\yt-dlp\`, so an
  update never touches files a running stream uses. Check/update: `meowbox tools status|update`, or Cài đặt.
- Each stream runs in its own Windows job: when the speaker disconnects (or the agent quits or crashes), yt-dlp and
  ffmpeg end with it.

## Meow Coding (Claude Code)

The cat tile on the speaker shows what Claude Code is doing on this PC: the cat types at a laptop while a session
works, holds up a "?" when one waits for you (a permission prompt or a question: the speaker opens the cat tile,
wakes the screen and chimes), and is happy when a turn is done. Next to it: the session title, the current action
("Đang đọc file", "Đang chạy bash", "Đọc ảnh nè!"), the time in that state, context used, tokens and the 5-hour limit.

- Turn it on with **Meow Coding (Claude Code)** in the tray menu, or `meowbox coding install` (`uninstall`, `status`).
  This adds hooks to `~/.claude/settings.json` (other hooks and settings are kept; the file as it was before is
  saved once as `settings.json.meowbox.bak`). Sessions started after that show up.
- Each hook runs `curl` in the background (`async`) and sends the event to `http://127.0.0.1:8767/hook`
  (`coding_port` in `agent.json`, this PC only) with a per-install key (`%APPDATA%\MeowBox\hook.key`), so a web page
  or another program cannot feed the cat. Claude Code never waits for it, and when the agent is not running the hook
  still ends without an error (`|| exit 0`). Hooks written by an older agent are rewritten when the agent starts.
- The agent reads each session's transcript (`~/.claude/projects/…/*.jsonl`) for the title, model, tokens and
  context.
- The 5-hour and weekly usage of your Claude plan come from Claude Code's status line: `install` sets one
  (`curl … http://127.0.0.1:8767/status`, it shows `=^.^= MeowBox · 5h 23% · 7d 41%`) unless you already have your
  own, which it never replaces. The speaker shows `5h 23%` next to the tokens. API-key sessions have no plan limits.
- Only short labels go to the speaker: title, project folder, branch, and the kind of action (`read`, `bash`,
  `image`...). No prompts, commands, file names or contents.

## Command line

After installing (with "add to PATH"), or `target\release\meowbox.exe` from a build:

```powershell
meowbox notify "Build done" "esp32-playground OK" --sound chime --priority high
meowbox say "Build xong rồi nha"            # Windows TTS → WAV → /api/notify/audio
meowbox say "test" --save test.wav           # only save the file
meowbox wifi-setup [on|off|status]           # setup Wi-Fi MeowBox-XXXX (password on the speaker's screen)
meowbox coding install                       # Meow Coding: Claude Code hooks (uninstall | status)
meowbox tools status                         # yt-dlp and ffmpeg versions (update: latest yt-dlp)
meowbox config --set-host 192.168.1.50      # speaker IP (System tile)
meowbox config --token <token> --speaker MeowBox-1A2B
meowbox run                                  # the agent without a tray icon, --no-relay, --no-update
```

`--config <file>` and `--host IP[:port]` work with every command; `-v` logs more.

`say` picks a Vietnamese voice (Microsoft An) when Windows has one. If not: *Settings → Time & language → Speech →
Add voices*, add Vietnamese.

GPU load and GPU temperature come from Windows itself, for any GPU (the "GPU Engine" counters and the display
driver's report, as in Task Manager; some GPUs report no temperature). Nothing else to install. There is no CPU temperature: Windows has no API
for it without a kernel driver, so the speaker's temperature gauge shows the GPU's.

## Build from source

Rust stable (MSVC toolchain, `rustup default stable-x86_64-pc-windows-msvc`). From `pc-agent/`:

```powershell
cargo run --bin MeowBoxAgent -- --dev --open   # tray + window, next to an installed agent (no single-instance check)
cargo run --bin meowbox -- run -v              # headless, log to the console
cargo test                                     # unit + integration tests (fake speaker, relay, Meow Coding)
cargo clippy --all-targets -- -D warnings
cargo fmt
```

`MeowBoxAgent.exe` dev flags: `--dev` (skip the single-instance check), `--open` (open the window at start),
`--selftest-dialog N` (open and close the window N times, then quit), `-v`. With `--dev`, for screenshots:
`--theme <id>` (instead of the speaker's), `--dark` / `--light` (instead of Windows'), `--page settings`.

Generated, edit the source instead: `src/ui/themes_gen.rs` (window palettes, from `firmware/themes/*.json` by
`firmware/tools/gen_themes.py`; the `"agent"` block of each theme holds the window's colours and dark palette),
`assets/fonts/*.ttf` (display fonts, `firmware/tools/fonts/gen_theme_fonts.py --agent`), `assets/meowbox.ico` and
`packaging/wizard-*.bmp` (`py -3.14 assets/make_art.py`). The tray icons `assets/icons/cat-<state>-<size>.png` are
hand-drawn pixel art and the sources themselves (`make_art.py --pixel` redraws them from scratch); `build.rs` embeds
them. `cargo test` fails if a theme's colours are not readable in the window (WCAG contrast).

Layout (`src/`): `link.rs` (the `/ws/pc` link and the core), `discovery.rs`, `stats.rs`, `media.rs`, `tts.rs`,
`relay/` (YouTube relay, MP4 → ADTS, job objects), `tools/` (yt-dlp, ffmpeg), `coding/` (Meow Coding), `ui/` (tray,
toasts, the egui window: `mirror.rs`, `connect.rs`, `settings.rs`, `theme.rs`, `strings.rs` in Vietnamese and
English), `win/` (Windows bits), `cli.rs`, `config.rs`, `log.rs`. Tests that need the network or touch a real
profile are off by default (`cargo test -- --ignored` downloads ffmpeg into `%LOCALAPPDATA%`, point it somewhere
disposable).

### YouTube relay

```
GET http://<pc>:8765/yt/info?url=<link>                → JSON: type, title, duration, is_live, entries (playlist)
GET http://<pc>:8765/yt/stream?url=<link>              → audio/aac (live: video/mp2t), header icy-name = title
GET http://<pc>:8765/yt/stream?url=<playlist>&index=N  → item N of the playlist (from 0)
GET http://<pc>:8765/yt/ping                           → {"ok":true} (the speaker's relay_test)
```

Only the connected speaker's IP may use it, anything else gets 403. `url` must be a YouTube `http(s)://` link
(youtube.com, youtu.be), else 400.

## Build the installer

Rust and Inno Setup 6 (`winget install JRSoftware.InnoSetup`).

```powershell
.\build.ps1             # cargo test + cargo build --release (MeowBoxAgent.exe + meowbox.exe) + Inno Setup
.\build.ps1 -SkipTests
```

Output: `packaging\build\MeowBoxAgent-Setup-<version>.exe` (~6 MB, no yt-dlp or ffmpeg inside). The version is
`version` in `Cargo.toml` (the same number as the firmware).

## Test without a speaker (mock)

`mock_speaker.py` (Python, aiohttp) plays the speaker: `/ws/pc` (one PC at a time, `busy`, `take`), `/api/ping`,
`/api/theme` (`--theme zen`), `/api/notify*`, `/api/pc/relay_test` as the firmware does (no mDNS/UDP: the agent does not use them).

```powershell
py -3.14 mock_speaker.py --port 8081 --name MeowBox-AAAA
py -3.14 mock_speaker.py --port 8082 --name MeowBox-BBBB     # a second speaker: tests the name check
meowbox --config test.json config --set-host 127.0.0.1:8081 --token test --speaker MeowBox-AAAA --relay-port 8797
meowbox --config test.json run
curl "http://127.0.0.1:8081/mock/cmd?cmd=vol_up"             # the mock sends a media_cmd to the agent
```

A test agent next to an installed one: give it another `--relay-port` and `coding_port`, and run it with `APPDATA`,
`LOCALAPPDATA` and `USERPROFILE` pointing to a scratch folder, or it uses (and refreshes) the real settings, tools and
Claude Code hooks.

## Limits

- Windows only (Media Session, Core Audio, WinRT text-to-speech, Network List Manager).
- No CPU temperature (`cpu_temp` is always `null`); GPU temperature only when the driver reports one.
- A song's `pos`/`len` depend on the app. Chrome reports 0 for live videos.
- The relay is for personal use. When YouTube changes, update yt-dlp in Cài đặt (or wait for the daily check).
- Two agents on one PC (say `meowbox run` while the tray app runs) take the speaker from each other.
- The installer is not code-signed: SmartScreen says "Unknown publisher", click *More info → Run anyway*.
- The settings window draws with OpenGL; while it is open it uses ~125 MB (the graphics driver), back to a few MB
  when it closes.
