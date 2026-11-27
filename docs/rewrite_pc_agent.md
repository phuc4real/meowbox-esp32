# Rewrite the PC agent in Rust

Status: phases 0–3 done (2026-11-27), crate in `pc-agent-rs/`. Replaces the Python agent in `pc-agent/` (0.2.5, ~3,000 lines) with a
Rust build that behaves the same for the speaker and the user. Estimate: **20–25 working days**, in 6 phases that
each end in something that runs.

## Why

- Installer ~20 MB (PyInstaller + Python runtime) → target **≤ 8 MB** (yt-dlp and ffmpeg are downloaded, as today).
- Idle RAM of the tray app well below the Python build (target ≤ 20 MB working set, measure both).
- PyInstaller exes get flagged by antivirus / endpoint protection more often than a plain native exe.
- Protocol messages become typed structs (serde), so a wrong field is a compile error, not a silent `None`.

## Fixed decisions

| Topic | Choice |
|---|---|
| Language / target | Rust stable (1.100 on this PC), `x86_64-pc-windows-msvc` only |
| UI | `egui` via `eframe` (dialogs); the tray straight on `Shell_NotifyIconW` (`src/ui/tray.rs`, see phase 0) |
| yt-dlp | the official **onedir** build `yt-dlp_win.zip` (~24 MB, `yt-dlp.exe` + `_internal\`), checked against the release's `SHA2-256SUMS` |
| ffmpeg | unchanged: BtbN `win64-lgpl-shared`, downloaded on the first live stream |
| Compatibility | same `%APPDATA%\MeowBox\agent.json`, `hook.key`, `agent.log`, `%LOCALAPPDATA%\MeowBox\bin`, Run key name, firewall rule names, installer AppId, exe names. An upgrade over 0.2.x keeps every setting |
| Protocol | `/ws/pc` proto 2 and the relay HTTP API stay byte-compatible: no firmware change |
| UI language | Vietnamese, as today; all strings in one `strings.rs` (no English UI in this rewrite) |

## Layout

New crate at `pc-agent-rs/` while the Python agent is still the shipped one; at cut-over (phase 5) it moves into
`pc-agent/` and the Python code is deleted. `mock_speaker.py` stays (Python, test tool only).

```
pc-agent-rs/
  Cargo.toml            # one package, two bins, release profile: lto, codegen-units=1, strip, opt-level="s"
  build.rs              # winresource: icon, version info, manifest (per-monitor DPI v2, common controls 6)
  src/
    bin/meowbox.rs      # console exe: CLI (run, notify, say, wifi-setup, tools, coding, config)
    bin/tray.rs         # [[bin]] name = "MeowBoxAgent", windows_subsystem = "windows"
    lib.rs              # APP_DIR, CONFIG_PATH, VERSION (from Cargo.toml), PROTO = 2, REPO
    config.rs           # agent.json load/save (atomic), defaults, unknown keys kept on save
    state.rs            # AgentState + tokio::sync::watch channel
    log.rs              # tracing: console, or agent.log rotated at 1 MB × 3; panic hook writes to the log
    discovery.rs        # /api/ping, ping_detail, find_speaker, same_subnet, explain_failure
    link.rs             # Core: the /ws/pc loop, hello, sender, backoff, commands from the UI
    stats.rs            # sysinfo + nvidia-smi + LibreHardwareMonitor (parse_lhm)
    media.rs            # GSMTC read / commands, endpoint volume, media keys
    tts.rs              # WinRT SpeechSynthesizer → WAV bytes
    speaker_api.rs      # speaker_request (notify, say, wifi portal, token check, relay_test)
    update.rs           # GitHub latest release, version compare, download installer
    relay/mod.rs        # axum server, allowlist layer, /yt/info /yt/stream /yt/ping
    relay/ytdlp.rs      # argv building, -J cache (30 min), format picking, headers
    relay/mp4adts.rs    # streaming MP4 → ADTS
    relay/job.rs        # Job Object per stream (kill tree, also on crash)
    tools/ytdlp.rs      # onedir install / update / version / maintain
    tools/ffmpeg.rs     # install with progress, path lookup, status
    coding/tracker.rs   # Session, Tracker (state machine, snapshot, limits)
    coding/transcript.rs
    coding/server.rs    # 127.0.0.1 /hook and /status
    coding/hooks.rs     # ~/.claude/settings.json install / refresh / uninstall
    win/                # single_instance, autostart, netlist (INetworkListManager), wifi_name,
                        # firewall (elevated netsh), local_ip_towards, open_url / open_folder
    ui/tray.rs          # tray icon, menu, status line, toasts
    ui/icon.rs          # cat icon drawn with tiny-skia (port of cat_icon), 3 status dots
    ui/fonts.rs         # Segoe UI + Segoe UI Symbol loaded from C:\Windows\Fonts (Vietnamese, ✓ ● ✕)
    ui/connect.rs       # "Kết nối loa" 3-step wizard
    ui/settings.rs      # "Cài đặt" window
    ui/strings.rs
  tests/                # integration tests (fake speaker, relay, coding server)
  fixtures/             # small synthetic m4a files + golden ADTS output
```

## Crates

| Need | Crate |
|---|---|
| async runtime | `tokio` (rt-multi-thread with 2 workers, net, process, time, sync, fs) |
| HTTP client | `reqwest` with `native-tls` (SChannel: Windows cert store, works behind corporate TLS inspection, smaller than rustls) |
| WebSocket client | `tokio-tungstenite` |
| HTTP servers | `axum` (+ `ConnectInfo<SocketAddr>` for the caller's IP) |
| JSON | `serde`, `serde_json` |
| CLI | `clap` (derive) |
| logging | `tracing`, `tracing-subscriber`, own size-rotating writer (~50 lines) |
| PC stats | `sysinfo` |
| Windows APIs | `windows` (Media_Control, Media_SpeechSynthesis, Storage_Streams, Win32 Audio endpoint volume, SendInput, NetworkListManager, JobObjects, ShellExecuteW, CreateMutexW), `winreg` |
| dialogs | `eframe` / `egui` |
| icon drawing | `tiny-skia` |
| downloads | `zip`, `sha2`, `hex`, `tempfile` |
| resources | `winresource` (build dependency) |

Keep the dependency list to this; anything new needs a reason in the PR.

## Process and threads

- **Main thread (MeowBoxAgent.exe)**: a plain Win32 message loop and a hidden window that owns the tray icon. Its
  window procedure only queues events (click, menu, wake, Explorer restarted); the app drains the queue from the
  loop, or from the dialog's frame while a dialog is open. Other threads wake it with `PostMessageW` (`Waker`), which
  also works while eframe's loop is running. The menu is built fresh each time it opens (`TrackPopupMenu`).
- **Dialogs**: `eframe::run_native` with `run_and_return = true`, started from the main loop when "Kết nối loa…" or
  "Cài đặt…" is clicked and returning when the window closes. While it runs, winit pumps the same thread's messages,
  so the tray keeps working. One window with two pages (Connect, Settings) instead of two windows; opening the other
  one from the tray while it is open just switches the page.
  *Phase 0 spike proves this* (open, close, reopen ten times; tray menu usable while the window is open; no leaked
  windows). Fallback if it misbehaves: run eframe for the whole lifetime with an invisible root viewport and the
  dialogs as deferred viewports.
- **Core thread**: a tokio runtime (COM initialised MTA in `on_thread_start`) running the link, the relay, the
  coding server and maintenance tasks. The UI talks to it through a `CoreHandle`:
  `mpsc` commands (`ReconnectNow { take }`, `AllowIp(ip)`, `ClearAllowed`, `ConfigChanged`, `Stop`) and a
  `watch::Receiver<AgentState>`; long UI actions (ping, token check, relay test, downloads) are spawned on the
  runtime and report back through a channel the dialog polls, with `ctx.request_repaint()`.
- **meowbox.exe run**: same core, run on the main thread with Ctrl+C handling, no UI code linked in.
- `meowbox tray` starts `MeowBoxAgent.exe` (kept for old shortcuts). The old `meowbox ytdlp …` command is gone.

## Behaviour to keep (parity checklist)

Each line becomes a test or a manual check in phase 6.

**Discovery**: typed `host` first (how = manual), then `last_host` (how = last); a set `speaker` name must match;
`/api/ping` answer must have a `name` starting with `MeowBox`; ping 2 s, ping_detail 2.5 s with
`bad_address / not_meowbox / no_answer`, plus `other_network` when the IP is not on a local subnet.

**Link**: `ws://<host>/ws/pc`, ping every 15 s and drop the link when pongs stop. Hello
`{"t":"hello","name","ver","proto":2,"token","relay":"http://<local ip of this socket>:<port>","take"?}`,
`take` sent once after "Giành quyền". Connected right after the hello (no welcome); `last_host` and an empty
`speaker` are saved. Sender every 1 s: coding summary when changed or every 30 s, stats unless paused, media when
changed (ignoring `pos`) or every 5 s. Incoming: non-text ignored; invalid JSON and **objects with a repeated key**
ignored (serde derive rejects duplicate fields: test it); `busy` → wait 60 s, `auth` → 60 s, `proto` → 300 s,
`media_cmd` → run it and send media at once. Backoff 1 → 2 → … → 30 s, back to 1 after a normal session; the wait
counts down in `retry_in` and "Nối lại loa ngay" cuts it short and closes an open socket.

**Stats**: cpu %, ram %, disk % of `%SystemDrive%`, net up/down KB/s from counters, `gpu`/`cpu_temp`/`gpu_temp`
null unless nvidia-smi (3 s timeout) or LibreHardwareMonitor (`localhost:8085/data.json`, 0.5 s, retried after
30 s) gives them; LHM wins; same sensor picking as `parse_lhm`; all values rounded.

**Media**: current GSMTC session; state 4 = playing, 5 = paused, else stopped; position extrapolated from
`LastUpdatedTime` while playing; app = AUMID after `!`, without `.exe`. Commands through GSMTC, falling back to the
media key; volume ±5 % on the default endpoint, falling back to the volume keys. Message shape exactly as today.

**TTS**: voice whose display name + language contains `--voice`, else the first `vi*` voice, else the default;
output is the WAV the synthesizer gives.

**CLI**: same commands, options, defaults and Vietnamese messages as `cli.py`, including `--config`, `--host`,
`-v`, `--version`, and the old habit "no command but extra arguments = `run`". Exit codes as today.

**Relay**: listens on `0.0.0.0:<relay_port>`; only the connected speaker's IP and IPs added by the connect wizard,
everything else 403. URL must match the YouTube regex and contain no whitespace; yt-dlp always gets
`--no-warnings --quiet --no-progress` and the URL after `--`. `-J --flat-playlist [--no-playlist]`, 90 s timeout,
cached 30 min. Playlist `index`, 404 out of range. AAC pick (mp4a, audio-only, http(s), ≤ 192 kbps, best abr), HLS
pick for live (233/234 bitrates known). Headers `Content-Type`, `Cache-Control`, `icy-name`, `icy-br`, `X-Is-Live`,
`X-Duration`, `X-Playlist-Index/Count`, sent **with the first audio bytes** so a failure before that is still a 502.
Three paths: AAC → `Mp4ToAdts`; live → yt-dlp + ffmpeg copy into MPEG-TS; no AAC → yt-dlp | ffmpeg AAC 128k ADTS.
ffmpeg missing: start the download (toast), wait up to 6 s, else 503. Info JSON in a temp file for
`--load-info-json`, deleted after. Client gone → processes killed; Quit → all killed. Log lines "relay start/stop,
KB sent".

**yt-dlp (onedir)**: see the next section.

**ffmpeg**: `ffmpeg` in PATH first, else `bin\ffmpeg\ffmpeg.exe`; newest `ffmpeg-nX.Y-latest-win64-lgpl-shared-*.zip`
of BtbN's latest release, checked against `checksums.sha256`, only `bin/ffmpeg.exe`, `bin/*.dll` and `LICENSE.txt`
extracted, swapped in atomically; one download at a time with a percentage for the Settings page.

**Meow Coding**: port `coding.py` line by line: event handling (late PostToolUse/PreToolUse rules, `stop_at`
3 s window, AskUserQuestion, permission prompts), stale timers (5 / 10 / 60 min), state order, at most 4 sessions,
transcript reading (append-only offset, line still being written waits, usage counted once per message id,
`<synthetic>` ignored, ai-title), context window per model, model label, `git_branch` reading `.git/HEAD` only (no
git process, no UNC paths), `transcript_ok` (only under the Claude folder). Server on `127.0.0.1` only: loopback
caller, no `Origin`, `Content-Type: application/json`, `X-MeowBox-Hook` compared in constant time, body ≤ 4 MB.
`/status` answers the status-line text. `settings.json`: same hook and status-line commands (curl, byte for byte),
keep others' hooks and status line, `.meowbox.bak` of the first version, atomic write, refresh when stale.

**Tray**: grey / green / red dot; status lines and tooltip (≤ 127 chars) exactly as `status_line()`; same menu
items, order, checkmarks and visibility; left click = open the speaker's web page; "Gửi thông báo thử" toasts on
error; first run (no `speaker`) opens the wizard after 0.8 s; update check 5 s after start, then daily (agent toast
+ yt-dlp maintenance) when `auto_update`. Second launch opens the speaker's page and exits (named mutex
`Local\MeowBoxAgent`).

