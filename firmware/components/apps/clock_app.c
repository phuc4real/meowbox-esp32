/* Clock tile: time, lunar calendar, weather (open-meteo), alarms. One face layout per theme. */
#include "apps.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "ui.h"
#include "board.h"
#include "net.h"
#include "settings.h"
#include "lunar.h"
#include "audio_sfx.h"
#include "player.h"

static const char *TAG = "clock";

/* ---------------- weather ---------------- */
typedef struct {
    bool valid;
    float t;
    int code;
    int hi, lo;
    time_t at;
} weather_t;

static weather_t s_wx;
static TaskHandle_t s_wx_task;

static const char *wmo_text(int c, bool en)
{
    if (c == 0) return en ? "Clear" : "Trời quang";
    if (c <= 2) return en ? "Partly cloudy" : "Ít mây";
    if (c == 3) return en ? "Overcast" : "Nhiều mây";
    if (c == 45 || c == 48) return en ? "Fog" : "Sương mù";
    if (c >= 51 && c <= 57) return en ? "Drizzle" : "Mưa phùn";
    if (c >= 61 && c <= 67) return en ? "Rain" : "Mưa";
    if (c >= 71 && c <= 77) return en ? "Snow" : "Tuyết";
    if (c >= 80 && c <= 82) return en ? "Showers" : "Mưa rào";
    if (c >= 95) return en ? "Thunderstorm" : "Dông";
    return "?";
}

static bool fetch_weather(void)
{
    if (!settings_get_bool("weather", "on", true)) return false;
    char url[256];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,weather_code&daily=temperature_2m_max,temperature_2m_min"
             "&timezone=Asia%%2FBangkok&forecast_days=1",
             settings_get_num("weather", "lat", 21.0245), settings_get_num("weather", "lon", 105.8412));
    esp_http_client_config_t cfg = {.url = url, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 10000,
                                    .buffer_size = 2048};
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    bool ok = false;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        char *body = calloc(1, 4096);
        int n = 0, r;
        while (n < 4095 && (r = esp_http_client_read(c, body + n, 4095 - n)) > 0) n += r;
        if (esp_http_client_get_status_code(c) == 200) {
            cJSON *j = cJSON_Parse(body);
            cJSON *cur = cJSON_GetObjectItem(j, "current"), *daily = cJSON_GetObjectItem(j, "daily");
            cJSON *t = cJSON_GetObjectItem(cur, "temperature_2m"), *wc = cJSON_GetObjectItem(cur, "weather_code");
            cJSON *hi = cJSON_GetArrayItem(cJSON_GetObjectItem(daily, "temperature_2m_max"), 0);
            cJSON *lo = cJSON_GetArrayItem(cJSON_GetObjectItem(daily, "temperature_2m_min"), 0);
            if (cJSON_IsNumber(t) && cJSON_IsNumber(wc)) {
                s_wx.t = (float)t->valuedouble;
                s_wx.code = wc->valueint;
                s_wx.hi = cJSON_IsNumber(hi) ? (int)(hi->valuedouble + 0.5) : 0;
                s_wx.lo = cJSON_IsNumber(lo) ? (int)(lo->valuedouble + 0.5) : 0;
                s_wx.at = time(NULL);
                s_wx.valid = true;
                ok = true;
                ESP_LOGI(TAG, "weather %.1f C code %d (%d/%d)", s_wx.t, s_wx.code, s_wx.hi, s_wx.lo);
            }
            cJSON_Delete(j);
        } else {
            ESP_LOGW(TAG, "weather http %d", esp_http_client_get_status_code(c));
        }
        free(body);
    }
    esp_http_client_cleanup(c);
    return ok;
}

static void weather_task(void *arg)
{
    while (1) {
        if (!wifi_mgr_connected()) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
            continue;
        }
        bool ok = fetch_weather();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ok ? 30 * 60000 : 60000));
    }
}

static void on_settings(const char *group, void *arg)
{
    if (strcmp(group, "weather") == 0 && s_wx_task) {
        s_wx.valid = false;
        xTaskNotifyGive(s_wx_task);
    }
}

