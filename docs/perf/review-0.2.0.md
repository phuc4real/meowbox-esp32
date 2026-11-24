# Code review, v0.2.0

Section 5.3 of `docs/firmware_v0.2.0.md`. Scope: what changed in `firmware/components` and `firmware/main` between
0.1.0 (`0fad9dc`) and v0.2.0: `player`, `net` (`wifi_mgr.c`, `web.c`), `webui`, `apps` (`update_app.c`, `pc_app.c`,
`radio_app.c`, `system_app.c`), `ui`. Done 2026-11-24.

Severity: **high** = crash, data loss or a feature that stops working; **medium** = wrong behaviour in a rare case;
**low** = robustness or style, no visible effect today.

| Where | Problem | Severity | Status |
|---|---|---|---|
| `net/wifi_mgr.c` `wifi_mgr_hold_awake` / `ps_restore` | The restore timer (esp_timer task) and a new holder (LVGL or update task) could interleave: the timer reads "no holders", a stream starts and sees power save already off, then the timer turns power save back on. The stream would then play with power save on. The window is a few microseconds, 10 s after the last holder let go. The timer was also created lazily without a lock. | medium | fixed: one mutex around the decision and `esp_wifi_set_ps`; timer created in `wifi_mgr_start` |
| `player/player.c` `fetch_task`, `stream_loop`, `hls_loop`, playlist read | `malloc`/`heap_caps_malloc`/`calloc` results not checked (URL buffer, 4 KB read buffer, 8 KB playlist text). Out of memory would crash instead of reporting an error. Mostly from 0.1.0. | low | fixed: stop the session with `err` `no_mem` (added to `API.md`) |
| `tools/release.py:84` | f-string reused its own quote character: a syntax error on Python 3.11 (the agent's minimum), only valid on 3.12+. | low | fixed |
| `apps/update_app.c` `newer()`, `update_available()`, `h_install` | Read `s_m.ver` without `s_lock` while a check may be rewriting it. A check and an install cannot run at the same time (state machine), so at worst a status read sees a half-copied version string for one call. | low | left as is |
| `apps/update_app.c` `h_install` vs `auto_tick` | The state test and the state change are not atomic, so a daily auto check (off by default) could start in the same millisecond as an install. `start_check` refuses while `s_state` is `download`, and `h_install` sets it before creating the task, so the window is a few instructions. | low | left as is |
| `apps/pc_app.c` `send_cmd` | Reads `s_fd` from the LVGL task while the httpd task may set it to -1. `httpd_ws_send_frame_async` with a closed or -1 fd just fails. | low | left as is |
| `apps/pc_app.c` `h_relay_test` | Blocking HTTP call inside an httpd handler (up to 3 s). Other requests wait meanwhile. Only used from the agent's Connect window. | low | left as is (timeout is short) |
| `net/web.c` `/api/ping` | No login, and it returns the MAC. Meant for a DHCP reservation and for the agent. Anyone on the LAN can read the MAC through ARP anyway. | info | by design, documented in `API.md` |
| `net/web.c` `/api/debug` | Plan said "behind `CONFIG_MEOWBOX_DEBUG_API`", but it is always registered. It needs a web login and only reads counters. | info | kept on for 0.2.0 (memwatch and field reports use it); plan updated |
| `ui/ui.c` `inv_cb`/`inv_report` | Temporary redraw counter: one log line every 10 s. | info | stays until the theme UI/UX pass in `docs/roadmap.md` |

## Checked and fine
- **Error paths**: `esp_http_client_cleanup` on every path in `update_app.c` (`http_get_small`), `pc_app.c`
  (`h_relay_test`) and `player.c`. `cJSON_Delete` after every `web_read_json`/`cJSON_Parse`. `esp_https_ota_abort`
  on every failure after `begin`.
- **Tasks with PSRAM stacks never write flash**: `fetch` (network only), `upd_check` (network only, keeps the
  manifest in RAM), `discovery` (UDP only). `upd_install` writes OTA flash and `/data/update.json`: internal-RAM stack.
  Tasks created with `...WithCaps` are deleted with `vTaskDeleteWithCaps`.
- **LVGL from other tasks**: `update_app.c` takes `board_lvgl_lock` before `ui_overlay_show`. `radio_app.c`,
  `system_app.c` and `pc_app.c` touch LVGL only from `lv_timer` callbacks (LVGL task).
- **String bounds**: every copy from JSON, headers or the network goes through `snprintf` with `sizeof` of the target.
  The pack lookup (`webui_find`) rejects paths ≥ 56 bytes and checks every entry once at startup.
- **Timeouts**: manifest 10 s, OTA download 15 s per read, relay test 3 s, player HTTP (from 0.1.0) 8 s, weather 10 s.
  UDP discovery: 2 s receive timeout, at most 5 answers a second.
- **Python**: `python -m ruff check .` adds no findings (the remaining ones are older: `lunar_ref.py` E713,
  `tools/fwimage.py` B007, the known B904). `firmware/web`: `node --check src/app.js`, `test_api.py` ALL PASSED.
