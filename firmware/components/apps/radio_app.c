/* Radio tile + /api/radio/play + /api/sleep. Stations live in settings group "radio" (see web/API.md). */
#include "apps.h"

#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "ui.h"
#include "net.h"
#include "settings.h"
#include "player.h"
#include "audio_hw.h"
#include "audio_sfx.h"

static const char *TAG = "radio";

static lv_obj_t *s_name, *s_title, *s_state, *s_bar, *s_play_lbl, *s_vol, *s_list;
static volatile int64_t s_sleep_until; /* us, 0 = off */
static char s_relay[64];               /* "ip:port" learned from the PC agent */
static int s_saved_vol = -1;

/* ---------------- stations ---------------- */
static void url_encode(const char *in, char *out, size_t len)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < len; in++) {
        unsigned char c = (unsigned char)*in;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out[o++] = c;
        else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
    }
    out[o] = 0;
}

void radio_set_relay(const char *host_port) { snprintf(s_relay, sizeof(s_relay), "%s", host_port ? host_port : ""); }

/* Build the URL the player should open. Returns false if a YouTube station has no relay. */
static bool station_url(const cJSON *st, char *out, size_t len)
{
    const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(st, "url"));
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(st, "type"));
    if (!url) return false;
    if (type && strcmp(type, "youtube") == 0) {
        char host[64], hp[80];
        settings_get_str("pc", "host", host, sizeof(host), "");
        bool aut = settings_get_bool("pc", "auto", true);
        if (aut && s_relay[0]) snprintf(hp, sizeof(hp), "%s", s_relay);
        else if (host[0]) snprintf(hp, sizeof(hp), "%s:%d", host, settings_get_int("pc", "port", 8765));
        else if (s_relay[0]) snprintf(hp, sizeof(hp), "%s", s_relay);
        else return false;
        char enc[400];
        url_encode(url, enc, sizeof(enc));
        snprintf(out, len, "http://%s/yt/stream?url=%s", hp, enc);
        return true;
    }
    snprintf(out, len, "%s", url);
    return true;
}

static cJSON *find_station(cJSON *cfg, int id, int *index)
{
    cJSON *it;
    int i = 0;
    cJSON_ArrayForEach(it, cJSON_GetObjectItem(cfg, "st")) {
        cJSON *jid = cJSON_GetObjectItem(it, "id");
        if (cJSON_IsNumber(jid) && jid->valueint == id) {
            if (index) *index = i;
            return it;
        }
        i++;
    }
    return NULL;
}

/* returns 0 ok, -1 not found, -2 needs relay */
int radio_play_id(int id)
{
    cJSON *cfg = settings_get("radio");
    if (id <= 0) id = cJSON_GetObjectItem(cfg, "last") ? cJSON_GetObjectItem(cfg, "last")->valueint : 0;
    cJSON *st = find_station(cfg, id, NULL);
    if (!st) st = cJSON_GetArrayItem(cJSON_GetObjectItem(cfg, "st"), 0);
    int rc = -1;
    if (st) {
        char url[600];
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(st, "name"));
        int sid = cJSON_GetObjectItem(st, "id")->valueint;
        if (station_url(st, url, sizeof(url))) {
            player_play(url, name, sid);
            cJSON *p = cJSON_CreateObject();
            cJSON_AddNumberToObject(p, "last", sid);
            settings_update("radio", p);
            cJSON_Delete(p);
            rc = 0;
        } else {
            rc = -2;
        }
    }
    cJSON_Delete(cfg);
    return rc;
}

static void radio_step(int dir)
{
    cJSON *cfg = settings_get("radio");
    cJSON *arr = cJSON_GetObjectItem(cfg, "st");
    int n = cJSON_GetArraySize(arr), idx = 0;
    int cur = player_id() ? player_id() : (cJSON_GetObjectItem(cfg, "last") ? cJSON_GetObjectItem(cfg, "last")->valueint : 0);
    find_station(cfg, cur, &idx);
    int id = 0;
    if (n) id = cJSON_GetObjectItem(cJSON_GetArrayItem(arr, (idx + dir + n) % n), "id")->valueint;
    cJSON_Delete(cfg);
    if (id) radio_play_id(id);
}

