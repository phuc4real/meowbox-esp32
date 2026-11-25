/* System tile (network, battery, memory, mic levels, Wi-Fi setup, screen calibration) and the
 * theme/screenshot web endpoints. */
#include "apps.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "ui.h"
#include "board.h"
#include "net.h"
#include "player.h"
#include "settings.h"
#include "audio_hw.h"
#include "audio_sfx.h"

static lv_obj_t *s_net, *s_addr, *s_bat, *s_mem, *s_bar[AUDIO_MIC_CHANNELS], *s_frames;
static volatile int s_mic_db[AUDIO_MIC_CHANNELS];

static void mic_task(void *arg)
{
    const int frames = 441;
    int16_t *buf = heap_caps_malloc(frames * AUDIO_MIC_CHANNELS * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    if (!buf) vTaskDelete(NULL);
    float acc[AUDIO_MIC_CHANNELS] = {0};
    int blocks = 0;
    bool on = false;
    while (1) {
        bool want = !ui_asleep(); /* the mics only feed the level bars: off while the screen sleeps */
        if (want != on && audio_hw_mic_enable(want) == ESP_OK) {
            on = want;
            for (int c = 0; c < AUDIO_MIC_CHANNELS && !on; c++) s_mic_db[c] = 0;
        }
        if (!on) {
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        if (audio_hw_mic_read(buf, frames) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        for (int c = 0; c < AUDIO_MIC_CHANNELS; c++) {
            float s = 0;
            for (int i = 0; i < frames; i++) {
                float v = buf[i * AUDIO_MIC_CHANNELS + c];
                s += v * v;
            }
            acc[c] += s / frames;
        }
        if (++blocks == 10) {
            for (int c = 0; c < AUDIO_MIC_CHANNELS; c++) {
                float rms = sqrtf(acc[c] / blocks);
                s_mic_db[c] = rms < 1 ? 0 : (int)(20.0f * log10f(rms / 32768.0f) + 90.0f);
                acc[c] = 0;
            }
            blocks = 0;
        }
    }
}

static void sys_tick(lv_timer_t *t)
{
    if (ui_active_tile() != UI_TILE_SYSTEM || !s_net) return;
    char ss[33], ip[16];
    int rssi = 0;
    wifi_mgr_info(ss, sizeof(ss), ip, sizeof(ip), &rssi);
    if (wifi_mgr_mode() == NET_MODE_STA) {
        lv_label_set_text_fmt(s_net, "Wi-Fi %d dBm · %s", rssi, ss); /* a long name gets the "…", not the signal */
        int left = wifi_mgr_portal_left_s();
        if (left >= 0)
            lv_label_set_text_fmt(s_addr, "http://%s\n%s %s, %s %d:%02d", ip, ui_tr("Đang phát", "Hotspot"),
                                  wifi_mgr_ap_ssid(), ui_tr("tắt sau", "off in"), left / 60, left % 60);
        else if (left == -2)
            lv_label_set_text_fmt(s_addr, "http://%s\n%s %s, %s", ip, ui_tr("Đang phát", "Hotspot"),
                                  wifi_mgr_ap_ssid(), ui_tr("có máy đang kết nối", "a device is connected"));
        else lv_label_set_text_fmt(s_addr, "%s.local\nhttp://%s", wifi_mgr_hostname(), ip);
    } else if (wifi_mgr_mode() == NET_MODE_AP) {
        lv_label_set_text_fmt(s_net, "%s %s", ui_tr("Cài đặt: vào Wi-Fi", "Setup: join"), wifi_mgr_ap_ssid());
        lv_label_set_text_fmt(s_addr, "%s %s\nhttp://192.168.4.1", ui_tr("Mật khẩu", "Password"), wifi_mgr_ap_pass());
    } else {
        lv_label_set_text(s_net, ui_tr("Đang kết nối Wi-Fi…", "Connecting to Wi-Fi…"));
        lv_label_set_text(s_addr, "");
    }
    int eta = battery_eta_min(), mv = battery_mv();
    if (mv < 0) mv = 0;
    if (board_is_charging())
        lv_label_set_text_fmt(s_bat, "%s %d%%, %d.%02d V, %s", ui_tr("Pin", "Battery"), battery_pct(), mv / 1000,
                              (mv % 1000) / 10, ui_tr("đang sạc", "charging"));
    else if (eta > 0)
        lv_label_set_text_fmt(s_bat, "%s %d%%, %s %dh%02d", ui_tr("Pin", "Battery"), battery_pct(),
                              ui_tr("còn khoảng", "about"), eta / 60, eta % 60);
    else lv_label_set_text_fmt(s_bat, "%s %d%%, %d.%02d V", ui_tr("Pin", "Battery"), battery_pct(), mv / 1000, (mv % 1000) / 10);
    player_net_t n;
    if (player_net(&n)) { /* while a stream plays: its numbers instead of the version line (4.3) */
        bool slow = n.br && n.kbps + 8 < n.br;
        char br[8] = "?";
        if (n.br) snprintf(br, sizeof(br), "%u", n.br);
        lv_label_set_text_fmt(s_mem, "Radio %u/%s kbps, %s %u s, %s %u", n.kbps, br, ui_tr("đệm", "buffer"),
                              n.buf_ds / 10, ui_tr("ngắt", "drops"), n.under);
        lv_obj_set_style_text_color(s_mem, ui_lc(slow || n.under ? UI_C_WARN : UI_C_DIM), 0);
    } else {
        const char *upd = update_available();
        if (upd) lv_label_set_text_fmt(s_mem, "v%s · %s %s", esp_app_get_description()->version, ui_tr("có bản", "new"), upd);
        else {
            unsigned ps = (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / (1024 * 1024 / 10)); /* 0.1 MB */
            lv_label_set_text_fmt(s_mem, "v%s · RAM %uk · PSRAM %u.%uM", esp_app_get_description()->version,
                                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024), ps / 10, ps % 10);
        }
        lv_obj_set_style_text_color(s_mem, ui_lc(upd ? UI_C_ACCENT : UI_C_DIM), 0);
    }
    for (int c = 0; c < AUDIO_MIC_CHANNELS; c++)
        lv_bar_set_value(s_bar[c], s_mic_db[c] < 20 ? 0 : (s_mic_db[c] - 20) * 100 / 70, LV_ANIM_OFF);
}

void system_wifi_setup(bool in_lvgl)
{
    wifi_mgr_start_portal();
    audio_sfx_play(SFX_BEEP_HI);
    if (!in_lvgl && !board_lvgl_lock(500)) return;
    char txt[96];
    snprintf(txt, sizeof(txt), "%s\n%s %s", wifi_mgr_ap_ssid(), ui_tr("Mật khẩu", "Password"), wifi_mgr_ap_pass());
    ui_overlay_show(LV_SYMBOL_WIFI, ui_tr("Cài đặt Wi-Fi", "Wi-Fi setup"), txt, UI_C_ACCENT, 20, NULL, NULL);
    if (!in_lvgl) board_lvgl_unlock();
}

static void portal_clicked(lv_event_t *e) { system_wifi_setup(true); }

/* GET/POST /api/wifi/portal: state of the Wi-Fi setup hotspot; POST {"on":true|false} opens/closes it.
 * Token auth (or a web session) so the PC agent can reopen it. */
static esp_err_t portal_state(httpd_req_t *req)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "on", wifi_mgr_portal_on());
    cJSON_AddStringToObject(r, "ssid", wifi_mgr_ap_ssid());
    int left = wifi_mgr_portal_left_s();
    if (left == -2) cJSON_AddStringToObject(r, "off_in", "client");
    else if (left >= 0) cJSON_AddNumberToObject(r, "off_in", left);
    else cJSON_AddNullToObject(r, "off_in");
    return web_send_json(req, r);
}

