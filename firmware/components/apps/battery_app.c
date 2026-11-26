/* Battery runtime measurement.
 *  - samples every 30 s; while on battery a line per minute is appended to /data/batt/log.csv
 *    (ts,raw,mv,pct,bri,vol,play) — survives power loss, rotated at ~2000 lines
 *  - a "session" = from unplugging to plugging in / running empty / switching off; finished sessions are kept
 *    in /data/batt/sessions.json (last 10) with duration, % used and conditions
 *  - remaining time is estimated from the discharge rate of the last 60 min (linear fit)
 *  - GET /api/battery: now + estimate + sessions + samples of the current/last session
 */
#include "apps.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "board.h"
#include "net.h"
#include "settings.h"
#include "player.h"
#include "audio_hw.h"

static const char *TAG = "battery";

#define DIR "/data/batt"
#define LOG_FILE DIR "/log.csv"
#define LOG_OLD DIR "/log.old.csv"
#define SESS_FILE DIR "/sessions.json"
#define CUR_FILE DIR "/current.json"
#define MAX_LINES 2000
#define WIN 120 /* 60 min of 30 s samples */

typedef struct {
    time_t ts;
    float pct;
    int mv;
} samp_t;

static SemaphoreHandle_t s_lock;
static samp_t s_win[WIN];
static int s_wn, s_wi;
static float s_pct_f = -1; /* smoothed percent */
static int s_mv = -1, s_raw = -1;
static bool s_chg;
static int s_eta_min = -1;
static float s_rate = 0; /* %/h */

/* current session */
static bool s_open;
static time_t s_start, s_last;
static float s_start_pct, s_last_pct;
static int s_start_mv, s_last_mv;
static double s_bri_sum, s_play_min, s_n_min;
static int s_lines;

static float raw_to_pct(int raw)
{
    static const int lvl[] = {1970, 2062, 2154, 2246, 2338, 2430};
    if (raw <= lvl[0]) return 0;
    if (raw >= lvl[5]) return 100;
    for (int i = 0; i < 5; i++)
        if (raw < lvl[i + 1]) return i * 20 + (raw - lvl[i]) * 20.0f / (lvl[i + 1] - lvl[i]);
    return 100;
}

/* ---------------- files ---------------- */
static void save_current(void)
{
    char t[256];
    int n = snprintf(t, sizeof(t),
                     "{\"open\":%s,\"start\":%lld,\"last\":%lld,\"sp\":%.1f,\"lp\":%.1f,\"smv\":%d,\"lmv\":%d,\"bri\":%.0f,\"play\":%.0f,\"n\":%.0f}",
                     s_open ? "true" : "false", (long long)s_start, (long long)s_last, s_start_pct, s_last_pct, s_start_mv,
                     s_last_mv, s_bri_sum, s_play_min, s_n_min);
    settings_write_file(CUR_FILE, t, n);
}

static void close_session(const char *reason)
{
    if (!s_open) return;
    s_open = false;
    int minutes = (int)(difftime(s_last, s_start) / 60);
    ESP_LOGI(TAG, "session closed (%s): %d min, %.0f%% -> %.0f%%", reason, minutes, s_start_pct, s_last_pct);
    if (minutes >= 2) {
        char *txt = settings_read_file(SESS_FILE, NULL);
        cJSON *arr = txt ? cJSON_Parse(txt) : NULL;
        free(txt);
        if (!cJSON_IsArray(arr)) {
            cJSON_Delete(arr);
            arr = cJSON_CreateArray();
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "start", (double)s_start);
        cJSON_AddNumberToObject(o, "min", minutes);
        cJSON_AddNumberToObject(o, "from", (int)(s_start_pct + 0.5f));
        cJSON_AddNumberToObject(o, "to", (int)(s_last_pct + 0.5f));
        cJSON_AddNumberToObject(o, "mv_from", s_start_mv);
        cJSON_AddNumberToObject(o, "mv_to", s_last_mv);
        cJSON_AddNumberToObject(o, "bri", s_n_min > 0 ? (int)(s_bri_sum / s_n_min) : 0);
        cJSON_AddNumberToObject(o, "play_min", (int)s_play_min);
        cJSON_AddStringToObject(o, "end", reason);
        float used = s_start_pct - s_last_pct;
        /* projected full-charge runtime (100% -> 0%) from this session */
        if (used >= 5) cJSON_AddNumberToObject(o, "full_h", round(minutes / 60.0 * 100.0 / used * 10) / 10);
        cJSON_InsertItemInArray(arr, 0, o);
        while (cJSON_GetArraySize(arr) > 10) cJSON_DeleteItemFromArray(arr, 10);
        char *out = cJSON_PrintUnformatted(arr);
        settings_write_file(SESS_FILE, out, strlen(out));
        cJSON_free(out);
        cJSON_Delete(arr);
    }
    save_current();
}

