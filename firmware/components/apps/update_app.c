/* Firmware updates from GitHub Releases (docs/firmware_v0.2.0.md, 3; web/API.md "Update").
 *
 * The speaker reads manifest.json of the latest release (CONFIG_MEOWBOX_UPDATE_MANIFEST_URL, a
 * releases/latest/download/ link that GitHub redirects to the newest release). By default only when asked from the web
 * page; settings "update.auto" adds a check 2 min after Wi-Fi comes up and then every 24 h.
 * Installing downloads app.url with esp_https_ota into the free OTA slot, checks size and SHA-256 of what landed in
 * flash against the manifest, then sets the boot slot and restarts. The bootloader rolls back if the new image does not
 * mark itself valid; the next boot reports that through "last". The web UI is inside the app image (3.1), so one file
 * updates both.
 */
#include "apps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_system.h"
#include "psa/crypto.h"
#include "board.h"
#include "net.h"
#include "player.h"
#include "settings.h"
#include "ui.h"

static const char *TAG = "update";

#define PENDING_FILE "/data/update.json" /* {"to":"0.2.1","from":"0.2.0"} while an install is in flight */
#define MIN_BATTERY 30

typedef enum { U_IDLE, U_CHECK, U_DOWNLOAD, U_VERIFY, U_REBOOT, U_ERROR } ustate_t;
static const char *const STATE_STR[] = {"idle", "check", "download", "verify", "reboot", "error"};

static SemaphoreHandle_t s_lock;
static volatile ustate_t s_state;
static volatile int s_pct;
static char s_err[24];                 /* last check/install error */
static struct {
    bool valid;                        /* a manifest has been read */
    char ver[24], date[16], url[256], page[160];
    uint32_t size;
    uint8_t sha[32];
    int proto;
    bool too_old;                      /* running version < min_ver: needs USB */
    cJSON *notes;                      /* {"vi":[...],"en":[...]} */
    time_t checked;
} s_m;
static struct { char ver[24], err[16]; } s_last; /* outcome of the install before this boot */
static int64_t s_online_since;

/* ---------- helpers ---------- */
static const char *cur_ver(void) { return esp_app_get_description()->version; }

