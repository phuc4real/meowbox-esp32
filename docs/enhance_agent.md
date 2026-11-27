# PC agent: enhancements after 0.3.0

Status: plan, nothing started (2026-11-28). Everything here ships as **0.3.1**, together with the tray fix and the
GPU stats already on `main`. Two parts: **A**, the engine (what the agent does every second, found by
a review of the Rust code after 0.3.0) and **B**, the UI/UX redesign parked in `docs/roadmap.md` ("PC agent UI/UX",
written for the Python/tkinter agent, updated here for Rust/egui). The agent no longer has to mirror the Python one:
only the behaviour the speaker and the user see must stay.

Baseline (installed 0.3.0, connected, idle, measured 2026-11-28 over 30 s): **0.5 % of one core**, 10.7 MB private,
34 MB working set, 12 threads. Window open: ~88 MB (OpenGL driver), trimmed when it closes. Exes: `MeowBoxAgent.exe`
9.5 MB, `meowbox.exe` 4.5 MB; installer 5.5 MB.

## Part A: engine

### A1. Tray icon re-rendered every second while not connected — done (step 1)
`ui/mod.rs` `refresh()`: the status line carries the countdown "(thử lại sau N s)", so it changes every second and each
change draws the cat again (tiny-skia), makes a new `HICON` and calls `Shell_NotifyIconW`. Fix: keep the icon per dot
colour (three cached icons, rebuilt only when the DPI-dependent size changes) and update the tooltip alone with
`NIF_TIP`. Test: a unit test that `refresh()` with only `retry_in` changing does not call `make_icon` (count calls).

### A2. nvidia-smi started every second on NVIDIA PCs — do
`stats.rs` `nvidia()`: a new process per sample (tens of ms of CPU, up to 3 s timeout) that also delays the media
message sent after it. Windows now gives GPU load and temperature for any vendor (`gpu.rs`). Drop nvidia-smi
entirely (decided 2026-11-28): remove `nvidia()`, the `nvsmi` field and `which()` if nothing else uses it, and the
nvidia-smi mentions in `gpu.rs`, `stats.rs` and `pc-agent/README.md`. If an NVIDIA card turns out to report no
temperature through D3DKMT (only checked on AMD so far), the speaker shows none for it, as for integrated GPUs.

### A3. A new HTTP client for every request — done (step 1)
`discovery::lan_client()` and `tools::web_client()` build a `reqwest::Client` (connection pool, SChannel TLS setup) on
each call: every ping, notify, token check, GitHub call. Fix: two `LazyLock<reqwest::Client>` (LAN: no proxy; web:
user agent, system proxy). No behaviour change.

### A4. Media dead until restart if GSMTC fails once — done (step 1)
`media.rs`: the `OnceCell` caches a failed `GlobalSystemMediaTransportControlsSessionManager::RequestAsync()` as
`None` for good (e.g. at logon, before the service is up): the media tile stays empty. Fix: on failure, retry at most
every 30 s. Test: inject a failing factory, then a working one.

### A5. Media by events instead of polling every second — do (largest item, ~½ day)
The sender asks GSMTC for properties, timeline and playback info every second. Subscribe instead:
`CurrentSessionChanged` on the manager, `MediaPropertiesChanged` / `PlaybackInfoChanged` / `TimelinePropertiesChanged`
on the session; send `media` when they fire, plus every 5 s while playing (the speaker extrapolates `pos`). Gains:
less CPU and Windows wake-ups, a track change on the PC shows on the speaker at once instead of up to 1 s later.
Keep the polling path as a fallback when subscribing fails. Test: the change-detection logic on a fake session.

### A6. Disk list rescanned every second — done (step 1)
`stats.rs` `d.refresh(true)` enumerates the disks each second; the disk % moves slowly. Refresh every 30 s, send the
last value in between.

### A7. GPU temperature opens and closes every adapter every second — done (step 1)
`gpu::temperature()`: `D3DKMTEnumAdapters2` + query + close per sample. Read it every 5 s (temperatures move slowly)
and keep the adapter list; reopen only if a query fails (driver update, eGPU unplugged).

### A8. Speaker search pings one address after the other — done (step 1)
`discovery::find_speaker`: a dead typed IP costs its 2 s timeout before the last working IP is tried. Ping both at
once and take the typed one if both answer (same preference as now). Shortens reconnects after the speaker's IP
changed.

