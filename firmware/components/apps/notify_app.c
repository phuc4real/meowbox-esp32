/* Notification speaker (P4): /api/notify (POST JSON or GET query), /api/notify/audio, /api/notify/history. */
#include "apps.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "ui.h"
#include "net.h"
#include "settings.h"
#include "player.h"
#include "audio_sfx.h"
#include "utf8.h"

static const char *TAG = "notify";

#define QMAX 10
#define HMAX 20

typedef struct {
    int id;
    time_t ts;
    char title[49];
    char text[201];
    char sound[8];    /* chime | alert | none */
    char prio[8];     /* low | normal | high */
    char src[8];      /* api | pc | ntfy | audio */
    int duration;
} note_t;

static note_t s_q[QMAX];
static int s_qn;
static note_t s_hist[HMAX];
static int s_hn;
static int s_next_id = 1;
static SemaphoreHandle_t s_lock;
static bool s_resume_radio;

static void copy_str(char *dst, size_t len, const char *src) { utf8_copy(dst, len, src); }

/* Returns id, or -1 if the queue is full / bad input */
int notify_post(const char *title, const char *text, const char *sound, const char *prio, int duration, const char *src)
{
    if (!text || !*text) return -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_qn >= QMAX) {
        xSemaphoreGive(s_lock);
        return -1;
    }
    note_t *n = &s_q[s_qn++];
    memset(n, 0, sizeof(*n));
    n->id = s_next_id++;
    n->ts = time(NULL);
    copy_str(n->title, sizeof(n->title), title);
    copy_str(n->text, sizeof(n->text), text);
    copy_str(n->sound, sizeof(n->sound), sound && (!strcmp(sound, "alert") || !strcmp(sound, "none")) ? sound : "chime");
    copy_str(n->prio, sizeof(n->prio), prio && (!strcmp(prio, "low") || !strcmp(prio, "high")) ? prio : "normal");
    copy_str(n->src, sizeof(n->src), src);
    int def = settings_get_int("notify", "dur", 10);
    n->duration = duration > 0 && duration <= 300 ? duration : def;
    /* history: newest first */
    memmove(&s_hist[1], &s_hist[0], sizeof(note_t) * (HMAX - 1));
    s_hist[0] = *n;
    if (s_hn < HMAX) s_hn++;
    int id = n->id;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "#%d [%s/%s] %s: %s", id, n->prio, n->src, n->title, n->text);
    return id;
}

static void show_next(lv_timer_t *t)
{
    if (ui_overlay_visible() || clock_alarm_ringing()) return;
    note_t n;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_qn) {
        xSemaphoreGive(s_lock);
        if (s_resume_radio) { /* radio was on before an audio notification */
            player_state_t ps = player_state();
            bool busy = ps == PLAYER_PLAY || ps == PLAYER_BUF;
            if (!busy) { /* the clip ended or failed (ERR used to leave this flag set for a later stop to fire) */
                s_resume_radio = false;
                radio_play_id(0);
            } else if (!player_is_mem()) {
                s_resume_radio = false; /* another stream took over */
            }
        }
        return;
    }
    n = s_q[0];
    memmove(&s_q[0], &s_q[1], sizeof(note_t) * (QMAX - 1));
    s_qn--;
    xSemaphoreGive(s_lock);

    bool high = !strcmp(n.prio, "high"), low = !strcmp(n.prio, "low");
    bool quiet = ui_is_night() && !high && (low ? settings_get_bool("notify", "quiet_low", true) : false);
    if (high) ui_force_bright(n.duration + 5);
    ui_color_t col = high ? UI_C_ALERT : !strcmp(n.src, "pc") ? UI_C_ACCENT2 : UI_C_ACCENT;
    ui_overlay_show(LV_SYMBOL_BELL, n.title[0] ? n.title : ui_tr("Thông báo", "Notification"), n.text, col, n.duration,
                    NULL, NULL);
    if (!quiet && strcmp(n.sound, "none") != 0) audio_sfx_play(!strcmp(n.sound, "alert") ? SFX_ALERT : SFX_CHIME);
}