/* "0.10.2" > "0.9.7": compare up to three numbers; anything after them is ignored */
static int ver_cmp(const char *a, const char *b)
{
    int x[3] = {0}, y[3] = {0};
    sscanf(a, "%d.%d.%d", &x[0], &x[1], &x[2]);
    sscanf(b, "%d.%d.%d", &y[0], &y[1], &y[2]);
    for (int i = 0; i < 3; i++)
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

static bool newer(void) { return s_m.valid && ver_cmp(s_m.ver, cur_ver()) > 0; }

static int hex2bin(const char *h, uint8_t *out, int n)
{
    for (int i = 0; i < n; i++) {
        unsigned v;
        if (!h[2 * i] || !h[2 * i + 1] || sscanf(h + 2 * i, "%2x", &v) != 1) return -1;
        out[i] = v;
    }
    return 0;
}

static void set_error(const char *e)
{
    snprintf(s_err, sizeof(s_err), "%s", e);
    s_state = U_ERROR;
    ESP_LOGW(TAG, "error: %s", e);
}

/* ---------- check ---------- */
static char *http_get_small(const char *url, int max, int *status)
{
    *status = 0;
    esp_http_client_config_t cfg = {.url = url, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 10000,
                                    .buffer_size = 2048, .max_redirection_count = 5, .user_agent = "MeowBox"};
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return NULL;
    char *buf = NULL;
    for (int r = 0; r < 6; r++) { /* follow redirects by hand: open/fetch_headers does not */
        if (esp_http_client_open(c, 0) != ESP_OK) goto out;
        esp_http_client_fetch_headers(c);
        int st = esp_http_client_get_status_code(c);
        *status = st;
        if (st >= 300 && st < 400) {
            esp_http_client_set_redirection(c);
            esp_http_client_close(c);
            continue;
        }
        if (st != 200) goto out;
        buf = heap_caps_calloc(1, max + 1, MALLOC_CAP_SPIRAM);
        int n = 0, k;
        while (buf && n < max && (k = esp_http_client_read(c, buf + n, max - n)) > 0) n += k;
        break;
    }
out:
    esp_http_client_cleanup(c);
    return buf;
}

static bool parse_manifest(const char *txt)
{
    cJSON *j = cJSON_Parse(txt);
    if (!j) return false;
    const char *ver = cJSON_GetStringValue(cJSON_GetObjectItem(j, "ver"));
    cJSON *app = cJSON_GetObjectItem(j, "app");
    const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(app, "url"));
    const char *sha = cJSON_GetStringValue(cJSON_GetObjectItem(app, "sha256"));
    cJSON *size = cJSON_GetObjectItem(app, "size");
    uint8_t digest[32];
    bool ok = ver && url && sha && cJSON_IsNumber(size) && strlen(sha) == 64 && hex2bin(sha, digest, 32) == 0 &&
              strlen(url) < sizeof(s_m.url) && size->valuedouble > 0;
    if (ok) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_m.valid = true;
        snprintf(s_m.ver, sizeof(s_m.ver), "%s", ver);
        snprintf(s_m.url, sizeof(s_m.url), "%s", url);
        const char *d = cJSON_GetStringValue(cJSON_GetObjectItem(j, "date"));
        snprintf(s_m.date, sizeof(s_m.date), "%s", d ? d : "");
        const char *pg = cJSON_GetStringValue(cJSON_GetObjectItem(j, "page"));
        snprintf(s_m.page, sizeof(s_m.page), "%s", pg ? pg : "");
        s_m.size = (uint32_t)size->valuedouble;
        memcpy(s_m.sha, digest, 32);
        cJSON *pr = cJSON_GetObjectItem(j, "proto");
        s_m.proto = cJSON_IsNumber(pr) ? pr->valueint : 0;
        const char *mv = cJSON_GetStringValue(cJSON_GetObjectItem(j, "min_ver"));
        s_m.too_old = mv && ver_cmp(cur_ver(), mv) < 0;
        cJSON_Delete(s_m.notes);
        s_m.notes = cJSON_DetachItemFromObject(j, "notes");
        s_m.checked = time(NULL);
        xSemaphoreGive(s_lock);
    }
    cJSON_Delete(j);
    return ok;
}

static void check_task(void *arg)
{
    s_err[0] = 0;
    int st = 0;
    char *txt = wifi_mgr_connected() ? http_get_small(CONFIG_MEOWBOX_UPDATE_MANIFEST_URL, 16384, &st) : NULL;
    if (!txt) set_error(!wifi_mgr_connected() ? "offline" : st == 404 ? "no_release" : "manifest_unreachable");
    else if (!parse_manifest(txt)) set_error("bad_manifest");
    else {
        s_state = U_IDLE;
        ESP_LOGI(TAG, "latest %s, running %s%s", s_m.ver, cur_ver(), newer() ? " -> update available" : "");
    }
    free(txt);
    vTaskDeleteWithCaps(NULL);
}

static bool start_check(void)
{
    if (s_state == U_CHECK || s_state == U_DOWNLOAD || s_state == U_VERIFY || s_state == U_REBOOT) return false;
    s_state = U_CHECK;
    /* network only, no flash writes: the stack can live in PSRAM */
    if (xTaskCreatePinnedToCoreWithCaps(check_task, "upd_check", 6144, NULL, 3, NULL, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
        set_error("no_mem");
        return false;
    }
    return true;
}

/* ---------- install ---------- */
static bool verify_written(const esp_partition_t *p, uint32_t len)
{
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) return false;
    uint8_t *buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    bool ok = buf != NULL;
    for (uint32_t off = 0; ok && off < len; off += 4096) {
        uint32_t n = len - off < 4096 ? len - off : 4096;
        ok = esp_partition_read(p, off, buf, n) == ESP_OK && psa_hash_update(&op, buf, n) == PSA_SUCCESS;
    }
    free(buf);
    uint8_t h[32];
    size_t hl = 0;
    if (!ok || psa_hash_finish(&op, h, sizeof(h), &hl) != PSA_SUCCESS) {
        psa_hash_abort(&op);
        return false;
    }
    return hl == 32 && memcmp(h, s_m.sha, 32) == 0;
}

static void show_on_screen(const char *title, const char *text, ui_color_t c, int secs)
{
    if (board_lvgl_lock(1000)) {
        ui_overlay_show(LV_SYMBOL_DOWNLOAD, title, text, c, secs, NULL, NULL);
        board_lvgl_unlock();
    }
}

