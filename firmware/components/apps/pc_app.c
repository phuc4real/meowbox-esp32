/* PC screen (P5): WebSocket /ws/pc from pc-agent (stats, media, hello, notify) + media control back to the PC. */
#include "apps.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "ui.h"
#include "net.h"
#include "esp_http_client.h"
#include "utf8.h"

static const char *TAG = "pc";

typedef struct {
    int cpu, ram, gpu, cpu_t, gpu_t, disk;
    double up, down;
    char app[24], title[96], artist[64], state[10];
    int pos, len;
} pc_data_t;

static pc_data_t s_d;
static SemaphoreHandle_t s_lock;
static volatile int s_fd = -1;
static volatile int64_t s_last_us;
static char s_peer[16];
static char s_agent_name[32], s_agent_ver[16]; /* from hello */
static int s_agent_proto = -1;
static volatile bool s_dirty;

static lv_obj_t *s_arc_val[4], *s_net, *s_status, *s_app, *s_title, *s_artist, *s_play_lbl, *s_offline, *s_body;

bool pc_online(void) { return s_fd >= 0 && esp_timer_get_time() - s_last_us < 6000000; }

/* ---------------- websocket ---------------- */
static int jint(cJSON *o, const char *k, int def)
{
    cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? v->valueint : def;
}

static void handle_msg(cJSON *j)
{
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(j, "t"));
    if (!t) return;
    if (!strcmp(t, "stats")) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_d.cpu = jint(j, "cpu", -1);
        s_d.ram = jint(j, "ram", -1);
        s_d.gpu = jint(j, "gpu", -1);
        s_d.cpu_t = jint(j, "cpu_temp", -1);
        s_d.gpu_t = jint(j, "gpu_temp", -1);
        s_d.disk = jint(j, "disk", -1);
        cJSON *u = cJSON_GetObjectItem(j, "net_up"), *d = cJSON_GetObjectItem(j, "net_down");
        s_d.up = cJSON_IsNumber(u) ? u->valuedouble : 0;
        s_d.down = cJSON_IsNumber(d) ? d->valuedouble : 0;
        xSemaphoreGive(s_lock);
    } else if (!strcmp(t, "media")) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        utf8_copy(s_d.app, sizeof(s_d.app), cJSON_GetStringValue(cJSON_GetObjectItem(j, "app")));
        utf8_copy(s_d.title, sizeof(s_d.title), cJSON_GetStringValue(cJSON_GetObjectItem(j, "title")));
        utf8_copy(s_d.artist, sizeof(s_d.artist), cJSON_GetStringValue(cJSON_GetObjectItem(j, "artist")));
        snprintf(s_d.state, sizeof(s_d.state), "%s", cJSON_GetStringValue(cJSON_GetObjectItem(j, "state")) ?: "stopped");
        s_d.pos = jint(j, "pos", 0);
        s_d.len = jint(j, "len", 0);
        xSemaphoreGive(s_lock);
    } else if (!strcmp(t, "hello")) {
        const char *nm = cJSON_GetStringValue(cJSON_GetObjectItem(j, "name"));
        const char *vr = cJSON_GetStringValue(cJSON_GetObjectItem(j, "ver"));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        snprintf(s_agent_name, sizeof(s_agent_name), "%s", nm ? nm : "");
        snprintf(s_agent_ver, sizeof(s_agent_ver), "%s", vr ? vr : "");
        s_agent_proto = jint(j, "proto", 0);
        xSemaphoreGive(s_lock);
        const char *relay = cJSON_GetStringValue(cJSON_GetObjectItem(j, "relay"));
        if (relay) {
            const char *hp = strstr(relay, "://");
            radio_set_relay(hp ? hp + 3 : relay);
            ESP_LOGI(TAG, "agent relay %s", relay);
        }
    } else if (!strcmp(t, "notify")) {
        cJSON *d = cJSON_GetObjectItem(j, "duration");
        notify_post(cJSON_GetStringValue(cJSON_GetObjectItem(j, "title")), cJSON_GetStringValue(cJSON_GetObjectItem(j, "text")),
                    cJSON_GetStringValue(cJSON_GetObjectItem(j, "sound")), cJSON_GetStringValue(cJSON_GetObjectItem(j, "priority")),
                    cJSON_IsNumber(d) ? d->valueint : 0, "pc");
    }
    s_dirty = true;
}

static void learn_peer(int fd)
{
    struct sockaddr_in6 addr;
    socklen_t al = sizeof(addr);
    if (getpeername(fd, (struct sockaddr *)&addr, &al) == 0) {
        if (addr.sin6_family == AF_INET) inet_ntop(AF_INET, &((struct sockaddr_in *)&addr)->sin_addr, s_peer, sizeof(s_peer));
        else inet_ntop(AF_INET, &addr.sin6_addr.un.u32_addr[3], s_peer, sizeof(s_peer));
    }
    ESP_LOGI(TAG, "agent connected from %s", s_peer);
}