static esp_err_t h_portal_post(httpd_req_t *req)
{
    cJSON *j = web_read_json(req);
    if (!j) return ESP_OK;
    cJSON *on = cJSON_GetObjectItem(j, "on");
    bool valid = cJSON_IsBool(on), want = cJSON_IsTrue(on);
    cJSON_Delete(j);
    if (!valid) return web_send_err(req, 400, "bad_value");
    if (want) {
        if (!wifi_mgr_portal_on()) system_wifi_setup(false);
    } else if (!wifi_mgr_stop_portal()) {
        return web_send_err(req, 409, "offline"); /* the hotspot is the only way in while offline */
    }
    return portal_state(req);
}

/* Long-press the header to toggle the safe-area calibration frames */
static void toggle_frames(lv_event_t *e)
{
    if (lv_obj_has_flag(s_frames, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(s_frames, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_frames, LV_OBJ_FLAG_HIDDEN);
}

static void system_build(lv_obj_t *t)
{
    s_frames = lv_obj_create(t);
    lv_obj_remove_style_all(s_frames);
    lv_obj_set_size(s_frames, UI_W, UI_H);
    lv_obj_remove_flag(s_frames, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_frames, LV_OBJ_FLAG_HIDDEN);
    static const uint32_t col[] = {0xFF0000, 0xFF8000, 0xFFFF00, 0x00FF00, 0x00FFFF, 0x0080FF, 0xFF00FF, 0x808080};
    for (int i = 0; i < 8; i++) {
        lv_obj_t *f = lv_obj_create(s_frames);
        lv_obj_remove_style_all(f);
        lv_obj_set_pos(f, i * 4, i * 4);
        lv_obj_set_size(f, UI_W - i * 8, UI_H - i * 8);
        lv_obj_set_style_border_width(f, 1, 0);
        lv_obj_set_style_border_color(f, lv_color_hex(col[i]), 0);
        lv_obj_remove_flag(f, LV_OBJ_FLAG_CLICKABLE);
    }

    ui_header(t, ui_tr("Hệ thống", "System"), "");
    lv_obj_t *head = lv_obj_create(t);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, 160, 28);
    lv_obj_set_pos(head, 0, 0);
    lv_obj_add_flag(head, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(head, toggle_frames, LV_EVENT_LONG_PRESSED, NULL);

    lv_obj_t *col_obj = lv_obj_create(t);
    lv_obj_remove_style_all(col_obj);
    lv_obj_set_pos(col_obj, UI_SAFE_X + 8, 34);
    lv_obj_set_size(col_obj, UI_W - 2 * UI_SAFE_X - 16, 124);
    lv_obj_set_flex_flow(col_obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col_obj, 4, 0);
    lv_obj_remove_flag(col_obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    s_net = ui_label(col_obj, UI_F_SMALL, UI_C_TEXT, "");
    s_addr = ui_label(col_obj, UI_F_SMALL, UI_C_ACCENT2, "");
    s_bat = ui_label(col_obj, UI_F_SMALL, UI_C_TEXT, "");
    s_mem = ui_label(col_obj, UI_F_SMALL, UI_C_DIM, "");
    lv_obj_t *lbls[] = {s_net, s_addr, s_bat, s_mem};
    for (int i = 0; i < 4; i++) {
        lv_obj_set_width(lbls[i], lv_pct(100));
        if (i == 1) lv_label_set_long_mode(lbls[i], LV_LABEL_LONG_WRAP);
        else ui_dot(lbls[i]);
    }

    lv_obj_t *ml = ui_label(t, UI_F_SMALL, UI_C_DIM, "Mic");
    lv_obj_set_pos(ml, UI_SAFE_X + 8, 166);
    for (int c = 0; c < AUDIO_MIC_CHANNELS; c++) {
        s_bar[c] = ui_bar(t, 80, 4, UI_C_ACCENT);
        lv_obj_set_pos(s_bar[c], UI_SAFE_X + 44, 168 + c * 9);
    }
    lv_obj_t *btn = ui_button(t, LV_SYMBOL_WIFI " Wi-Fi", false, 104, 44);
    lv_obj_set_pos(btn, UI_W - UI_SAFE_X - 8 - 104, 162);
    lv_obj_add_event_cb(btn, portal_clicked, LV_EVENT_CLICKED, NULL);
}

/* ---------------- theme endpoints ---------------- */
static void add_fonts(cJSON *arr, const char *csv)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", csv ? csv : "");
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) cJSON_AddItemToArray(arr, cJSON_CreateString(tok));
}

static const theme_def_t *theme_by_id(const char *id)
{
    for (int i = 0; id && i < g_theme_count; i++)
        if (strcmp(g_themes[i].id, id) == 0) return &g_themes[i];
    return NULL;
}

/* PUT /api/config/display: validate theme / night_mode / fx, keep the legacy "night" bool in sync */
static bool display_put_hook(cJSON *b)
{
    cJSON *th = cJSON_GetObjectItem(b, "theme"), *nm = cJSON_GetObjectItem(b, "night_mode"), *fx = cJSON_GetObjectItem(b, "fx");
    if (th && (!cJSON_IsString(th) || !theme_by_id(th->valuestring))) return false;
    if (fx && (!cJSON_IsString(fx) || (strcmp(fx->valuestring, "full") && strcmp(fx->valuestring, "eco")))) return false;
    if (nm) {
        if (!cJSON_IsString(nm) || (strcmp(nm->valuestring, "auto") && strcmp(nm->valuestring, "on") && strcmp(nm->valuestring, "off")))
            return false;
        cJSON_DeleteItemFromObject(b, "night");
        cJSON_AddBoolToObject(b, "night", strcmp(nm->valuestring, "off") != 0);
    } else {
        cJSON *n = cJSON_GetObjectItem(b, "night");
        if (cJSON_IsBool(n)) cJSON_AddStringToObject(b, "night_mode", cJSON_IsTrue(n) ? "auto" : "off");
    }
    return true;
}
static esp_err_t h_themes(httpd_req_t *req)
{
    cJSON *r = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(r, "themes");
    for (int i = 0; i < g_theme_count; i++) {
        const theme_def_t *t = &g_themes[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", t->id);
        cJSON *n = cJSON_AddObjectToObject(o, "name");
        cJSON_AddStringToObject(n, "vi", t->name_vi);
        cJSON_AddStringToObject(n, "en", t->name_en);
        cJSON *d = cJSON_AddObjectToObject(o, "desc");
        cJSON_AddStringToObject(d, "vi", t->desc_vi);
        cJSON_AddStringToObject(d, "en", t->desc_en);
        cJSON_AddBoolToObject(o, "default", strcmp(t->id, g_theme_default) == 0);
        /* true when the "Effects: full/saver" setting changes something for this theme (screen and web) */
        cJSON_AddBoolToObject(o, "fx", t->fx_aberration || t->fx_scanline || t->fx_grid || t->fx_hud || t->fx_tilt ||
                                           t->fx_segments || t->btn == THEME_BTN_RAW);
        char p[48];
        snprintf(p, sizeof(p), "/themes/%s.png", t->id);
        cJSON_AddStringToObject(o, "preview", p);
        add_fonts(cJSON_AddArrayToObject(o, "fonts"), t->web_fonts);
        cJSON_AddItemToArray(arr, o);
    }
    char cur[16];
    settings_get_str("display", "theme", cur, sizeof(cur), g_theme_default);
    cJSON_AddStringToObject(r, "current", cur);
    return web_send_json(req, r);
}

static esp_err_t h_theme(httpd_req_t *req)
{
    char cur[16];
    settings_get_str("display", "theme", cur, sizeof(cur), g_theme_default);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "theme", cur);
    const theme_def_t *t = theme_by_id(cur);
    add_fonts(cJSON_AddArrayToObject(r, "fonts"), t ? t->web_fonts : "");
    return web_send_json(req, r);
}

/* ---------------- screenshot: GET /api/screenshot[?tile=0..4] -> 24-bit BMP ---------------- */
/* The snapshot is drawn by the LVGL task (lv_async_call), not on the httpd stack: rendering a whole tile there took
 * ~4-6 KB of httpd's 8 KB stack (lowest free 1.8 KB in the 0.2.2 soak test). */
static SemaphoreHandle_t s_snap_sem;
static lv_draw_buf_t *s_snap; /* handed from the LVGL task to httpd; both under the LVGL lock */
static bool s_snap_abandoned; /* httpd gave up waiting: the late snapshot frees itself */

static void snap_cb(void *arg)
{
    lv_draw_buf_t *b = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    if (s_snap_abandoned) {
        s_snap_abandoned = false;
        if (b) lv_draw_buf_destroy(b);
        return;
    }
    s_snap = b;
    xSemaphoreGive(s_snap_sem);
}

static esp_err_t h_shot(httpd_req_t *req)
{
    char q[8];
    if (web_query(req, "tile", q, sizeof(q))) {
        int t = atoi(q);
        if (t >= 0 && t < UI_TILE_COUNT && board_lvgl_lock(500)) {
            ui_show_tile((ui_tile_t)t, false);
            board_lvgl_unlock();
            vTaskDelay(pdMS_TO_TICKS(700));
        }
    }
    if (!s_snap_sem) s_snap_sem = xSemaphoreCreateBinary();
    if (!s_snap_sem || !board_lvgl_lock(1000)) return web_send_err(req, 503, "busy");
    xSemaphoreTake(s_snap_sem, 0); /* drop a give from a snapshot that came after an earlier timeout */
    if (s_snap) lv_draw_buf_destroy(s_snap);
    s_snap = NULL;
    s_snap_abandoned = false;
    lv_async_call(snap_cb, NULL);
    board_lvgl_unlock();
    if (xSemaphoreTake(s_snap_sem, pdMS_TO_TICKS(3000)) != pdTRUE) {
        if (board_lvgl_lock(1000)) {
            if (s_snap) lv_draw_buf_destroy(s_snap); /* it may have landed just now */
            else s_snap_abandoned = true;
            s_snap = NULL;
            board_lvgl_unlock();
        }
        return web_send_err(req, 503, "busy");
    }
    if (!board_lvgl_lock(1000)) return web_send_err(req, 503, "busy");
    lv_draw_buf_t *snap = s_snap;
    s_snap = NULL;
    board_lvgl_unlock();
    if (!snap) return web_send_err(req, 503, "busy");
    const int w = snap->header.w, h = snap->header.h, row = w * 3;
    uint8_t *line = malloc(row);
    if (!line) {
        lv_draw_buf_destroy(snap);
        return web_send_err(req, 503, "busy");
    }
    uint8_t hdr[54] = {'B', 'M'};
    uint32_t size = 54 + row * h;
    memcpy(hdr + 2, &size, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    int32_t nh = -h;
    memcpy(hdr + 22, &nh, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send_chunk(req, (const char *)hdr, 54);
    for (int y = 0; y < h; y++) {
        const uint16_t *src = (const uint16_t *)(snap->data + y * snap->header.stride);
        for (int x = 0; x < w; x++) {
            uint16_t c = src[x];
            line[x * 3 + 0] = (c & 0x1F) * 255 / 31;
            line[x * 3 + 1] = ((c >> 5) & 0x3F) * 255 / 63;
            line[x * 3 + 2] = ((c >> 11) & 0x1F) * 255 / 31;
        }
        httpd_resp_send_chunk(req, (const char *)line, row);
    }
    free(line);
    lv_draw_buf_destroy(snap);
    return httpd_resp_send_chunk(req, NULL, 0);
}

void system_app_init(void)
{
    ui_register_builder(UI_TILE_SYSTEM, system_build);
    web_register("/api/screenshot", HTTP_GET, h_shot, WEB_AUTH_SESSION);
    web_register("/api/wifi/portal", HTTP_GET, portal_state, WEB_AUTH_TOKEN);
    web_register("/api/wifi/portal", HTTP_POST, h_portal_post, WEB_AUTH_TOKEN);
    web_register("/api/themes", HTTP_GET, h_themes, WEB_AUTH_SESSION);
    web_register("/api/theme", HTTP_GET, h_theme, WEB_AUTH_NONE);
    web_set_put_hook("display", display_put_hook);
    lv_timer_create(sys_tick, 500, NULL);
    xTaskCreatePinnedToCore(mic_task, "mic", 4096, NULL, 4, NULL, 0);
}