void radio_toggle(void)
{
    if (player_state() == PLAYER_STOP || player_state() == PLAYER_ERR) radio_play_id(0);
    else player_stop();
}

bool radio_playing(void) { return player_state() == PLAYER_PLAY || player_state() == PLAYER_BUF; }

/* ---------------- volume persistence ---------------- */
static void save_volume_if_changed(void)
{
    int v = audio_hw_get_volume();
    if (v == s_saved_vol) return;
    s_saved_vol = v;
    cJSON *p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "vol", v);
    settings_update("radio", p);
    cJSON_Delete(p);
}

/* ---------------- UI ---------------- */
static void show_need_relay(void)
{
    ui_overlay_show(LV_SYMBOL_WARNING, "YouTube",
                    ui_tr("Cần bật MeowBox PC agent trên máy tính để phát YouTube.",
                          "Start the MeowBox PC agent to play YouTube."),
                    UI_C_WARN, 6, NULL, NULL);
}

static void btn_cb(lv_event_t *e)
{
    int a = (int)(intptr_t)lv_event_get_user_data(e);
    if (a == 0) {
        if (!radio_playing() && radio_play_id(0) == -2) show_need_relay();
        else if (radio_playing()) player_stop();
    } else {
        radio_step(a);
    }
}

static void list_close(void)
{
    if (s_list) {
        lv_obj_delete(s_list);
        s_list = NULL;
    }
}

static void list_pick(lv_event_t *e)
{
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    list_close();
    if (radio_play_id(id) == -2) show_need_relay();
}

static void list_bg(lv_event_t *e)
{
    if (lv_event_get_target(e) == lv_event_get_current_target(e)) list_close();
}

static void open_list(lv_event_t *e)
{
    list_close();
    s_list = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, UI_W, UI_H);
    lv_obj_set_style_bg_color(s_list, ui_lc(UI_C_BG), 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_80, 0);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_list, list_bg, LV_EVENT_CLICKED, NULL);

    lv_obj_t *box = lv_obj_create(s_list);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, UI_W - 2 * UI_SAFE_X - 16, UI_H - 2 * UI_SAFE_Y - 16);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, ui_lc(UI_C_SURFACE), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, ui_lc(UI_C_LINE), 0);
    lv_obj_set_style_border_width(box, ui_theme()->border ? ui_theme()->border : 1, 0);
    lv_obj_set_style_radius(box, ui_theme()->btn == THEME_BTN_TEXT ? 12 : 0, 0);
    lv_obj_set_style_pad_all(box, 6, 0);
    lv_obj_set_style_pad_row(box, 0, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    cJSON *cfg = settings_get("radio");
    cJSON *it;
    int n = 1;
    cJSON_ArrayForEach(it, cJSON_GetObjectItem(cfg, "st")) {
        int id = cJSON_GetObjectItem(it, "id")->valueint;
        const char *nm = cJSON_GetStringValue(cJSON_GetObjectItem(it, "name"));
        bool cur = id == player_id();
        lv_obj_t *b = lv_obj_create(box);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, lv_pct(100), 40);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_border_side(b, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, ui_lc(UI_C_LINE), 0);
        lv_obj_set_style_bg_color(b, ui_lc(UI_C_TEXT), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(b, LV_OPA_20, LV_STATE_PRESSED);
        char num[12];
        snprintf(num, sizeof(num), "%d", n++);
        lv_obj_t *l = ui_label(b, UI_F_SMALL, cur ? UI_C_ACCENT : UI_C_DIM, num);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 4, 0);
        l = ui_label(b, cur ? UI_F_TITLE : UI_F_SMALL, cur ? UI_C_ACCENT : UI_C_TEXT, nm ? nm : "?");
        ui_dot(l);
        lv_obj_set_width(l, UI_W - 2 * UI_SAFE_X - 70);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 28, 0);
        lv_obj_add_event_cb(b, list_pick, LV_EVENT_CLICKED, (void *)(intptr_t)id);
    }
    cJSON_Delete(cfg);
}