**Connect wizard**: 3 steps as `connect_dialog.py`: IP (auto-check the saved one, re-check after an edit, reasons +
Wi-Fi name + help link on failure), token (open the Notifications page, check with `/api/notify/history`, Next only
after a passing check, save + reconnect), relay (busy port → new port field; relay_test waits up to 10 s for the
link, Public network → "allow" button that runs the elevated netsh). Closing clears the extra allowed IPs.

**Settings**: agent version check → download `MeowBoxAgent-Setup-<v>.exe` to temp, run it, quit; yt-dlp and ffmpeg
status, update and install; auto-update and autostart checkboxes; relay port (1025–65535, restart needed); log
folder and GitHub buttons.

## yt-dlp onedir

- Asset `yt-dlp_win.zip` from `github.com/yt-dlp/yt-dlp/releases/latest/download/`, checksum line for
  `yt-dlp_win.zip` in `SHA2-256SUMS` (checked 2026-11-27: release 2026.11.23 has it, 23.9 MB).
- Installed into a **versioned folder** `%LOCALAPPDATA%\MeowBox\bin\yt-dlp\<tag>\` (exe + `_internal\`), plus
  `bin\yt-dlp\current` holding the tag. Windows locks a running exe's files, so an update never touches the folder in
  use: new streams use the new tag at once, the old folder is deleted at the next start (or after the last stream
  using it ends). An interrupted extraction (`<tag>.partial`) is removed and redone.
- Version = the `current` tag (no process start); `meowbox tools status` also runs `yt-dlp.exe --version` to show
  that the install works.
- Same maintenance rules as today: install at agent start if missing, update at most once a day via the
  `yt-dlp.checked` stamp when `auto_update`, errors only logged. The installer still runs `meowbox tools update`.
- Migration: the installer deletes `bin\yt-dlp.pyz`, `yt-dlp.tmp` and `yt-dlp.checked` at cut-over (phase 5), not the agent: the Python agent may still be using them before that.
- Started with `CREATE_NO_WINDOW`, inside a Job Object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, one job per
  stream (yt-dlp + ffmpeg). Dropping the job kills the tree (replaces `taskkill /T`), and a crashed agent leaves no
  orphan processes.
- Cost: first install downloads ~24 MB instead of ~3 MB; a stream starts faster than with the pyz (no zip import),
  measure in phase 2.

## Phases

### Phase 0: skeleton and UI spike (1.5 days) — done 2026-11-27

Result:
- `tray-icon` dropped: it has no balloon/toast, which the agent needs. Own tray on `Shell_NotifyIconW` (~300 lines):
  icon, tooltip, balloon (`NIF_INFO`), popup menu with default/checked/greyed items, re-add after Explorer restarts.
- eframe `run_and_return` works from the tray loop: `--selftest-dialog 20` opens and closes the window 20 times in a
  few seconds; handle count stays flat. Window borrows the app (`AppCreator<'app>`), so no Rc/RefCell.