static void open_session(time_t now)
{
    s_open = true;
    s_start = s_last = now;
    s_start_pct = s_last_pct = s_pct_f;
    s_start_mv = s_last_mv = s_mv;
    s_bri_sum = s_play_min = s_n_min = 0;
    /* new log for the new session */
    unlink(LOG_OLD);
    rename(LOG_FILE, LOG_OLD);
    s_lines = 0;
    save_current();
    ESP_LOGI(TAG, "on battery: session started at %.0f%%", s_pct_f);
}

static void append_log(time_t now, int raw)
{
    FILE *f = fopen(LOG_FILE, "a");
    if (!f) return;
    fprintf(f, "%lld,%d,%d,%.1f,%d,%d,%d\n", (long long)now, raw, s_mv, s_pct_f, board_get_brightness(),
            audio_hw_get_volume(), player_state() == PLAYER_PLAY);
    fclose(f);
    if (++s_lines >= MAX_LINES) { /* rotate */
        unlink(LOG_OLD);
        rename(LOG_FILE, LOG_OLD);
        s_lines = 0;
    }
}

/* Decide what happened to an open session while we were off. */
static void resume_from_disk(void)
{
    mkdir(DIR, 0775);
    char *txt = settings_read_file(CUR_FILE, NULL);
    cJSON *j = txt ? cJSON_Parse(txt) : NULL;
    free(txt);
    if (!j) return;
    s_open = cJSON_IsTrue(cJSON_GetObjectItem(j, "open"));
    s_start = (time_t)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "start"));
    s_last = (time_t)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "last"));
    s_start_pct = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "sp"));
    s_last_pct = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "lp"));
    s_start_mv = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "smv"));
    s_last_mv = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "lmv"));
    s_bri_sum = cJSON_GetNumberValue(cJSON_GetObjectItem(j, "bri"));
    s_play_min = cJSON_GetNumberValue(cJSON_GetObjectItem(j, "play"));
    s_n_min = cJSON_GetNumberValue(cJSON_GetObjectItem(j, "n"));
    cJSON_Delete(j);
    /* count existing log lines */
    FILE *f = fopen(LOG_FILE, "r");
    if (f) {
        int c;
        while ((c = fgetc(f)) != EOF)
            if (c == '\n') s_lines++;
        fclose(f);
    }
}

/* ---------------- sampling ---------------- */
static void estimate(void)
{
    /* least squares pct(t) over the window -> %/h */
    if (s_wn < 20) { s_eta_min = -1; return; }
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    time_t t0 = s_win[(s_wi - s_wn + WIN) % WIN].ts;
    for (int k = 0; k < s_wn; k++) {
        samp_t *p = &s_win[(s_wi - s_wn + k + WIN) % WIN];
        double x = difftime(p->ts, t0) / 3600.0, y = p->pct;
        sx += x; sy += y; sxx += x * x; sxy += x * y;
    }
    double den = s_wn * sxx - sx * sx;
    if (fabs(den) < 1e-9) return;
    double slope = (s_wn * sxy - sx * sy) / den; /* %/h, negative when discharging */
    s_rate = (float)-slope;
    s_eta_min = s_rate > 0.3 ? (int)(s_pct_f / s_rate * 60) : -1;
}