static void radio_tick(lv_timer_t *t)
{
    if (s_sleep_until && esp_timer_get_time() >= s_sleep_until) {
        s_sleep_until = 0;
        player_stop();
        ESP_LOGI(TAG, "sleep timer: stop");
    }
    static int n;
    if (++n % 20 == 0) save_volume_if_changed();
    /* Wi-Fi power save off while a network stream plays (4.1) */
    static bool awake;
    player_state_t ps = player_state();
    bool streaming = (ps == PLAYER_BUF || ps == PLAYER_PLAY) && !player_is_mem();
    if (streaming != awake) {
        awake = streaming;
        wifi_mgr_hold_awake(WIFI_HOLD_RADIO, streaming);
    }
    if (ui_active_tile() != UI_TILE_RADIO || !s_name) return;

    char name[64], title[128];
    player_name(name, sizeof(name));
    player_title(title, sizeof(title));
    player_state_t st = player_state();
    if (st == PLAYER_STOP && !name[0]) {
        cJSON *cfg = settings_get("radio");
        cJSON *s = find_station(cfg, cJSON_GetObjectItem(cfg, "last") ? cJSON_GetObjectItem(cfg, "last")->valueint : 0, NULL);
        snprintf(name, sizeof(name), "%s", s ? cJSON_GetStringValue(cJSON_GetObjectItem(s, "name")) : "Radio");
        cJSON_Delete(cfg);
    }
    char nm[80];
    snprintf(nm, sizeof(nm), "%s " LV_SYMBOL_DOWN, name);
    if (strcmp(lv_label_get_text(s_name), nm)) lv_label_set_text(s_name, nm);
    const char *tt = title[0] ? title : st == PLAYER_STOP ? ui_tr("Bấm " LV_SYMBOL_PLAY " để nghe", "Press " LV_SYMBOL_PLAY " to listen") : "";
    if (strcmp(lv_label_get_text(s_title), tt)) lv_label_set_text(s_title, tt);
    const char *sts = st == PLAYER_PLAY ? ui_tr("Đang phát", "Playing")
                    : st == PLAYER_BUF  ? ui_tr("Đang tải", "Buffering")
                    : st == PLAYER_ERR  ? ui_tr("Lỗi kết nối", "Connection error")
                                        : ui_tr("Đã dừng", "Stopped");
    char line[80], stb[40];
    player_net_t net;
    if (st == PLAYER_BUF && player_net(&net)) { /* buffering progress instead of a frozen "Buffering" */
        snprintf(stb, sizeof(stb), "%s %u%%", sts, net.fill_pct);
        sts = stb;
    }
    if (s_sleep_until) {
        int left = (int)((s_sleep_until - esp_timer_get_time()) / 60000000) + 1;
        snprintf(line, sizeof(line), "%s, %s %d %s", sts, ui_tr("tắt sau", "off in"), left, ui_tr("phút", "min"));
    } else snprintf(line, sizeof(line), "%s", sts);
    if (strcmp(lv_label_get_text(s_state), line)) lv_label_set_text(s_state, line);
    lv_obj_set_style_text_color(s_state, ui_lc(st == PLAYER_ERR ? UI_C_ALERT : st == PLAYER_PLAY ? UI_C_OK : UI_C_DIM), 0);
    lv_bar_set_value(s_bar, st == PLAYER_STOP ? 0 : player_buffer_pct(), LV_ANIM_OFF);
    lv_label_set_text(s_play_lbl, radio_playing() ? LV_SYMBOL_STOP : LV_SYMBOL_PLAY);
    char v[24];
    snprintf(v, sizeof(v), "%s %d", ui_tr("Âm lượng", "Volume"), audio_hw_get_volume());
    lv_label_set_text(s_vol, v);
}