- Segoe UI + Segoe UI Symbol render all Vietnamese letters and ✓ ● ○ ✕ › ↗.
- Sizes (release): `MeowBoxAgent.exe` 6.1 MB (egui + glow + accesskit + default fonts), `meowbox.exe` 0.3 MB.
- Memory: tray only 11 MB working set / 2 MB private (Python 0.2.5: ~57 MB). Window open ~88 MB (the OpenGL
  driver). The driver keeps ~36 MB committed after the window closes; the app trims its working set then, so it is
  back to ~2 MB in RAM (the rest sits in the page file until a dialog opens again).
- Dev flags of `MeowBoxAgent.exe`: `--dev` (skip the single-instance check, to run next to an installed agent),
  `--open` (open the window at start), `--selftest-dialog N`, `-v`.
- Not checked by hand yet: right-click menu, left click, balloon, Explorer restart, 150 % DPI.

Original scope:

- Crate, two bins, `build.rs` resources, release profile, `config`, `log`, `state`.
- Spike: tray icon + menu + message loop, eframe window opened/closed/reopened from the menu, Vietnamese text with
  Segoe UI, DPI scaling at 100/150 %. Decide run_and_return vs. persistent viewport here.
- Done when: `MeowBoxAgent.exe` shows the cat in the tray, opens an empty window with Vietnamese text, quits cleanly;
  exe sizes noted.