/* ---------------- HTTP ---------------- */
static esp_err_t reply_id(httpd_req_t *req, int id)
{
    if (id < 0) return web_send_err(req, 429, "queue_full");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddNumberToObject(r, "id", id);
    return web_send_json(req, r);
}

static esp_err_t h_post(httpd_req_t *req)
{
    cJSON *j = web_read_json(req);
    if (!j) return ESP_OK;
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(j, "text"));
    if (!text || !*text || strlen(text) > 200) {
        cJSON_Delete(j);
        return web_send_err(req, 400, "bad_value");
    }
    cJSON *d = cJSON_GetObjectItem(j, "duration");
    int id = notify_post(cJSON_GetStringValue(cJSON_GetObjectItem(j, "title")), text,
                         cJSON_GetStringValue(cJSON_GetObjectItem(j, "sound")),
                         cJSON_GetStringValue(cJSON_GetObjectItem(j, "priority")), cJSON_IsNumber(d) ? d->valueint : 0, "api");
    cJSON_Delete(j);
    return reply_id(req, id);
}

static esp_err_t h_get(httpd_req_t *req)
{
    char text[256] = "", title[64] = "", sound[8] = "", prio[8] = "", dur[8] = "";
    if (!web_query(req, "text", text, sizeof(text)) || !text[0]) return web_send_err(req, 400, "bad_value");
    web_query(req, "title", title, sizeof(title));
    web_query(req, "sound", sound, sizeof(sound));
    web_query(req, "priority", prio, sizeof(prio));
    web_query(req, "duration", dur, sizeof(dur));
    text[200] = 0;
    title[48] = 0;
    utf8_trim(text);
    utf8_trim(title);
    return reply_id(req, notify_post(title, text, sound, prio, atoi(dur), "api"));
}

static esp_err_t h_audio(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > 1024 * 1024) return web_send_err(req, 413, "too_large");
    uint8_t *buf = heap_caps_malloc(req->content_len, MALLOC_CAP_SPIRAM);
    if (!buf) return web_send_err(req, 503, "busy");
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, (char *)buf + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            free(buf);
            return ESP_FAIL;
        }
        got += r;
    }
    char title[64] = "", text[256] = "";
    web_query(req, "title", title, sizeof(title));
    web_query(req, "text", text, sizeof(text));
    title[48] = 0;
    text[200] = 0;
    utf8_trim(title);
    utf8_trim(text);
    bool was_radio = radio_playing() && !player_is_mem();
    if (player_play_mem(buf, got, title[0] ? title : "Notification") != ESP_OK) return web_send_err(req, 400, "bad_value");
    if (was_radio) s_resume_radio = true;
    int id = notify_post(title, text[0] ? text : (title[0] ? " " : LV_SYMBOL_AUDIO), "none", "normal", 0, "audio");
    return reply_id(req, id < 0 ? 0 : id);
}

static esp_err_t h_history(httpd_req_t *req)
{
    cJSON *r = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(r, "items");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_hn; i++) {
        note_t *n = &s_hist[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", n->id);
        cJSON_AddNumberToObject(o, "ts", (double)n->ts);
        cJSON_AddStringToObject(o, "title", n->title);
        cJSON_AddStringToObject(o, "text", n->text);
        cJSON_AddStringToObject(o, "sound", n->sound);
        cJSON_AddStringToObject(o, "priority", n->prio);
        cJSON_AddStringToObject(o, "src", n->src);
        cJSON_AddItemToArray(arr, o);
    }
    xSemaphoreGive(s_lock);
    return web_send_json(req, r);
}

void notify_app_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    web_register("/api/notify", HTTP_POST, h_post, WEB_AUTH_TOKEN);
    web_register("/api/notify", HTTP_GET, h_get, WEB_AUTH_TOKEN);
    web_register("/api/notify/audio", HTTP_POST, h_audio, WEB_AUTH_TOKEN);
    web_register("/api/notify/history", HTTP_GET, h_history, WEB_AUTH_TOKEN);
    lv_timer_create(show_next, 300, NULL);
}