static void radio_build(lv_obj_t *t)
{
    s_list = NULL;
    s_vol = ui_header(t, "Radio", "");

    /* station name: looks tappable (opens the list) */
    lv_obj_t *nb = lv_obj_create(t);
    lv_obj_remove_style_all(nb);
    lv_obj_set_pos(nb, UI_SAFE_X + 4, 32);
    lv_obj_set_size(nb, UI_W - 2 * UI_SAFE_X - 8, 40);
    lv_obj_add_flag(nb, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(nb, ui_lc(UI_C_TEXT), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(nb, LV_OPA_10, LV_STATE_PRESSED);
    if (ui_theme()->btn == THEME_BTN_RAW) {
        lv_obj_set_style_bg_color(nb, ui_lc(UI_C_ACCENT2), 0);
        lv_obj_set_style_bg_opa(nb, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(nb, 3, 0);
        lv_obj_set_style_border_color(nb, ui_lc(UI_C_LINE), 0);
    }
    lv_obj_add_event_cb(nb, open_list, LV_EVENT_CLICKED, NULL);
    s_name = ui_label(nb, UI_F_BODY, ui_theme()->btn == THEME_BTN_RAW ? UI_C_BG : UI_C_TEXT, "Radio");
    lv_obj_set_width(s_name, UI_W - 2 * UI_SAFE_X - 20);
    ui_dot(s_name);
    lv_obj_align(s_name, LV_ALIGN_LEFT_MID, 4, 0);

    s_title = ui_label(t, UI_F_SMALL, UI_C_TEXT, "");
    lv_obj_set_width(s_title, UI_W - 2 * UI_SAFE_X - 16);
    ui_scroll(s_title);
    lv_obj_set_pos(s_title, UI_SAFE_X + 8, 78);

    s_bar = ui_bar(t, UI_W - 2 * UI_SAFE_X - 16, 3, UI_C_ACCENT);
    lv_obj_set_pos(s_bar, UI_SAFE_X + 8, 108);
    s_state = ui_label(t, UI_F_SMALL, UI_C_DIM, "");
    lv_obj_set_pos(s_state, UI_SAFE_X + 8, 116);

    lv_obj_t *row = lv_obj_create(t);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 210, 60);
    lv_obj_set_pos(row, UI_SAFE_X + 6, 146);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *b = ui_button(row, LV_SYMBOL_PLAY, true, 72, 52);
    s_play_lbl = ui_button_label(b);
    lv_obj_add_event_cb(b, btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)0);
    b = ui_button(row, LV_SYMBOL_PREV, false, 52, 52);
    lv_obj_add_event_cb(b, btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    b = ui_button(row, LV_SYMBOL_NEXT, false, 52, 52);
    lv_obj_add_event_cb(b, btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
}

/* ---------------- web API ---------------- */
static void add_play(cJSON *o)
{
    char name[64], title[128];
    player_name(name, sizeof(name));
    player_title(title, sizeof(title));
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "st", player_state_str());
    cJSON_AddNumberToObject(p, "id", player_id());
    cJSON_AddStringToObject(p, "name", name);
    cJSON_AddStringToObject(p, "title", title);
    player_net_t n;
    if (player_net(&n)) { /* stream numbers (docs/firmware_v0.2.0.md, 4.3; web/API.md) */
        cJSON *j = cJSON_AddObjectToObject(p, "net");
        cJSON_AddNumberToObject(j, "kbps", n.kbps);
        cJSON_AddNumberToObject(j, "br", n.br);
        cJSON_AddNumberToObject(j, "buf", n.buf_ds / 10.0);
        cJSON_AddNumberToObject(j, "pre", n.pre_s);
        cJSON_AddNumberToObject(j, "fill", n.fill_pct);
        cJSON_AddNumberToObject(j, "under", n.under);
        cJSON_AddNumberToObject(j, "reconn", n.reconn);
        cJSON_AddStringToObject(j, "fmt", n.fmt);
        cJSON_AddStringToObject(j, "err", n.err);
    }
    if (cJSON_GetObjectItem(o, "play")) cJSON_ReplaceItemInObject(o, "play", p);
    else cJSON_AddItemToObject(o, "play", p);
}

void radio_status_hook(cJSON *r)
{
    add_play(r);
    int left = s_sleep_until ? (int)((s_sleep_until - esp_timer_get_time()) / 1000000) : 0;
    cJSON_ReplaceItemInObject(r, "sleep", cJSON_CreateNumber(left > 0 ? left : 0));
}

static esp_err_t h_play(httpd_req_t *req)
{
    cJSON *j = web_read_json(req);
    if (!j) return ESP_OK;
    const char *cmd = cJSON_GetStringValue(cJSON_GetObjectItem(j, "cmd"));
    esp_err_t res = ESP_OK;
    int rc = 0;
    if (!cmd) rc = -3;
    else if (!strcmp(cmd, "play")) {
        cJSON *id = cJSON_GetObjectItem(j, "id");
        const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(j, "url"));
        if (url) {
            const char *nm = cJSON_GetStringValue(cJSON_GetObjectItem(j, "name"));
            cJSON *tmp = cJSON_CreateObject();
            cJSON_AddStringToObject(tmp, "url", url);
            cJSON_AddStringToObject(tmp, "type", strstr(url, "youtu") ? "youtube" : "stream");
            char u[600];
            if (station_url(tmp, u, sizeof(u))) player_play(u, nm ? nm : url, 0);
            else rc = -2;
            cJSON_Delete(tmp);
        } else {
            rc = radio_play_id(cJSON_IsNumber(id) ? id->valueint : 0);
        }
    } else if (!strcmp(cmd, "stop")) player_stop();
    else if (!strcmp(cmd, "next")) radio_step(1);
    else if (!strcmp(cmd, "prev")) radio_step(-1);
    else if (!strcmp(cmd, "vol")) {
        cJSON *v = cJSON_GetObjectItem(j, "vol");
        if (cJSON_IsNumber(v)) audio_hw_set_volume(v->valueint);
        else rc = -3;
    } else rc = -3;
    cJSON_Delete(j);
    if (rc == -1) return web_send_err(req, 404, "not_found");
    if (rc == -2) return web_send_err(req, 503, "busy");
    if (rc == -3) return web_send_err(req, 400, "bad_value");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddNumberToObject(r, "vol", audio_hw_get_volume());
    add_play(r);
    res = web_send_json(req, r);
    return res;
}

static esp_err_t send_sleep(httpd_req_t *req)
{
    int left = s_sleep_until ? (int)((s_sleep_until - esp_timer_get_time()) / 1000000) : 0;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "left", left > 0 ? left : 0);
    return web_send_json(req, r);
}