static void send_to(int fd, const char *msg)
{
    httpd_ws_frame_t f = {.type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)msg, .len = strlen(msg), .final = true};
    httpd_ws_send_frame_async(web_server(), fd, &f);
}

/* One PC at a time (docs/firmware_v0.2.0.md, 2.4): while one is online, another one's hello gets
 * {"t":"busy","pc":"<name>"} and is closed, unless it says "take":true ("Take over" in the tray); then the old one
 * gets the busy message instead. An agent with a protocol older than MB_PROTO_MIN gets {"t":"proto","need":N}. */
static bool admit(int fd, cJSON *j)
{
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(j, "t"));
    bool hello = t && !strcmp(t, "hello");
    char msg[96];
    if (hello && jint(j, "proto", 0) < MB_PROTO_MIN) {
        snprintf(msg, sizeof(msg), "{\"t\":\"proto\",\"need\":%d,\"have\":%d}", MB_PROTO_MIN, MB_PROTO);
        send_to(fd, msg);
        httpd_sess_trigger_close(web_server(), fd);
        return false;
    }
    if (fd == s_fd) return true;
    if (pc_online()) {
        cJSON *take = cJSON_GetObjectItem(j, "take");
        const char *who = cJSON_GetStringValue(cJSON_GetObjectItem(j, "name"));
        bool same_pc = hello && who && who[0] && !strcmp(who, s_agent_name); /* reconnect before the old link timed out */
        if (!hello || (!cJSON_IsTrue(take) && !same_pc)) {
            if (hello) {
                const char *nm = cJSON_GetStringValue(cJSON_GetObjectItem(j, "name"));
                snprintf(msg, sizeof(msg), "{\"t\":\"busy\",\"pc\":\"%.40s\"}", s_agent_name[0] ? s_agent_name : s_peer);
                send_to(fd, msg);
                httpd_sess_trigger_close(web_server(), fd);
                ESP_LOGI(TAG, "%s refused: busy with %s", nm ? nm : "agent", s_agent_name);
            }
            return false;
        }
        const char *nm = cJSON_GetStringValue(cJSON_GetObjectItem(j, "name"));
        snprintf(msg, sizeof(msg), "{\"t\":\"busy\",\"pc\":\"%.40s\"}", nm ? nm : "?");
        send_to(s_fd, msg);
        httpd_sess_trigger_close(web_server(), s_fd);
        ESP_LOGI(TAG, "%s takes over from %s", nm ? nm : "agent", s_agent_name);
    }
    s_fd = fd;
    learn_peer(fd);
    return true;
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (req->method == HTTP_GET) { /* handshake: the connection is admitted on its first message (hello) */
        if (!pc_online()) {
            s_fd = fd;
            s_last_us = esp_timer_get_time();
            learn_peer(fd);
        }
        s_dirty = true;
        return ESP_OK;
    }
    httpd_ws_frame_t f = {0};
    if (httpd_ws_recv_frame(req, &f, 0) != ESP_OK) return ESP_FAIL;
    if (f.type == HTTPD_WS_TYPE_CLOSE) {
        if (s_fd == fd) s_fd = -1;
        return ESP_OK;
    }
    if (f.len == 0 || f.len > 4096) return ESP_OK;
    char *buf = malloc(f.len + 1);
    f.payload = (uint8_t *)buf;
    if (httpd_ws_recv_frame(req, &f, f.len) == ESP_OK) {
        buf[f.len] = 0;
        cJSON *j = cJSON_Parse(buf);
        if (j && admit(fd, j)) {
            s_last_us = esp_timer_get_time();
            handle_msg(j);
        }
        cJSON_Delete(j);
    }
    free(buf);
    return ESP_OK;
}

static void send_cmd(const char *cmd)
{
    if (!pc_online()) return;
    char msg[64];
    snprintf(msg, sizeof(msg), "{\"t\":\"media_cmd\",\"cmd\":\"%s\"}", cmd);
    send_to(s_fd, msg);
}

/* ---------------- UI ---------------- */
static lv_obj_t *s_bar[4];

static void btn_cb(lv_event_t *e) { send_cmd((const char *)lv_event_get_user_data(e)); }

static void fmt_rate(double kbps, char *out, size_t len)
{
    if (kbps >= 1024) snprintf(out, len, "%.1f MB/s", kbps / 1024);
    else snprintf(out, len, "%d KB/s", (int)kbps);
}