### Phase 1: headless core (6–7 days)

Result (2026-11-27):
- `discovery`, `link` (CoreHandle / run_forever), `stats`, `media`, `tts`, `speaker_api`, `json`, `cli`, `win`
  (autostart, network category through Network List Manager + GetAdaptersAddresses, Wi-Fi name, firewall rule,
  local IP); `coding` is a stub that sends `{"t":"coding","s":[]}` like the Python agent with no session.
- The tray runs the core: status dot and tooltip follow the link, "Nối lại loa ngay", "Giành quyền nối loa",
  "Gửi thông báo thử" (balloon on failure), "Tạm dừng gửi số liệu", "Chạy cùng Windows".
- Tests: 25 unit (incl. real TTS, GSMTC and Network List Manager on this PC) + 3 integration against a fake speaker
  (`tests/link.rs`: hello/token, coding/stats/media, media_cmd answered at once, repeated-key message ignored, take
  over, auth → 60 s, busy, not found).
- Against `mock_speaker.py`: hello, stats every second, media on change / 5 s, media_cmd, `notify`, `say` (WAV),
  config saved with last_host and speaker.
- Sizes (release): `MeowBoxAgent.exe` 7.9 MB, `meowbox.exe` 3.3 MB. Tray + core idle: 19 MB working set, 8 MB
  private, 8 threads.