static esp_err_t h_sleep_get(httpd_req_t *req) { return send_sleep(req); }

static esp_err_t h_sleep_post(httpd_req_t *req)
{
    cJSON *j = web_read_json(req);
    if (!j) return ESP_OK;
    cJSON *m = cJSON_GetObjectItem(j, "min");
    int min = cJSON_IsNumber(m) ? m->valueint : -1;
    cJSON_Delete(j);
    if (min < 0 || min > 240) return web_send_err(req, 400, "bad_value");
    s_sleep_until = min ? esp_timer_get_time() + (int64_t)min * 60000000 : 0;
    return send_sleep(req);
}

static void sfx_hook(bool start) { player_hold(start); }

static void radio_cfg_hook(cJSON *doc)
{
    cJSON_ReplaceItemInObject(doc, "vol", cJSON_CreateNumber(audio_hw_get_volume()));
}

static void on_settings(const char *group, void *arg)
{
    /* the web UI's radio "vol" field changes the live volume */
    if (strcmp(group, "radio") == 0) {
        int v = settings_get_int("radio", "vol", -1);
        if (v >= 0 && v != s_saved_vol) {
            s_saved_vol = v;
            audio_hw_set_volume(v);
        }
    }
}

void radio_app_init(void)
{
    player_init();
    audio_sfx_set_hook(sfx_hook);
    s_saved_vol = settings_get_int("radio", "vol", 40);
    audio_hw_set_volume(s_saved_vol);

    ui_register_builder(UI_TILE_RADIO, radio_build);
    lv_timer_create(radio_tick, 500, NULL);
    settings_on_change(on_settings, NULL);
    web_set_config_hook("radio", radio_cfg_hook);
    web_register("/api/radio/play", HTTP_POST, h_play, WEB_AUTH_SESSION);
    web_register("/api/sleep", HTTP_GET, h_sleep_get, WEB_AUTH_SESSION);
    web_register("/api/sleep", HTTP_POST, h_sleep_post, WEB_AUTH_SESSION);
}