static void pc_tick(lv_timer_t *t)
{
    bool on = pc_online();
    if (s_fd >= 0 && !on && esp_timer_get_time() - s_last_us > 15000000) s_fd = -1;
    ui_tile_set_skip(UI_TILE_PC, !on); /* no computer: swiping goes from Radio straight to the cat */
    if (!s_body) return;
    /* compare with what the tile shows, not a remembered state: the tile is rebuilt on theme / night changes and
     * the agent may connect while another tile is on screen */
    if (on == lv_obj_has_flag(s_body, LV_OBJ_FLAG_HIDDEN)) {
        s_dirty = true;
        if (on) {
            lv_obj_add_flag(s_offline, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_body, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_offline, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_body, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (ui_active_tile() != UI_TILE_PC) return;
    lv_label_set_text(s_status, on ? s_peer : "");
    if (!on || !s_dirty) return;
    s_dirty = false;

    pc_data_t d;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    d = s_d;
    xSemaphoreGive(s_lock);
    int vals[4] = {d.cpu, d.ram, d.gpu, d.cpu_t >= 0 ? d.cpu_t : d.gpu_t};
    for (int i = 0; i < 4; i++) {
        bool hot = vals[i] >= 85;
        if (vals[i] < 0) lv_label_set_text(s_arc_val[i], "-");
        else lv_label_set_text_fmt(s_arc_val[i], i == 3 ? "%d°" : "%d", vals[i]);
        lv_obj_set_style_text_color(s_arc_val[i], ui_lc(hot ? UI_C_ALERT : UI_C_TEXT), 0);
        lv_bar_set_value(s_bar[i], vals[i] < 0 ? 0 : vals[i] > 100 ? 100 : vals[i], LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_bar[i], ui_lc(hot ? UI_C_ALERT : UI_C_ACCENT), LV_PART_INDICATOR);
    }
    char up[16], dn[16];
    fmt_rate(d.up, up, sizeof(up));
    fmt_rate(d.down, dn, sizeof(dn));
    lv_label_set_text_fmt(s_net, "%s %s   %s %s", LV_SYMBOL_UP, up, LV_SYMBOL_DOWN, dn);
    bool has_media = d.title[0] != 0;
    lv_label_set_text(s_app, has_media ? d.app : ui_tr("Không có nhạc", "Nothing playing"));
    ui_set_text_clean(s_title, d.title);
    ui_set_text_clean(s_artist, d.artist);
    lv_label_set_text(s_play_lbl, !strcmp(d.state, "playing") ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

static void pc_build(lv_obj_t *t)
{
    s_status = ui_header(t, ui_tr("Máy tính", "Computer"), "");

    s_offline = lv_obj_create(t);
    lv_obj_remove_style_all(s_offline);
    lv_obj_set_size(s_offline, UI_W - 2 * UI_SAFE_X - 16, 150);
    lv_obj_set_pos(s_offline, UI_SAFE_X + 8, 40);
    lv_obj_set_flex_flow(s_offline, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_offline, 6, 0);
    lv_obj_remove_flag(s_offline, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = ui_label(s_offline, UI_F_TITLE, UI_C_TEXT, ui_tr("Máy tính chưa kết nối", "Computer not connected"));
    l = ui_label(s_offline, UI_F_SMALL, UI_C_DIM,
                 ui_tr("Mở MeowBox trên máy tính để xem CPU, RAM và điều khiển nhạc từ loa.",
                       "Open MeowBox on your computer to see CPU, RAM and control music from here."));
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);

    s_body = lv_obj_create(t);
    lv_obj_remove_style_all(s_body);
    lv_obj_set_size(s_body, UI_W, UI_H - 20);
    lv_obj_remove_flag(s_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_body, LV_OBJ_FLAG_HIDDEN);

    static const char *names[4] = {"CPU %", "RAM %", "GPU %", "°C"};
    const int colw = (UI_W - 2 * UI_SAFE_X - 16 - 3 * 8) / 4;
    for (int i = 0; i < 4; i++) {
        int x = UI_SAFE_X + 8 + i * (colw + 8);
        lv_obj_t *n = ui_label(s_body, UI_F_SMALL, UI_C_DIM, i == 3 ? ui_tr("Nhiệt độ", "Temp") : names[i]);
        lv_obj_set_pos(n, x, 32);
        s_arc_val[i] = ui_label(s_body, UI_F_TITLE, UI_C_TEXT, "-");
        lv_obj_set_pos(s_arc_val[i], x, 50);
        s_bar[i] = ui_bar(s_body, colw, 4, UI_C_ACCENT);
        lv_obj_set_pos(s_bar[i], x, 78);
    }
    s_net = ui_label(s_body, UI_F_SMALL, UI_C_DIM, "");
    lv_obj_set_pos(s_net, UI_SAFE_X + 8, 88);
    ui_rule(s_body, UI_SAFE_X + 8, 112, UI_W - 2 * UI_SAFE_X - 16);
    s_app = ui_label(s_body, UI_F_SMALL, UI_C_DIM, "");
    lv_obj_set_pos(s_app, UI_SAFE_X + 8, 118);
    /* title on its own full-width line; artist and the buttons below it */
    s_title = ui_label(s_body, UI_F_TITLE, UI_C_TEXT, "");
    lv_obj_set_width(s_title, UI_W - 2 * UI_SAFE_X - 16);
    ui_scroll(s_title);
    lv_obj_set_pos(s_title, UI_SAFE_X + 8, 134);
    s_artist = ui_label(s_body, UI_F_SMALL, UI_C_ACCENT2, "");
    lv_obj_set_width(s_artist, UI_W - 2 * UI_SAFE_X - 16 - 124 - 8);
    ui_dot(s_artist);
    lv_obj_set_pos(s_artist, UI_SAFE_X + 8, 172);
    lv_obj_t *row = lv_obj_create(s_body);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 124, 44);
    lv_obj_align(row, LV_ALIGN_TOP_RIGHT, -UI_SAFE_X - 6, 162);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *b = ui_button(row, LV_SYMBOL_PREV, false, 36, 44);
    lv_obj_add_event_cb(b, btn_cb, LV_EVENT_CLICKED, (void *)"prev");
    b = ui_button(row, LV_SYMBOL_PLAY, true, 44, 44);
    s_play_lbl = ui_button_label(b);
    lv_obj_add_event_cb(b, btn_cb, LV_EVENT_CLICKED, (void *)"play_pause");
    b = ui_button(row, LV_SYMBOL_NEXT, false, 36, 44);
    lv_obj_add_event_cb(b, btn_cb, LV_EVENT_CLICKED, (void *)"next");
    s_dirty = true;
    if (pc_online()) {
        lv_obj_add_flag(s_offline, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_body, LV_OBJ_FLAG_HIDDEN);
    }
}

/* POST /api/pc/relay_test: the speaker itself calls http://<relay>/yt/ping, the only way to know that YouTube will
 * play (a PC firewall blocks the speaker -> PC direction, not PC -> speaker). */
static esp_err_t h_relay_test(httpd_req_t *req)
{
    cJSON *b = web_read_json(req);
    if (!b) return ESP_OK;
    cJSON_Delete(b);
    char hp[64], url[96];
    cJSON *r = cJSON_CreateObject();
    if (!radio_relay_hostport(hp, sizeof(hp))) {
        cJSON_AddBoolToObject(r, "ok", false);
        cJSON_AddStringToObject(r, "err", "no_relay");
        return web_send_json(req, r);
    }
    snprintf(url, sizeof(url), "http://%s/yt/ping", hp);
    esp_http_client_config_t cfg = {.url = url, .timeout_ms = 3000};
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    int64_t t0 = esp_timer_get_time();
    esp_err_t e = c ? esp_http_client_perform(c) : ESP_FAIL;
    int st = c && e == ESP_OK ? esp_http_client_get_status_code(c) : 0;
    if (c) esp_http_client_cleanup(c);
    bool ok = st == 200;
    cJSON_AddBoolToObject(r, "ok", ok);
    cJSON_AddStringToObject(r, "relay", hp);
    if (ok) cJSON_AddNumberToObject(r, "ms", (double)((esp_timer_get_time() - t0) / 1000));
    else {
        char err[16];
        if (st) snprintf(err, sizeof(err), "http_%d", st);
        else snprintf(err, sizeof(err), "%s", e == ESP_ERR_HTTP_EAGAIN || e == ESP_ERR_TIMEOUT ? "timeout" : "unreachable");
        cJSON_AddStringToObject(r, "err", err);
    }
    return web_send_json(req, r);
}

static void pc_cfg_hook(cJSON *doc)
{
    cJSON *st = cJSON_AddObjectToObject(doc, "st");
    cJSON_AddBoolToObject(st, "on", pc_online());
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON_AddStringToObject(st, "name", s_agent_name);
    cJSON_AddStringToObject(st, "ip", s_peer);
    cJSON_AddStringToObject(st, "ver", s_agent_ver);
    cJSON_AddNumberToObject(st, "proto", s_agent_proto); /* -1 = no agent seen since boot, 0 = agent before 0.2.0 */
    xSemaphoreGive(s_lock);
    cJSON_AddNumberToObject(st, "proto_need", MB_PROTO);  /* agent older than this: suggest updating it */
    cJSON_AddNumberToObject(st, "seen", s_last_us ? (double)((esp_timer_get_time() - s_last_us) / 1000000) : -1);
}

void pc_app_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    ui_register_builder(UI_TILE_PC, pc_build);
    lv_timer_create(pc_tick, 500, NULL);
    web_register_ws("/ws/pc", ws_handler);
    web_register("/api/pc/relay_test", HTTP_POST, h_relay_test, WEB_AUTH_TOKEN);
    web_set_config_hook("pc", pc_cfg_hook);
}