### A9. Relay copies — small
`relay/mod.rs` allocates a new 16 KB buffer per read; `mp4adts.rs` copies each AAC frame once more (`take().to_vec()`
then into the output). Reuse one read buffer, write frames straight from the input buffer. Covered by the existing
byte-for-byte fixture tests.

### A10. Settings page starts `ffmpeg -version` every second while downloading — small
`ui/settings.rs`: the 1 s refresh during a download reads the version too. Read only the status (percentage) then, and
the version once when the download ends.

### A11. Binary size — small
`reqwest` features `http2` and `charset` are not needed (the speaker and GitHub work over HTTP/1.1, all bodies are
UTF-8 JSON or bytes). Drop them; measure both exes before and after. Also try `opt-level = "z"` for `meowbox.exe` only
if it saves more than 0.5 MB (profile per binary is not possible in one package: measure first, decide then).

### A12. Python-parity leftovers — tidy
Now that the Python agent is gone: `json::round` (half-to-even like Python's `round`) can be a plain round; the
"no command means `run`" argv rewrite (`cli::with_default_command`) and `meowbox tray` can go (the installer starts
`MeowBoxAgent.exe`, nobody types `agent.py --no-relay` any more). Keep the repeated-key refusal on `/ws/pc`: it is a
safety check, not parity.

### A13. A refusal can be lost before the agent reads it — do
The agent sends `coding`, `stats` and `media` right after `hello`, before the speaker has answered. When the speaker
refuses (`busy`, `auth`, `proto`) it sends the answer and closes (`pc_app.c` `refuse()`); closing with the agent's
messages still unread makes lwIP reset the connection, and the reset can drop the answer before the agent reads it.
The agent then sees a plain disconnect and retries every second instead of showing "busy" or "wrong token". The fake
speaker in `tests/link.rs` had the same race (`wrong_token_and_busy` failed about 1 run in 3, also on 0.3.0); it now
closes like a well-behaved peer, which hides the problem in tests. Fix on the agent side: after `hello`, wait up to
~300 ms for a refusal before sending anything else (the speaker answers at once), so nothing is unread when it closes.
Optionally on the speaker: read what is pending before closing.

## Part B: UI/UX redesign (from the roadmap)

What exists in 0.3.0: one egui window with two pages (Kết nối loa as a 3-step wizard, Cài đặt), Segoe UI, egui's
light look, Vietnamese only; tray icon drawn in code (cat + status dot); a menu of 13 items plus 2 that show only when
needed; left click opens the speaker's web page; toasts for ffmpeg download, a new agent version, test-notification
errors and Meow Coding on/off.

Direction (design review, 2026-11-28): the agent is the speaker's remote, so it **wears the speaker's theme, fonts
included**, and its one memorable element is a **mirror of the speaker's Computer screen** at the top of the window.
Everything else stays quiet: left-aligned lists, grouping by space, no card grid, no decoration the speaker does not
have. Status never relies on colour alone.

### B1. The window wears the speaker's theme
- **Palettes and shapes:** `firmware/tools/gen_themes.py` also writes `pc-agent/src/ui/themes_gen.rs` (generated and
  committed with its source, like every generated file here; add it to the "Generated files" table in CLAUDE.md): per
  theme the day colours (`bg`, `surface`, `text`, `dim`, `line`, `accent`, `on_accent`, `ok`, `warn`, `alert`) and
  `shape` (radius, border, button style, shadow). The agent asks `GET /api/theme` (no auth) after connecting and
  restyles; before the first connection it uses Swiss.
- **Fonts:** each theme's display font, used for headings and the big numbers of the mirror (B2); body text stays
  Segoe UI for legibility at small sizes. TTF sources are already in `firmware/tools/fonts/src/` (Archivo, Be Vietnam
  Pro, Chakra Petch, Cormorant Garamond) and `gen_theme_fonts.py` already subsets them to Vietnamese for the web: have
  it also write subset TTFs into `pc-agent/assets/fonts/` (generated). Brutalism's Times New Roman ships with Windows.
  Budget: ≤ 150 KB per theme in the exe; the font of the current theme is loaded when the window opens.
- **Dark mode** follows Windows (`AppsUseLightTheme` = 0), with an agent palette derived from each theme's day colours
  for a desktop window: **not** the speaker's `night` palette, which is made for a dim bedroom screen (Cyberpunk's
  night text `#8C1A6E` on black is far too dark to read on a monitor). The derived palettes live next to the generated
  ones in `gen_themes.py` (`agent_dark` per theme in `themes/<id>.json`).