- Differences from Python, on purpose: `--config` / `--host` also accepted after the command; a message that is not a
  JSON object is ignored (Python's link loop crashed on it); a "Search now" during the IP check is dropped (same as
  Python, which cleared it before waiting).
- Real speaker (firmware 0.2.5): link held, `notify`, `say`, `wifi-setup status` → 200; the tray app connected and
  ran 10 min. Not yet seen on the device: a media_cmd from the speaker's buttons (no button press reached the log).
  Only one agent per speaker at a time: both builds use the PC name, so the speaker lets either in and they would
  take turns.

Original scope:

- discovery, link, stats, media, TTS, speaker_api, win utils (autostart, single instance, netlist, wifi, firewall).
- CLI: `run`, `notify`, `say`, `wifi-setup`, `config`, `coding status` stub.
- Tests: fake speaker in `tests/` (axum WS: ping, hello/token/busy/take/proto, media_cmd); config round trip keeps
  unknown keys; duplicate-key refusal; parse_lhm on a saved LHM JSON; backoff sequence.
- Done when: `meowbox run --host <speaker>` drives the real speaker (stats tile, media tile, play/pause/next/volume
  from the speaker) and against `mock_speaker.py`, side by side with the Python agent's output.

### Phase 2: relay and tools (4 days)