static void install_task(void *arg)
{
    char ver[24], url[256];
    uint32_t size = s_m.size;
    snprintf(ver, sizeof(ver), "%s", s_m.ver);
    snprintf(url, sizeof(url), "%s", s_m.url);
    ESP_LOGI(TAG, "installing %s from %s (%lu bytes)", ver, url, (unsigned long)size);
    wifi_mgr_hold_awake(WIFI_HOLD_OTA, true);
    player_stop();
    char msg[64];
    snprintf(msg, sizeof(msg), "%s %s", ui_tr("Đang cập nhật lên", "Updating to"), ver);
    show_on_screen(msg, ui_tr("Đừng tắt loa, khoảng 1–2 phút.", "Keep the speaker on, about 1-2 min."), UI_C_ACCENT, 600);

    char pend[96];
    int pl = snprintf(pend, sizeof(pend), "{\"to\":\"%s\",\"from\":\"%s\"}", ver, cur_ver());
    settings_write_file(PENDING_FILE, pend, pl);

    esp_http_client_config_t http = {.url = url, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 15000,
                                     .buffer_size = 4096, .buffer_size_tx = 1024, .keep_alive_enable = true,
                                     .max_redirection_count = 5, .user_agent = "MeowBox"};
    esp_https_ota_config_t cfg = {.http_config = &http};
    esp_https_ota_handle_t h = NULL;
    esp_err_t r = esp_https_ota_begin(&cfg, &h);
    if (r != ESP_OK) {
        set_error("download");
        goto fail;
    }
    while ((r = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int got = esp_https_ota_get_image_len_read(h);
        s_pct = size ? (int)((int64_t)got * 100 / size) : 0;
        if (s_pct > 99) s_pct = 99;
    }
    if (r != ESP_OK || !esp_https_ota_is_complete_data_received(h)) {
        set_error("download");
        esp_https_ota_abort(h);
        goto fail;
    }
    s_state = U_VERIFY;
    int got = esp_https_ota_get_image_len_read(h);
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if ((uint32_t)got != size || !part || !verify_written(part, size)) {
        set_error("bad_hash");
        esp_https_ota_abort(h);
        goto fail;
    }
    if (esp_https_ota_finish(h) != ESP_OK) { /* validates the image and sets the boot slot */
        set_error("bad_image");
        goto fail;
    }
    s_pct = 100;
    s_state = U_REBOOT;
    ESP_LOGI(TAG, "%s written and verified, restarting", ver);
    vTaskDelay(pdMS_TO_TICKS(1500)); /* lets the web page see "reboot" */
    esp_restart();

fail:
    remove(PENDING_FILE);
    wifi_mgr_hold_awake(WIFI_HOLD_OTA, false);
    snprintf(msg, sizeof(msg), "%s (%s)", ui_tr("Cập nhật lỗi", "Update failed"), s_err);
    show_on_screen(msg, ui_tr("Loa vẫn chạy bản cũ.", "The speaker keeps the current version."), UI_C_ALERT, 10);
    vTaskDelete(NULL);
}

/* ---------- web API ---------- */
static cJSON *info_json(void)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "cur", cur_ver());
    cJSON_AddStringToObject(r, "state", STATE_STR[s_state]);
    cJSON_AddNumberToObject(r, "pct", s_pct);
    cJSON_AddStringToObject(r, "err", s_err);
    cJSON_AddBoolToObject(r, "auto", settings_get_bool("update", "auto", false));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON_AddNumberToObject(r, "checked", s_m.valid ? (double)s_m.checked : 0);
    if (s_m.valid) {
        cJSON_AddStringToObject(r, "latest", s_m.ver);
        if (newer()) {
            cJSON_AddStringToObject(r, "new", s_m.ver);
            cJSON_AddStringToObject(r, "date", s_m.date);
            cJSON_AddNumberToObject(r, "size", s_m.size);
            cJSON_AddNumberToObject(r, "proto", s_m.proto);
            cJSON_AddBoolToObject(r, "usb_only", s_m.too_old);
            if (s_m.notes) cJSON_AddItemToObject(r, "notes", cJSON_Duplicate(s_m.notes, true));
        }
        if (s_m.page[0]) cJSON_AddStringToObject(r, "page", s_m.page);
    }
    xSemaphoreGive(s_lock);
    if (s_last.ver[0]) {
        cJSON *l = cJSON_AddObjectToObject(r, "last");
        cJSON_AddStringToObject(l, "ver", s_last.ver);
        cJSON_AddStringToObject(l, "err", s_last.err);
    }
    return r;
}