static void battery_task(void *arg)
{
    int minute_tick = 0;
    bool resumed = false;
    while (1) {
        int raw = board_battery_raw();
        int mv = board_battery_mv();
        bool chg = board_is_charging();
        time_t now = time(NULL);
        bool tv = net_time_valid();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (raw > 0) {
            float p = raw_to_pct(raw);
            s_pct_f = s_pct_f < 0 ? p : s_pct_f * 0.8f + p * 0.2f; /* smooth ADC noise */
            s_raw = raw;
        }
        if (mv > 0) s_mv = s_mv < 0 ? mv : (s_mv * 4 + mv) / 5;
        s_chg = chg;

        if (tv && !resumed) {
            resumed = true;
            if (s_open) {
                double gap = difftime(now, s_last);
                if (chg) close_session(s_last_pct <= 5 ? "empty" : "off");
                else if (gap > 600) close_session(s_last_pct <= 5 ? "empty" : "off");
                else ESP_LOGI(TAG, "continuing session after %.0f s off", gap);
            }
        }
        if (tv && resumed) {
            if (chg) {
                if (s_open) close_session("plugged");
                s_wn = 0;
                s_eta_min = -1;
            } else {
                if (!s_open) open_session(now);
                s_win[s_wi] = (samp_t){now, s_pct_f, s_mv};
                s_wi = (s_wi + 1) % WIN;
                if (s_wn < WIN) s_wn++;
                estimate();
                if (++minute_tick % 2 == 0) {
                    s_last = now;
                    s_last_pct = s_pct_f;
                    s_last_mv = s_mv;
                    s_bri_sum += board_get_brightness();
                    s_n_min += 1;
                    if (player_state() == PLAYER_PLAY) s_play_min += 1;
                    append_log(now, raw);
                    if (minute_tick % 10 == 0) save_current(); /* every 5 min */
                }
            }
        }
        xSemaphoreGive(s_lock);
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

/* ---------------- public ---------------- */
int battery_eta_min(void) { return s_eta_min; }
int battery_mv(void) { return s_mv; }
int battery_pct(void) { return s_pct_f < 0 ? board_battery_percent() : (int)(s_pct_f + 0.5f); }

static void status_hook_bat(cJSON *r)
{
    cJSON_ReplaceItemInObject(r, "bat", cJSON_CreateNumber(battery_pct()));
    cJSON_AddNumberToObject(r, "bat_mv", s_mv);
    cJSON_AddNumberToObject(r, "bat_eta", s_eta_min);
}

static esp_err_t h_battery(httpd_req_t *req)
{
    cJSON *r = cJSON_CreateObject();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON_AddNumberToObject(r, "pct", battery_pct());
    cJSON_AddNumberToObject(r, "raw", s_raw);
    cJSON_AddNumberToObject(r, "mv", s_mv);
    cJSON_AddBoolToObject(r, "chg", s_chg);
    cJSON_AddNumberToObject(r, "rate", round(s_rate * 10) / 10); /* %/h */
    cJSON_AddNumberToObject(r, "eta", s_eta_min);                 /* minutes, -1 unknown */
    cJSON *cur = cJSON_AddObjectToObject(r, "cur");
    cJSON_AddBoolToObject(cur, "open", s_open);
    cJSON_AddNumberToObject(cur, "start", (double)s_start);
    cJSON_AddNumberToObject(cur, "min", s_open ? (int)(difftime(time(NULL), s_start) / 60) : 0);
    cJSON_AddNumberToObject(cur, "from", (int)(s_start_pct + 0.5f));
    xSemaphoreGive(s_lock);

    char *txt = settings_read_file(SESS_FILE, NULL);
    cJSON *sess = txt ? cJSON_Parse(txt) : NULL;
    free(txt);
    cJSON_AddItemToObject(r, "sessions", cJSON_IsArray(sess) ? sess : cJSON_CreateArray());
    if (!cJSON_IsArray(sess)) cJSON_Delete(sess);

    /* samples of the current (or last) session for a chart: [ts, pct] every ~N lines, max 240 points */
    cJSON *pts = cJSON_AddArrayToObject(r, "pts");
    FILE *f = fopen(LOG_FILE, "r");
    if (!f) f = fopen(LOG_OLD, "r");
    if (f) {
        int lines = 0, c;
        while ((c = fgetc(f)) != EOF)
            if (c == '\n') lines++;
        rewind(f);
        int every = lines / 240 + 1, i = 0;
        char line[96];
        while (fgets(line, sizeof(line), f)) {
            if (i++ % every) continue;
            long long ts;
            int rw, mv;
            float pct;
            if (sscanf(line, "%lld,%d,%d,%f", &ts, &rw, &mv, &pct) == 4) {
                cJSON *p = cJSON_CreateArray();
                cJSON_AddItemToArray(p, cJSON_CreateNumber((double)ts));
                cJSON_AddItemToArray(p, cJSON_CreateNumber((int)(pct * 10) / 10.0));
                cJSON_AddItemToArray(p, cJSON_CreateNumber(mv));
                cJSON_AddItemToArray(pts, p);
            }
        }
        fclose(f);
    }
    return web_send_json(req, r);
}

static esp_err_t h_log(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"meowbox-battery.csv\"");
    httpd_resp_sendstr_chunk(req, "ts,raw,mv,pct,brightness,volume,radio\n");
    const char *files[] = {LOG_OLD, LOG_FILE};
    char *buf = malloc(2048);
    for (int k = 0; k < 2; k++) {
        FILE *f = fopen(files[k], "r");
        if (!f) continue;
        size_t n;
        while ((n = fread(buf, 1, 2048, f)) > 0) httpd_resp_send_chunk(req, buf, n);
        fclose(f);
    }
    free(buf);
    return httpd_resp_send_chunk(req, NULL, 0);
}

void battery_app_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    resume_from_disk();
    web_register("/api/battery", HTTP_GET, h_battery, WEB_AUTH_SESSION);
    web_register("/api/battery/log.csv", HTTP_GET, h_log, WEB_AUTH_SESSION);
    xTaskCreate(battery_task, "battery", 4096, NULL, 2, NULL);
    (void)status_hook_bat;
}

void battery_status_hook(cJSON *r) { status_hook_bat(r); }