static void weather_cfg_hook(cJSON *doc)
{
    if (!s_wx.valid) {
        cJSON_AddNullToObject(doc, "now");
        return;
    }
    cJSON *n = cJSON_AddObjectToObject(doc, "now");
    cJSON_AddNumberToObject(n, "t", (int)(s_wx.t * 10) / 10.0);
    cJSON_AddNumberToObject(n, "code", s_wx.code);
    cJSON_AddNumberToObject(n, "hi", s_wx.hi);
    cJSON_AddNumberToObject(n, "lo", s_wx.lo);
    cJSON_AddNumberToObject(n, "at", (double)s_wx.at);
}

/* ---------------- alarms ---------------- */
static bool s_ringing, s_ring_radio;
static lv_timer_t *s_ring_timer;
static time_t s_ring_started, s_snooze_at;
static volatile int s_stop_req; /* 1 = stop, 2 = snooze */
static int s_last_min = -1;

#define HINT_TAG ((void *)0x4C41524D)

static void ring_tick(lv_timer_t *t)
{
    if (time(NULL) - s_ring_started > 5 * 60) { /* give up after 5 minutes */
        clock_alarm_stop(false);
        return;
    }
    /* radio alarm: fall back to the chime if the stream did not start within ~20 s */
    if (s_ring_radio && (radio_playing() || time(NULL) - s_ring_started < 20)) return;
    s_ring_radio = false;
    audio_sfx_play(SFX_ALARM);
}

static void ring_dismissed(bool tapped, void *arg)
{
    if (s_ringing) s_stop_req = tapped ? 2 : 1;
}

static void start_ring(int h, int m, int station)
{
    if (s_ringing) return;
    s_ringing = true;
    s_ring_radio = station > 0 && radio_play_id(station) == 0;
    s_ring_started = time(NULL);
    char t[32];
    snprintf(t, sizeof(t), "%02d:%02d", h, m);
    ui_overlay_show(LV_SYMBOL_BELL, ui_tr("Báo thức", "Alarm"), t, UI_C_ALERT, 0, ring_dismissed, NULL);
    lv_obj_t *hint = ui_label(lv_layer_top(), UI_F_SMALL, UI_C_DIM,
                              ui_tr("Chạm: báo lại · Giữ VOL: tắt", "Tap: snooze · Hold VOL: stop"));
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -UI_SAFE_Y - 4);
    lv_obj_set_user_data(hint, HINT_TAG);
    ui_force_bright(300);
    if (!s_ring_radio) audio_sfx_play(SFX_ALARM);
    s_ring_timer = lv_timer_create(ring_tick, 2200, NULL);
    ESP_LOGI(TAG, "alarm %s ringing", t);
}

static void finish_ring(bool snooze)
{
    if (!s_ringing) return;
    s_ringing = false;
    if (s_ring_timer) lv_timer_delete(s_ring_timer);
    s_ring_timer = NULL;
    audio_sfx_stop();
    /* the alarm's own stream: stop it outright (also on snooze, the chime comes back later). radio_toggle() would
     * START the radio if the stream had already failed (ERR) */
    if (s_ring_radio) player_stop();
    s_ring_radio = false;
    lv_obj_t *top = lv_layer_top();
    for (int i = (int)lv_obj_get_child_count(top) - 1; i >= 0; i--) {
        lv_obj_t *c = lv_obj_get_child(top, i);
        if (lv_obj_get_user_data(c) == HINT_TAG) lv_obj_delete(c);
    }
    if (ui_overlay_visible()) ui_overlay_hide();
    if (snooze) {
        s_snooze_at = time(NULL) + settings_get_int("alarms", "snooze", 5) * 60;
        ESP_LOGI(TAG, "snoozed");
    }
}

bool clock_alarm_ringing(void) { return s_ringing; }
void clock_alarm_stop(bool snooze) { s_stop_req = snooze ? 2 : 1; }