static esp_err_t h_get(httpd_req_t *req) { return web_send_json(req, info_json()); }

static esp_err_t h_check(httpd_req_t *req)
{
    cJSON *j = web_read_json(req);
    if (!j) return ESP_OK;
    cJSON_Delete(j);
    if (!wifi_mgr_connected()) return web_send_err(req, 409, "offline");
    if (!start_check() && s_state != U_CHECK) return web_send_err(req, 409, "busy");
    return web_send_json(req, info_json());
}

static esp_err_t h_install(httpd_req_t *req)
{
    cJSON *j = web_read_json(req);
    if (!j) return ESP_OK;
    const char *ver = cJSON_GetStringValue(cJSON_GetObjectItem(j, "ver"));
    char v[24];
    snprintf(v, sizeof(v), "%s", ver ? ver : "");
    cJSON_Delete(j);
    if (s_state == U_CHECK || s_state == U_DOWNLOAD || s_state == U_VERIFY || s_state == U_REBOOT)
        return web_send_err(req, 409, "busy");
    if (!newer() || strcmp(v, s_m.ver) != 0) return web_send_err(req, 409, "stale");
    if (s_m.too_old) return web_send_err(req, 409, "usb_only");
    if (battery_pct() < MIN_BATTERY && !board_is_charging()) return web_send_err(req, 409, "low_battery");
    s_err[0] = 0;
    s_pct = 0;
    s_state = U_DOWNLOAD;
    /* writes flash: internal-RAM stack */
    if (xTaskCreate(install_task, "upd_install", 8192, NULL, 4, NULL) != pdPASS) {
        set_error("no_mem");
        return web_send_err(req, 500, "no_mem");
    }
    return web_send_json(req, info_json());
}

void update_status_hook(cJSON *r)
{
    if (newer()) cJSON_AddStringToObject(r, "upd", s_m.ver);
}

const char *update_available(void) { return newer() ? s_m.ver : NULL; }

/* ---------- auto check (opt-in) ---------- */
static void auto_tick(void *arg)
{
    if (!wifi_mgr_connected()) {
        s_online_since = 0;
        return;
    }
    int64_t now = esp_timer_get_time();
    if (!s_online_since) s_online_since = now;
    if (!settings_get_bool("update", "auto", false)) return;
    static int64_t last_check;
    if ((last_check == 0 && now - s_online_since > 120LL * 1000000) ||
        (last_check && now - last_check > 24LL * 3600 * 1000000)) {
        last_check = now;
        start_check();
    }
}

void update_app_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    /* outcome of an install started before this boot */
    size_t n = 0;
    char *p = settings_read_file(PENDING_FILE, &n);
    if (p) {
        cJSON *j = cJSON_Parse(p);
        const char *to = cJSON_GetStringValue(cJSON_GetObjectItem(j, "to"));
        if (to) {
            snprintf(s_last.ver, sizeof(s_last.ver), "%s", to);
            bool ok = strcmp(to, cur_ver()) == 0;
            snprintf(s_last.err, sizeof(s_last.err), "%s", ok ? "" : "rollback");
            ESP_LOGW(TAG, "update to %s %s", to, ok ? "succeeded" : "did not stick: rolled back");
        }
        cJSON_Delete(j);
        free(p);
        remove(PENDING_FILE);
    }
    web_register("/api/update", HTTP_GET, h_get, WEB_AUTH_SESSION);
    web_register("/api/update", HTTP_POST, h_install, WEB_AUTH_SESSION);
    web_register("/api/update/check", HTTP_POST, h_check, WEB_AUTH_SESSION);
    esp_timer_create_args_t a = {.callback = auto_tick, .name = "upd_auto"};
    esp_timer_handle_t t;
    if (esp_timer_create(&a, &t) == ESP_OK) esp_timer_start_periodic(t, 60LL * 1000000);
}