Result (2026-11-27):
- `relay` (axum: /yt/info, /yt/stream, /yt/ping, allowlist), `relay::yt` (URL check, argv, -J cache, format picks,
  headers), `relay::mp4adts`, `relay::job` (Job Object per stream), `tools::ytdlp` (onedir, versioned folders, daily
  maintenance), `tools::ffmpeg`, `meowbox tools status|update`; toasts from the relay reach the tray.
- mp4adts: same bytes as the Python converter on three synthetic fixtures (`fixtures/`, made by
  `fixtures/make_fixtures.py`), whole and in random chunk sizes; garbage and truncated input give an error, no panic.
- Against the mock speaker with real YouTube: video (AAC itag 140 → ADTS, decodes), live (`/@LofiGirl/live`, itag 234
  → MPEG-TS through ffmpeg, decodes, UTF-8 title with emoji in `icy-name`), 400 for a bad URL, 403 from an IP that is
  not the speaker, 502 for a finished live. A slow client that disconnects: yt-dlp and its ffmpeg are gone at once.
- yt-dlp onedir: 24 MB download, 40 MB unpacked, `--version` in ~1 s. ffmpeg: 9.0.2, 14 s to download and unpack.
- Real speaker: `POST /api/pc/relay_test` → `{"ok":true,"ms":21}` (speaker → PC relay on port 8797), and a YouTube
  station played through the Rust relay.
- Found while porting: FFmpeg-Builds (formerly BtbN) renamed its release files (no `-latest-` any more) and moved
  the repo, so **the Python agent 0.2.5 can no longer download ffmpeg** (its regex finds no asset). The Rust parser
  takes both names.
- The old `bin\yt-dlp.pyz` is left alone while the Python agent may still use it; the installer removes it at
  cut-over (phase 5). The Rust build keeps its own stamp (`bin\yt-dlp\checked`).

Original scope:

- relay server, ytdlp args/cache/picks/headers, mp4adts, Job Objects, tools (yt-dlp onedir, ffmpeg), `meowbox tools`.
- mp4adts golden tests: synthetic tone files made once with ffmpeg (fragmented like YouTube DASH, and plain with moov
  first, HE-AAC and LC, mono and stereo), expected ADTS from the Python `Mp4ToAdts`; also feed in random chunk sizes.
  Only synthetic audio in `fixtures/`, a few tens of KB.
- Ports of `test_agent.py`'s relay checks (URL accept/refuse lists, argv ends with `-- url`, no process for a
  refused URL, allowlist 403).
- Done when: a video, a playlist and a live stream play on the speaker through the Rust relay; disconnecting
  mid-stream leaves no yt-dlp/ffmpeg process.

### Phase 3: Meow Coding (2 days)

Result (2026-11-27):
- `coding` (tracker, helpers), `coding::transcript` (incremental, lock not held while reading), `coding::hooks`
  (install / refresh / uninstall, backup, broken JSON never overwritten), `coding::server` (127.0.0.1, key, no
  Origin, JSON only, 4 MB); `meowbox coding install|uninstall|status`; tray item with checkmark and toasts.
- 12 tests: the coding cases of test_agent.py, outputs of the Python functions for the same inputs (model label,
  short, basename, action, epoch, status line, hook and status commands byte for byte), event order (late tool
  events after Stop, permission prompt before PreToolUse), transcript (split message counted once, sidechain and
  `<synthetic>` skipped, half-written line waits, status line context wins), settings.json round trip.
- End to end in an isolated profile: the curl commands written to settings.json, run as Claude Code runs them, reach
  the Rust server; the mock speaker gets `{"t":"coding","s":[{"name":"…","br":"rust-agent","st":"work","act":"edit",
  "tok":42000,"model":"Opus 5.5","ctx":12}],"lim":{"h5":42,…}}`, then `done` after Stop.
- Note: the real ~/.claude/settings.json on this PC still has the keyless commands of the installed 0.2.5 agent; any
  agent built from current code (Python from source or Rust) rewrites them with the key at start, as designed
  (0.2.5 ignores the key, so it keeps working).
- Sizes (release): `MeowBoxAgent.exe` 9.0 MB, `meowbox.exe` 4.5 MB.

Original scope:

- tracker, transcript, server, hooks, `meowbox coding install/uninstall/status`.
- Port every coding check of `test_agent.py`, plus tracker event-order cases (late PostToolUse after Stop, permission
  prompt before PreToolUse), transcript partial-line case, settings.json install → refresh → uninstall round trip
  with foreign hooks kept.
- Done when: a real Claude Code session shows on the speaker's cat screen with the same states and limits as with the
  Python agent, and the hook commands in settings.json are byte-identical.

### Phase 4: tray and dialogs (4–5 days)

- tray (status line, icon dots, menu, toasts, update check), connect wizard, settings page, fonts, strings.
- Done when: every tray item and both pages work on the device, including first-run flow, busy/take-over, wrong
  token, busy relay port, Public network firewall button, agent update download, ffmpeg download progress.

### Phase 5: packaging and cut-over (2 days)

- `build.ps1`: `cargo build --release` → copy both exes to `packaging\build\dist\MeowBoxAgent\` → Inno Setup.
  `MeowBoxAgent.iss`: same AppId and rules; `[InstallDelete]` for the old PyInstaller files
  (`{app}\_internal`, `{app}\*.pyd`, `{app}\python*.dll`).
- Move `pc-agent-rs/` into `pc-agent/`, delete the Python agent (`meowbox_agent/`, `agent.py`, `requirements.txt`,
  `packaging/entry.py`, `meowbox.spec`, `test_agent.py`); keep `mock_speaker.py`, `install_autostart.ps1` if still
  useful.
- Update docs: `pc-agent/README.md`, `README.md`, `README_vi.md`, `firmware/README.md`, `docs/firmware_v0.2.0.md`
  (2.x agent sections), `docs/plan.md`, `CLAUDE.md` (agent check becomes `cargo test` + `cargo clippy` from
  `pc-agent/`, Python ≥ 3.11 note removed), `.claude/skills/run-meowbox` and `release-docs` skills.
- `firmware/tools/release.py`: build the agent with cargo if it builds the agent today.

### Phase 6: soak and device test (2–3 days)

- Upgrade test: install 0.2.5, configure, install the Rust build over it: speaker, token, autostart, Meow Coding hooks
  and firewall rules all still work; uninstall removes everything as before.
- Fresh Windows user test (no Python, no Rust, no ffmpeg in PATH; offline installer run).
- 24 h soak with the tray app: link drops (speaker reboot, OTA, Wi-Fi off), 50 YouTube plays, Claude Code sessions;
  memory and handle count flat; log rotation works.
- Measure and write down: installer size, exe sizes, idle working set, CPU at idle, stream start time vs. 0.2.5.

## Lint and checks (new rules for CLAUDE.md at cut-over)

- `cargo fmt` (rustfmt.toml with `max_width = 120` to match the repo), `cargo clippy --all-targets -- -D warnings`,
  `cargo test` must pass after touching `pc-agent/`.
- Same privacy rules: no device identifiers in fixtures or tests.

## Risks

| Risk | Mitigation |
|---|---|
| eframe reopen from a tray loop misbehaves | phase 0 spike, persistent-viewport fallback |
| egui does not look native (no system theme, own widgets) | accept; light theme, Segoe UI, Windows-like spacing; dialogs are small |
| WinRT async in tokio (GSMTC, TTS) | await through `windows-future`, or `.get()` inside `spawn_blocking`; COM MTA on runtime threads |
| Behaviour drift from the Python agent | parity checklist above, golden tests from the Python code, side-by-side runs in phase 1–3 |
| Locked yt-dlp files during update | versioned folders (above) |
| Antivirus flags a new unsigned exe | far rarer than PyInstaller; code signing is out of scope, note it if it happens |
| Two exes both large | `meowbox.exe` does not link the UI; check sizes in phase 0 |

## Open questions (defaults used unless decided otherwise)

1. One window with Connect/Settings pages instead of two windows: **yes**.
2. Version number: the Rust agent ships as **0.3.0** (same protocol 2).
3. Develop in `pc-agent-rs/` and move at cut-over, rather than a branch that replaces `pc-agent/` from day one:
   **yes**, so 0.2.x fixes can still ship from `main` meanwhile.