static void check_alarms(const struct tm *tm)
{
    if (s_snooze_at && time(NULL) >= s_snooze_at) {
        s_snooze_at = 0;
        start_ring(tm->tm_hour, tm->tm_min, 0);
        return;
    }
    if (tm->tm_min == s_last_min) return;
    s_last_min = tm->tm_min;
    cJSON *cfg = settings_get("alarms");
    cJSON *it;
    int wd = (tm->tm_wday + 6) % 7; /* Monday = 0 */
    bool changed = false;
    cJSON_ArrayForEach(it, cJSON_GetObjectItem(cfg, "al")) {
        bool on = cJSON_IsTrue(cJSON_GetObjectItem(it, "on"));
        cJSON *jh = cJSON_GetObjectItem(it, "h"), *jm = cJSON_GetObjectItem(it, "m"), *jd = cJSON_GetObjectItem(it, "days");
        int h = cJSON_IsNumber(jh) ? jh->valueint : -1, m = cJSON_IsNumber(jm) ? jm->valueint : -1;
        int days = cJSON_IsNumber(jd) ? jd->valueint : 0;
        if (on && h == tm->tm_hour && m == tm->tm_min && (days == 0 || (days & (1 << wd)))) {
            const char *snd = cJSON_GetStringValue(cJSON_GetObjectItem(it, "snd"));
            cJSON *stn = cJSON_GetObjectItem(it, "st");
            start_ring(h, m, snd && !strcmp(snd, "radio") && cJSON_IsNumber(stn) ? stn->valueint : 0);
            if (days == 0) {
                cJSON_ReplaceItemInObject(it, "on", cJSON_CreateFalse());
                changed = true;
            }
        }
    }
    if (changed) settings_update("alarms", cfg);
    cJSON_Delete(cfg);
}

static bool next_alarm(const struct tm *now, int *oh, int *om)
{
    cJSON *cfg = settings_get("alarms");
    cJSON *it;
    int best = 8 * 1440, nowm = ((now->tm_wday + 6) % 7) * 1440 + now->tm_hour * 60 + now->tm_min;
    cJSON_ArrayForEach(it, cJSON_GetObjectItem(cfg, "al")) {
        if (!cJSON_IsTrue(cJSON_GetObjectItem(it, "on"))) continue;
        cJSON *jh = cJSON_GetObjectItem(it, "h"), *jm = cJSON_GetObjectItem(it, "m"), *jd = cJSON_GetObjectItem(it, "days");
        if (!cJSON_IsNumber(jh) || !cJSON_IsNumber(jm)) continue;
        int h = jh->valueint, m = jm->valueint, days = cJSON_IsNumber(jd) ? jd->valueint : 0;
        for (int d = 0; d < 7; d++) {
            if (days && !(days & (1 << d))) continue;
            int t = d * 1440 + h * 60 + m - nowm;
            if (t <= 0) t += 7 * 1440;
            if (t < best) {
                best = t;
                *oh = h;
                *om = m;
            }
        }
    }
    cJSON_Delete(cfg);
    return best < 8 * 1440;
}

/* ---------------- UI ---------------- */
LV_IMAGE_DECLARE(ui_img_enso);

static lv_obj_t *l_time, *l_g1, *l_g2, *l_wday, *l_date, *l_lunar, *l_lunar2, *l_wx, *l_wx2, *l_wx_head;
static lv_obj_t *l_alarm, *l_bat, *l_net, *l_term;