- **Accent use rule:** the accent is a fill (buttons, the selected tab, the current step), always with `on_accent` text;
  never text or a link colour on `bg` (Brutalism's `#FFFF00` on white is invisible as text). Links use `text` underlined.
- **Contrast check in `cargo test`:** every text/background pair the agent uses, for the 4 themes × light/dark, ≥ 4.5:1
  for body text and ≥ 3:1 for large numbers and UI borders (WCAG). A failing pair fails the build, so a theme change in
  `themes/*.json` cannot make the agent unreadable unnoticed.
- **No `fx`:** scanlines, RGB split, HUD corners, tilt and segmented bars stay on the speaker. Brutalism keeps its hard
  offset shadow on buttons (a shape, not an effect).

### B2. The window: speaker mirror + one quiet list
- **Speaker mirror** at the top, in the theme's display font and colours: the speaker's name and IP, the link state as
  words, and the same numbers the speaker's Computer screen shows (CPU, RAM, GPU, temperature) plus what is playing.
  The data is what the core just sent (`stats` and `media`), so the window shows exactly what the speaker gets. Not
  connected: the block greys out and its last line says why and what to do (B4 wording), with the button that does it.
- Below it, two tabs (Kết nối, Cài đặt) in sentence case, and a single left-aligned column: label on the left, value or
  control on the right, groups separated by space, no cards, no shadows (except Brutalism's buttons, B1).

```
┌──────────────────────────────────────────────┐
│  MeowBox-1A2B   192.168.1.50      đã nối      │  theme display font
│  CPU 14   RAM 55   GPU 18   44°               │  big numbers, as on the speaker
│  ♪ Lofi Girl, christmas lofi radio            │
├──────────────────────────────────────────────┤
│  Kết nối    Cài đặt                           │
│                                               │
│  1  IP loa          192.168.1.50          ✓   │
│  2  Token           ••••••••••            ✓   │
│  3  YouTube relay   loa gọi được (21 ms)  ✓   │
│                                               │
│                         [ Gửi thông báo thử ] │
└──────────────────────────────────────────────┘
```

- **Connect is one page, not a wizard:** three rows, each checked by itself (the agent already auto-checks the saved
  IP, the saved token and the relay). A row turns ✓ when it passes; the first row that fails expands in place with the
  reason and the fix (Wi-Fi name, the Notifications page button, the firewall button), and the cursor goes to it. The
  numbers stay because the rows really are a sequence: the token cannot be checked without the IP, the relay without
  the token. A wrong token later shows as row 2 failing, without walking through step 1 again. "Gửi thông báo thử"
  moves here from the tray menu.
- **Settings** in the same list style: agent version and update, yt-dlp, ffmpeg (with progress bars), daily update
  check, run at Windows start, relay port, language (B4), and at the bottom: log folder, GitHub, about.
- **Progress:** downloads (yt-dlp, ffmpeg, agent installer) show a bar with MB done / total.
- **Window:** one window only (opening it again focuses it), position and size remembered in agent.json, max content
  width ~520 px at 100 %. Checked at 100, 125, 150, 200 % scaling.
- **Keyboard:** Tab order follows the reading order, visible focus ring in the theme's `line` colour, Enter on a row's
  field runs its check; checked with Narrator (accesskit is already in).
- **Empty and error states,** one sentence that says what to do and one button that does it: no speaker yet, speaker
  lost, token refused, speaker held by another PC, relay port taken, yt-dlp missing, ffmpeg download failed.
- **Motion:** none, except the progress bars and a row expanding when its check fails.
- **Screenshots** of every state above × 4 themes × light/dark through UI Automation (the `run-meowbox` recipe),
  compared before/after.

### B3. Tray
- **Left click opens the agent window** (the mirror), as Windows users expect from a tray icon; the speaker's web page
  moves into the menu.
- Menu:
  1. Status line (greyed), "Giành quyền nối loa" when another PC holds the speaker.
  2. Mở cửa sổ MeowBox Agent (default, bold) · Mở trang loa.
  3. Tạm dừng gửi số liệu · Meow Coding (checkboxes).
  4. "Có bản x.y.z" while an update is waiting.
  5. Thoát.
  Kết nối loa and Cài đặt are the window's tabs; Nối lại loa ngay, Chạy cùng Windows, log folder, GitHub, about and
  the test notification live in the window (B2).
- **Status line says what to do, at most ~45 characters;** details (IP, reason) go in the window. Examples:
  "Đã nối MeowBox-1A2B", "Đang tìm loa…", "Mất kết nối với loa, thử lại sau 8 s", "Loa không nhận token",
  "MeowBox-1A2B đang nối với DESKTOP-ABC".

### B4. Words: English, and one voice
- `ui/strings.rs` gets an English table with the same keys (a test checks both have the same keys, as `test_api.py`
  does for the web's `lang/*.json`).
- Language follows Windows (`GetUserDefaultUILanguage`: Vietnamese → vi, anything else → en), with a setting in Cài đặt
  (Tự động / Tiếng Việt / English) saved as `lang` in agent.json.
- **Copy review of every string,** with these rules:
  - The cat's playful voice only where the cat is the subject (Meow Coding, the test notification).
  - Errors say what happened and how to fix it, plainly: no apology, no joke. "Token sai" becomes "Loa không nhận token
    này. Lấy token ở trang Thông báo của loa."
  - An action keeps its name through the flow: the button "Gửi thông báo thử" gives the toast "Đã gửi thông báo thử".
  - Sentence case, plain verbs, no filler.
- The command line keeps its messages as they are (they are for the terminal).

### B5. Icon: the cat shows the state
- **State by the cat's eyes, not by colour alone** (red/green is the pair colour-blind users miss, and a 4–5 px dot at
  16 px is hard to read anyway): connected = eyes open, connecting = eyes half closed, error = × eyes. A small colour
  dot stays as a second cue.
- Hand-drawn pixel versions for the sizes the tray uses: 16, 20, 24, 32 and 40 px (100–250 % scaling), × 3 states.
  PNG sources in `pc-agent/assets/icons/`, embedded; 48 px and up (Explorer, installer, `assets/meowbox.ico`) keep the
  vector drawing, rebuilt by a small generator script.

### B6. Toasts
At most one toast per event kind per 10 minutes. Windows' Focus Assist (do not disturb) is respected by the balloon
API in use (`NIF_INFO`); keep that API.

| Event | Toast |
|---|---|
| Connected for the first time after start | no (the icon's eyes open) |
| Speaker lost for more than 2 minutes after having been connected | yes, once |
| Token refused (`auth`) | yes |
| Taken over by another PC (`busy` while connected) | yes |
| Downloading ffmpeg / done / failed | yes (as now) |
| New agent version | yes, once per version |
| Meow Coding on/off, test notification sent / failed | yes (the user's action, same verb as its button) |

### B7. Installer
- Vietnamese installer language (Inno Setup's unofficial `Vietnamese.isl`, vendored in `packaging/`) besides English,
  picked from the Windows language.
- **Wizard images:** the cat in Swiss colours for Inno's large side image (164×314) and the small top-right image
  (55×58), so the installer no longer looks like the stock template.
- Wording of the tasks; the finish page's "Start MeowBox Agent" opens the agent window on a first install.
- Upgrade from 0.2.x/0.3.x: say in one line that settings are kept.

## Order and estimates

| Step | Items | Estimate |
|---|---|---|
| 1 | A1, A3, A4, A6, A7, A8 (small engine fixes, each with a test) | 3 h |
| 2 | A2 drop nvidia-smi, A10, A11, A12 | 2 h |
| 3 | A5 media by events | ½ day |
| 4 | B3 tray, B6 toasts | ½ day |
| 5 | B4 English and the copy review | ½ day |
| 6 | B1 theme palettes, fonts, dark palettes, contrast test | 1 day |
| 7 | B2 mirror, one-page connect, settings list, states, keyboard, screenshots | 1–1½ days |
| 8 | B5 icon artwork, B7 installer and its images | ½ day |
| 9 | A9 relay copies | 1 h |
| 10 | A13 wait for a refusal after hello | 1 h |

Measured after step 1 (2026-11-28, `meowbox.exe run --no-relay` against `mock_speaker.py`, scratch profile, idle,
30 s after a 10 s warm-up, two runs each): 0.3.0 0.42 / 0.47 % of one core, 7.7–8.2 MB private; step 1 0.62 / 0.42 %,
8.5–8.6 MB private. The difference is within the noise; the tray icon (A1) is not in `run`.

After each step: `cargo test`, `cargo clippy --all-targets -- -D warnings`, the CPU/memory measurement above (connected,
idle, 30 s) written into this file, and for B steps screenshots through UI Automation in a scratch profile.

## Decisions

Settled 2026-11-28: nvidia-smi is dropped (A2); parts A and B ship together as 0.3.1, no separate 0.4.0. In the
design review: the window follows the speaker's theme with its fonts; the tray menu is
shortened as in B3 and left click opens the window; the command line keeps its messages; the connect wizard becomes
one page.