static const char *const WD_VI[] = {"Chủ Nhật", "Thứ Hai", "Thứ Ba", "Thứ Tư", "Thứ Năm", "Thứ Sáu", "Thứ Bảy"};
static const char *const WD_EN[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *const MON_EN[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

static void set_text(lv_obj_t *l, const char *t)
{
    if (l && strcmp(lv_label_get_text(l), t) != 0) lv_label_set_text(l, t);
}

static void show(lv_obj_t *o, bool on)
{
    if (!o) return;
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void update_status(const struct tm *tm)
{
    const theme_def_t *th = ui_theme();
    char buf[64];
    int pct = battery_pct(), eta = battery_eta_min();
    if (pct < 0) pct = 0;
    bool raw = th->btn == THEME_BTN_RAW;
    if (raw) snprintf(buf, sizeof(buf), "%s\n%d%%", board_is_charging() ? LV_SYMBOL_CHARGE : ui_tr("PIN", "BAT"), pct);
    else if (board_is_charging()) snprintf(buf, sizeof(buf), LV_SYMBOL_CHARGE " %s %d%%", ui_tr("Pin", "Bat"), pct);
    else if (eta > 0) snprintf(buf, sizeof(buf), "%s %d%%, %dh%02d", ui_tr("Pin", "Bat"), pct, eta / 60, eta % 60);
    else snprintf(buf, sizeof(buf), "%s %d%%", ui_tr("Pin", "Bat"), pct);
    set_text(l_bat, buf);
    if (l_net) {
        int rssi = 0;
        if (wifi_mgr_connected()) {
            wifi_mgr_info(NULL, 0, NULL, 0, &rssi);
            snprintf(buf, sizeof(buf), "NET %ddBm", rssi);
        } else {
            snprintf(buf, sizeof(buf), "NET %s", wifi_mgr_mode() == NET_MODE_AP ? "AP" : "...");
        }
        set_text(l_net, buf);
    }
    int ah = 0, am = 0;
    bool has = tm->tm_year >= 120 && next_alarm(tm, &ah, &am);
    if (has && th->btn == THEME_BTN_RAW) snprintf(buf, sizeof(buf), "%s %02d:%02d", ui_tr("báo thức", "alarm"), ah, am);
    else if (has) snprintf(buf, sizeof(buf), LV_SYMBOL_BELL " %02d:%02d", ah, am);
    set_text(l_alarm, has ? buf : "");
}

static void clock_tick(lv_timer_t *tmr)
{
    if (s_stop_req) {
        int r = s_stop_req;
        s_stop_req = 0;
        finish_ring(r == 2);
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year >= 120) check_alarms(&tm);
    if (ui_active_tile() != UI_TILE_CLOCK || !l_time) return;

    static int n;
    if (n++ % 4 == 0) update_status(&tm);

    const theme_def_t *th = ui_theme();
    bool en = ui_lang_en();
    char buf[128];

    if (tm.tm_year < 120) {
        set_text(l_time, "--:--");
        set_text(l_g1, "--:--");
        set_text(l_g2, "--:--");
        set_text(l_date, wifi_mgr_mode() == NET_MODE_AP ? wifi_mgr_ap_ssid() : ui_tr("Đang chờ giờ mạng", "Waiting for time"));
        if (wifi_mgr_mode() == NET_MODE_AP) {
            snprintf(buf, sizeof(buf), "%s %s, http://192.168.4.1", ui_tr("Mật khẩu", "Password"), wifi_mgr_ap_pass());
            set_text(l_lunar, buf);
        } else set_text(l_lunar, "");
        return;
    }
    bool h24 = settings_get_bool("display", "h24", true);
    int hh = h24 ? tm.tm_hour : (tm.tm_hour % 12 ? tm.tm_hour % 12 : 12);
    bool colon = (tm.tm_sec % 2) == 0 || th->btn == THEME_BTN_RAW;
    snprintf(buf, sizeof(buf), colon ? "%02d:%02d" : "%02d %02d", hh, tm.tm_min);
    set_text(l_time, buf);
    set_text(l_g1, buf);
    set_text(l_g2, buf);

    int y = tm.tm_year + 1900, m = tm.tm_mon + 1, d = tm.tm_mday;
    lunar_date_t ld = lunar_from_solar(y, m, d);
    char lbuf[48], cc[24];
    lunar_format(&ld, en ? LUNAR_FMT_EN : LUNAR_FMT_VI, lbuf, sizeof(lbuf));
    lunar_can_chi_year(ld.year, cc, sizeof(cc));
    const char *hol = lunar_holiday(y, m, d, &ld, en);

    switch (th->clock) {
    case THEME_CLOCK_LEFT:
        set_text(l_wday, en ? WD_EN[tm.tm_wday] : WD_VI[tm.tm_wday]);
        snprintf(buf, sizeof(buf), "%02d.%02d.%d", d, m, y);
        set_text(l_date, buf);
        {
            char nbuf[32];
            lunar_format(&ld, en ? LUNAR_FMT_EN : LUNAR_FMT_VI_NUM, nbuf, sizeof(nbuf));
            set_text(l_lunar, nbuf);
        }
        snprintf(buf, sizeof(buf), "%s %s", en ? "year of" : "năm", cc);
        set_text(l_lunar2, hol ? hol : buf);
        break;
    case THEME_CLOCK_ENSO:
        set_text(l_wday, en ? WD_EN[tm.tm_wday] : WD_VI[tm.tm_wday]);
        if (en) snprintf(buf, sizeof(buf), "%s %d", MON_EN[tm.tm_mon], d);
        else snprintf(buf, sizeof(buf), "%d tháng %d", d, m);
        set_text(l_date, buf);
        {
            char nbuf[32];
            lunar_format(&ld, en ? LUNAR_FMT_EN : LUNAR_FMT_VI_NUM, nbuf, sizeof(nbuf));
            snprintf(buf, sizeof(buf), "%s\n%s", nbuf, hol ? hol : cc);
        }
        set_text(l_lunar, buf);
        break;
    default:
        if (en) snprintf(buf, sizeof(buf), "%s, %s %d, %d", WD_EN[tm.tm_wday], MON_EN[tm.tm_mon], d, y);
        else snprintf(buf, sizeof(buf), "%s, %d/%d/%d", WD_VI[tm.tm_wday], d, m, y);
        set_text(l_date, buf);
        if (th->clock == THEME_CLOCK_BLOCK) /* 186 px of monospace: short form, holiday instead of the year name */
            snprintf(buf, sizeof(buf), "%s %s%s%s", en ? "Lunar" : "ÂL", lbuf, hol ? ", " : "", hol ? hol : "");
        else snprintf(buf, sizeof(buf), "%s, %s", lbuf, hol ? hol : cc);
        set_text(l_lunar, buf);
        break;
    }

    bool wx = s_wx.valid;
    if (wx) {
        int tc = (int)(s_wx.t + (s_wx.t >= 0 ? 0.5f : -0.5f));
        const char *w = wmo_text(s_wx.code, en);
        switch (th->clock) {
        case THEME_CLOCK_LEFT: /* a 68 px column: like "Âm lịch" next to it, no city or high/low (the web has them) */
            set_text(l_wx_head, en ? "Weather" : "Thời tiết");
            snprintf(buf, sizeof(buf), "%d°", tc);
            set_text(l_wx, buf);
            set_text(l_wx2, w);
            break;
        case THEME_CLOCK_BLOCK:
            snprintf(buf, sizeof(buf), "%d°C, %s", tc, w);
            set_text(l_wx, buf);
            break;
        case THEME_CLOCK_CENTER:
            snprintf(buf, sizeof(buf), "%d°C  %s  %d/%d", tc, w, s_wx.hi, s_wx.lo);
            set_text(l_wx, buf);
            break;
        default:
            snprintf(buf, sizeof(buf), "%d°, %d°/%d°\n%s", tc, s_wx.hi, s_wx.lo, w);
            set_text(l_wx, buf);
            break;
        }
    }
    show(l_wx, wx);
    show(l_wx2, wx);
    show(l_wx_head, wx);
    if (l_term) {
        snprintf(buf, sizeof(buf), "> %s: %s, radio: %s_", en ? "cat" : "mèo", cat_mood_text(), player_state_str());
        set_text(l_term, buf);
    }
}

static lv_obj_t *clock_label(lv_obj_t *parent, ui_color_t c)
{
    lv_obj_t *l = ui_label(parent, UI_F_CLOCK, c, "--:--");
    if (ui_theme()->btn == THEME_BTN_FILL) lv_obj_set_style_text_letter_space(l, -2, 0);
    return l;
}

static lv_obj_t *plain_box(lv_obj_t *parent, int x, int y, int w, int h, ui_color_t bg, int border)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, ui_lc(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    if (border) {
        lv_obj_set_style_border_width(o, border, 0);
        lv_obj_set_style_border_color(o, ui_lc(UI_C_LINE), 0);
    }
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static void build_left(lv_obj_t *t) /* Swiss */
{
    const int lh = lv_font_get_line_height(ui_f(UI_F_SMALL));
    l_wday = ui_label(t, UI_F_SMALL, UI_C_TEXT, "");
    lv_obj_set_pos(l_wday, ui_inset_l(UI_HEAD_Y, UI_HEAD_Y + lh), UI_HEAD_Y);
    l_date = ui_label(t, UI_F_SMALL, UI_C_DIM, "");
    lv_obj_set_pos(l_date, UI_SAFE_X + 6, UI_HEAD_Y + 18);
    l_alarm = ui_label(t, UI_F_SMALL, UI_C_ACCENT, "");
    lv_obj_align(l_alarm, LV_ALIGN_TOP_RIGHT, -ui_inset_r(UI_HEAD_Y, UI_HEAD_Y + lh), UI_HEAD_Y);
    l_bat = ui_label(t, UI_F_SMALL, UI_C_DIM, "");
    lv_obj_align(l_bat, LV_ALIGN_TOP_RIGHT, -UI_SAFE_X - 6, UI_HEAD_Y + 18);
    l_time = clock_label(t, UI_C_TEXT);
    lv_obj_set_pos(l_time, UI_SAFE_X, 58); /* the big digits draw above the label box */
    ui_rule(t, UI_SAFE_X + 6, 146, UI_W - 2 * UI_SAFE_X - 12);
    lv_obj_t *h = ui_label(t, UI_F_SMALL, UI_C_DIM, ui_tr("Âm lịch", "Lunar"));
    lv_obj_set_pos(h, UI_SAFE_X + 6, 150);
    l_lunar = ui_label(t, UI_F_TITLE, UI_C_TEXT, "");
    lv_obj_set_pos(l_lunar, UI_SAFE_X + 6, 166);
    lv_obj_set_width(l_lunar, 124);
    ui_dot(l_lunar);
    l_lunar2 = ui_label(t, UI_F_SMALL, UI_C_TEXT, "");
    lv_obj_set_pos(l_lunar2, UI_SAFE_X + 6, 190);
    lv_obj_set_width(l_lunar2, 124);
    ui_dot(l_lunar2);
    l_wx_head = ui_label(t, UI_F_SMALL, UI_C_DIM, "");
    lv_obj_set_pos(l_wx_head, 150, 150);
    lv_obj_set_width(l_wx_head, 68);
    ui_dot(l_wx_head);
    l_wx = ui_label(t, UI_F_TITLE, UI_C_TEXT, "");
    lv_obj_set_pos(l_wx, 150, 166);
    lv_obj_set_width(l_wx, 68);
    ui_dot(l_wx);
    l_wx2 = ui_label(t, UI_F_SMALL, UI_C_TEXT, "");
    lv_obj_set_pos(l_wx2, 150, 190);
    lv_obj_set_width(l_wx2, 68);
    ui_dot(l_wx2);
    cat_mini_attach(t, 222, 138, 2); /* raised: the bottom-right corner is rounded off */
}

static void build_center(lv_obj_t *t) /* Cyberpunk */
{
    const int lh = lv_font_get_line_height(ui_f(UI_F_SMALL));
    l_net = ui_label(t, UI_F_SMALL, UI_C_OK, "");
    lv_obj_set_pos(l_net, ui_inset_l(UI_HEAD_Y - 2, UI_HEAD_Y - 2 + lh), UI_HEAD_Y - 2);
    l_alarm = ui_label(t, UI_F_SMALL, UI_C_WARN, "");
    lv_obj_align(l_alarm, LV_ALIGN_TOP_MID, 0, UI_HEAD_Y - 2);
    l_bat = ui_label(t, UI_F_SMALL, UI_C_DIM, "");
    lv_obj_align(l_bat, LV_ALIGN_TOP_RIGHT, -ui_inset_r(UI_HEAD_Y - 2, UI_HEAD_Y - 2 + lh), UI_HEAD_Y - 2);
    ui_hud_corners(t, 18, 30, UI_W - 36, 86);
    if (ui_theme()->fx_aberration && ui_fx() && !ui_is_night()) {
        l_g1 = clock_label(t, UI_C_ACCENT2);
        lv_obj_align(l_g1, LV_ALIGN_TOP_MID, -1, 30);
        lv_obj_set_style_text_opa(l_g1, LV_OPA_80, 0);
        l_g2 = clock_label(t, UI_C_TEXT);
        lv_obj_align(l_g2, LV_ALIGN_TOP_MID, 1, 30);
        lv_obj_set_style_text_opa(l_g2, LV_OPA_40, 0);
    }
    l_time = clock_label(t, UI_C_ACCENT);
    lv_obj_align(l_time, LV_ALIGN_TOP_MID, 0, 30);
    l_date = ui_label(t, UI_F_BODY, UI_C_TEXT, "");
    lv_obj_set_pos(l_date, UI_SAFE_X + 8, 120);
    lv_obj_set_width(l_date, UI_W - 2 * UI_SAFE_X - 16);
    ui_dot(l_date);
    l_lunar = ui_label(t, UI_F_SMALL, UI_C_WARN, "");
    lv_obj_set_pos(l_lunar, UI_SAFE_X + 8, 146);
    lv_obj_set_width(l_lunar, 190);
    ui_dot(l_lunar);
    l_wx = ui_label(t, UI_F_SMALL, UI_C_DIM, "");
    lv_obj_set_pos(l_wx, UI_SAFE_X + 8, 166);
    lv_obj_set_width(l_wx, 186);
    ui_dot(l_wx);
    l_term = ui_label(t, UI_F_SMALL, UI_C_DIM, "");
    lv_obj_set_pos(l_term, UI_SAFE_X + 8, 184);
    lv_obj_set_width(l_term, 186);
    ui_dot(l_term);
    cat_mini_attach(t, 212, 146, 2); /* clear of the rounded bottom-right corner */
}

static void build_enso(lv_obj_t *t) /* Zen */
{
    lv_obj_t *e = lv_image_create(t);
    lv_image_set_src(e, &ui_img_enso);
    lv_image_set_scale(e, 320); /* 150 px art drawn at ~188 px */
    lv_obj_set_size(e, 188, 188);
    lv_image_set_inner_align(e, LV_IMAGE_ALIGN_CENTER);
    lv_obj_set_pos(e, 4, 22);
    lv_obj_set_style_image_recolor(e, ui_lc(UI_C_TEXT), 0);
    lv_obj_set_style_image_recolor_opa(e, LV_OPA_COVER, 0);
    l_time = clock_label(t, UI_C_TEXT);
    lv_obj_set_width(l_time, 188);
    lv_obj_set_style_text_align(l_time, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(l_time, e, LV_ALIGN_CENTER, 0, 0);
    l_wday = ui_label(t, UI_F_SMALL, UI_C_DIM, "");
    lv_obj_set_pos(l_wday, 198, 34);
    l_date = ui_label(t, UI_F_BODY, UI_C_TEXT, "");
    lv_obj_set_pos(l_date, 198, 52);
    lv_obj_set_width(l_date, 86);
    ui_dot(l_date);
    l_lunar = ui_label(t, UI_F_SMALL, UI_C_ACCENT2, "");
    lv_obj_set_pos(l_lunar, 198, 80);
    lv_obj_t *r = ui_rule(t, 198, 124, 50);
    lv_obj_set_style_bg_color(r, ui_lc(UI_C_OK), 0);
    l_wx = ui_label(t, UI_F_SMALL, UI_C_DIM, "");
    lv_obj_set_pos(l_wx, 198, 130);
    l_alarm = ui_label(t, UI_F_SMALL, UI_C_DIM, "");
    lv_obj_set_pos(l_alarm, UI_SAFE_X + 14, 198);
    cat_mini_attach(t, 196, 158, 2);
    if (ui_f(UI_F_BODY) != &mb_font_20) { /* the Zen font carries the 猫 glyph */
        lv_obj_t *seal = plain_box(t, 254, 190, 22, 22, UI_C_ACCENT, 0);
        lv_obj_t *k = ui_label(seal, UI_F_BODY, UI_C_ON_ACCENT, "猫");
        lv_obj_center(k);
    }
}

static void build_block(lv_obj_t *t) /* Brutalism */
{
    ui_header(t, ui_tr("đồng_hồ", "clock"), "");
    lv_obj_t *blk = plain_box(t, 16, 30, 212, 90, UI_C_ACCENT, 3);
    if (ui_fx() && !ui_is_night()) {
        lv_obj_set_style_shadow_width(blk, 1, 0);
        lv_obj_set_style_shadow_offset_x(blk, 5, 0);
        lv_obj_set_style_shadow_offset_y(blk, 5, 0);
        lv_obj_set_style_shadow_color(blk, ui_lc(UI_C_LINE), 0);
    }
    l_time = clock_label(blk, UI_C_ON_ACCENT);
    lv_obj_center(l_time);
    lv_obj_t *pb = plain_box(t, 236, 32, 46, 46, UI_C_ALERT, 3);
    l_bat = ui_label(pb, UI_F_SMALL, UI_C_ON_ACCENT, ""); /* on the alert colour: black by day, on yellow at night */
    lv_obj_set_pos(l_bat, 3, 2);
    lv_obj_t *col = lv_obj_create(t);
    lv_obj_remove_style_all(col);
    lv_obj_set_pos(col, UI_SAFE_X + 6, 132);
    lv_obj_set_size(col, 186, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(col, ui_lc(UI_C_BG), 0);
    lv_obj_set_style_bg_opa(col, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    l_date = ui_label(col, UI_F_TITLE, UI_C_TEXT, "");
    l_lunar = ui_label(col, UI_F_SMALL, UI_C_TEXT, "");
    lv_obj_set_width(l_lunar, 186);
    ui_dot(l_lunar);
    l_wx = ui_label(col, UI_F_SMALL, UI_C_TEXT, "");
    lv_obj_set_width(l_wx, 186);
    ui_dot(l_wx);
    l_alarm = ui_label(col, UI_F_SMALL, UI_C_ACCENT2, "");
    lv_obj_set_style_text_decor(l_alarm, LV_TEXT_DECOR_UNDERLINE, 0);
    lv_obj_t *cb = plain_box(t, 210, 138, 70, 66, UI_C_OK, 2); /* clear of the rounded bottom-right corner */
    cat_mini_attach(cb, 2, 0, 2);
}

static void open_cat(lv_event_t *e) { ui_show_tile(UI_TILE_CAT, true); }

static void clock_build(lv_obj_t *t)
{
    l_time = l_g1 = l_g2 = l_wday = l_date = l_lunar = l_lunar2 = l_wx = l_wx2 = l_wx_head = NULL;
    l_alarm = l_bat = l_net = l_term = NULL;
    switch (ui_theme()->clock) {
    case THEME_CLOCK_CENTER: build_center(t); break;
    case THEME_CLOCK_ENSO: build_enso(t); break;
    case THEME_CLOCK_BLOCK: build_block(t); break;
    default: build_left(t); break;
    }
    lv_obj_t *hit = lv_obj_create(t); /* tap the cat corner to open the cat tile */
    lv_obj_remove_style_all(hit);
    lv_obj_set_size(hit, 76, 64);
    lv_obj_align(hit, LV_ALIGN_BOTTOM_RIGHT, -UI_SAFE_X + 4, -UI_SAFE_Y - 16);
    lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(hit, open_cat, LV_EVENT_SHORT_CLICKED, NULL);
}

void clock_app_init(void)
{
    ui_register_builder(UI_TILE_CLOCK, clock_build);
    lv_timer_create(clock_tick, 250, NULL);
    settings_on_change(on_settings, NULL);
    web_set_config_hook("weather", weather_cfg_hook);
    xTaskCreate(weather_task, "weather", 6144, NULL, 3, &s_wx_task);
}
