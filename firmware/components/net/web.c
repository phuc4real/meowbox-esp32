#include "net.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "psa/crypto.h"
#include "nvs.h"
#include "settings.h"
#include "webui.h"

static const char *TAG = "web";

#define AUTH_FILE "/data/auth.json"
#define DEFAULT_USER "meowbox"
#define DEFAULT_PASS "m3owb0x2026"
#define LEGACY_DEFAULT_PASS "meowbox" /* default before 2026-11-23; migrated to DEFAULT_PASS on boot */
#define MAX_SESS 4
#define SESS_TTL_US (24LL * 3600 * 1000000)
#define MAX_BODY 8192

static httpd_handle_t s_srv;
static web_json_hook_t s_status_hook;
static struct { char group[12]; web_json_hook_t hook; } s_cfg_hooks[8];
static struct { char group[12]; web_put_hook_t hook; } s_put_hooks[4];

/* ================= helpers ================= */
esp_err_t web_send_json(httpd_req_t *req, cJSON *json)
{
    char *txt = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!txt) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t r = httpd_resp_send(req, txt, HTTPD_RESP_USE_STRLEN);
    cJSON_free(txt);
    return r;
}

esp_err_t web_send_ok(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

esp_err_t web_send_err(httpd_req_t *req, int status, const char *code)
{
    static const struct { int s; const char *t; } st[] = {
        {400, "400 Bad Request"}, {401, "401 Unauthorized"}, {403, "403 Forbidden"}, {404, "404 Not Found"},
        {413, "413 Payload Too Large"}, {429, "429 Too Many Requests"}, {500, "500 Internal Server Error"},
        {503, "503 Service Unavailable"}};
    const char *line = "500 Internal Server Error";
    for (size_t i = 0; i < sizeof(st) / sizeof(st[0]); i++)
        if (st[i].s == status) line = st[i].t;
    char body[48];
    snprintf(body, sizeof(body), "{\"err\":\"%s\"}", code);
    httpd_resp_set_status(req, line);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

cJSON *web_read_json(httpd_req_t *req)
{
    if (req->content_len > MAX_BODY) {
        web_send_err(req, 413, "too_large");
        return NULL;
    }
    char *buf = malloc(req->content_len + 1);
    int got = 0;
    while (got < (int)req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            free(buf);
            return NULL;
        }
        got += r;
    }
    buf[got] = 0;
    cJSON *j = got ? cJSON_Parse(buf) : cJSON_CreateObject();
    free(buf);
    if (!j || !cJSON_IsObject(j)) {
        cJSON_Delete(j);
        web_send_err(req, 400, "bad_json");
        return NULL;
    }
    return j;
}

static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') *o++ = ' ';
        else if (*s == '%' && isxdigit((int)s[1]) && isxdigit((int)s[2])) {
            char h[3] = {s[1], s[2], 0};
            *o++ = (char)strtol(h, NULL, 16);
            s += 2;
        } else *o++ = *s;
    }
    *o = 0;
}

bool web_query(httpd_req_t *req, const char *key, char *out, size_t len)
{
    size_t ql = httpd_req_get_url_query_len(req);
    if (!ql) return false;
    char *q = malloc(ql + 1);
    bool ok = httpd_req_get_url_query_str(req, q, ql + 1) == ESP_OK && httpd_query_key_value(q, key, out, len) == ESP_OK;
    free(q);
    if (ok) url_decode(out);
    return ok;
}

void web_set_status_hook(web_json_hook_t hook) { s_status_hook = hook; }

void web_set_put_hook(const char *group, web_put_hook_t hook)
{
    for (int i = 0; i < 4; i++) {
        if (!s_put_hooks[i].hook) {
            snprintf(s_put_hooks[i].group, sizeof(s_put_hooks[i].group), "%s", group);
            s_put_hooks[i].hook = hook;
            return;
        }
    }
}

void web_set_config_hook(const char *group, web_json_hook_t hook)
{
    for (int i = 0; i < 8; i++) {
        if (!s_cfg_hooks[i].hook) {
            snprintf(s_cfg_hooks[i].group, sizeof(s_cfg_hooks[i].group), "%s", group);
            s_cfg_hooks[i].hook = hook;
            return;
        }
    }
}

/* ================= auth ================= */
typedef struct { char id[33]; int64_t last; } sess_t;
static sess_t s_sess[MAX_SESS];
static char s_salt[33], s_hash[65];
static bool s_def_pw;
static int s_fail_count;
static int64_t s_lock_until;

static void hex(const uint8_t *in, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) sprintf(out + i * 2, "%02x", in[i]);
}

static void random_hex(char *out, size_t bytes)
{
    uint8_t b[32];
    esp_fill_random(b, bytes);
    hex(b, bytes, out);
}

static void hash_pw(const char *salt, const char *pw, char out[65])
{
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "%s%s", salt, pw);
    uint8_t h[32];
    size_t hl = 0;
    psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)buf, n, h, sizeof(h), &hl);
    hex(h, 32, out);
}

static void auth_save(void)
{
    char txt[160];
    int n = snprintf(txt, sizeof(txt), "{\"salt\":\"%s\",\"hash\":\"%s\"}", s_salt, s_hash);
    settings_write_file(AUTH_FILE, txt, n);
}

static void auth_set_password(const char *pw)
{
    random_hex(s_salt, 16);
    hash_pw(s_salt, pw, s_hash);
    s_def_pw = strcmp(pw, DEFAULT_PASS) == 0;
    auth_save();
}

static void auth_init(void)
{
    psa_crypto_init();
    char *txt = settings_read_file(AUTH_FILE, NULL);
    cJSON *j = txt ? cJSON_Parse(txt) : NULL;
    free(txt);
    const char *salt = cJSON_GetStringValue(cJSON_GetObjectItem(j, "salt"));
    const char *hash = cJSON_GetStringValue(cJSON_GetObjectItem(j, "hash"));
    if (salt && hash && strlen(salt) < sizeof(s_salt) && strlen(hash) == 64) {
        strcpy(s_salt, salt);
        strcpy(s_hash, hash);
        char h[65];
        hash_pw(s_salt, LEGACY_DEFAULT_PASS, h);
        if (strcmp(h, s_hash) == 0) {
            ESP_LOGW(TAG, "old default password found, switching to the new default");
            auth_set_password(DEFAULT_PASS);
        } else {
            hash_pw(s_salt, DEFAULT_PASS, h);
            s_def_pw = strcmp(h, s_hash) == 0;
        }
    } else {
        auth_set_password(DEFAULT_PASS);
    }
    cJSON_Delete(j);

    /* notify token: generate on first boot */
    char tok[24];
    settings_get_str("notify", "token", tok, sizeof(tok), "");
    if (!tok[0]) {
        static const char al[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
        for (int i = 0; i < 16; i++) tok[i] = al[esp_random() % (sizeof(al) - 1)];
        tok[16] = 0;
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "token", tok);
        settings_update("notify", p);
        cJSON_Delete(p);
    }
}

static bool auth_check_pw(const char *pw)
{
    char h[65];
    hash_pw(s_salt, pw, h);
    return strcmp(h, s_hash) == 0;
}

static sess_t *find_session(httpd_req_t *req)
{
    char cookie[40];
    size_t l = sizeof(cookie);
    if (httpd_req_get_cookie_val(req, "sid", cookie, &l) != ESP_OK) return NULL;
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < MAX_SESS; i++) {
        if (s_sess[i].id[0] && strcmp(s_sess[i].id, cookie) == 0) {
            if (now - s_sess[i].last > SESS_TTL_US) {
                s_sess[i].id[0] = 0;
                return NULL;
            }
            s_sess[i].last = now;
            return &s_sess[i];
        }
    }
    return NULL;
}

static bool token_ok(httpd_req_t *req)
{
    char tok[24], got[80];
    settings_get_str("notify", "token", tok, sizeof(tok), "");
    if (!tok[0]) return false;
    size_t l = httpd_req_get_hdr_value_len(req, "Authorization");
    if (l && l < sizeof(got) && httpd_req_get_hdr_value_str(req, "Authorization", got, sizeof(got)) == ESP_OK) {
        if (strncmp(got, "Bearer ", 7) == 0 && strcmp(got + 7, tok) == 0) return true;
    }
    char q[24];
    return web_query(req, "token", q, sizeof(q)) && strcmp(q, tok) == 0;
}

/* Handler wrapper storing the auth mode in user_ctx */
typedef struct { esp_err_t (*fn)(httpd_req_t *); web_auth_t auth; } route_t;

static esp_err_t route_dispatch(httpd_req_t *req)
{
    route_t *rt = req->user_ctx;
    if (rt->auth == WEB_AUTH_SESSION && !find_session(req)) return web_send_err(req, 401, "auth");
    if (rt->auth == WEB_AUTH_TOKEN && !token_ok(req) && !find_session(req)) return web_send_err(req, 401, "auth");
    return rt->fn(req);
}

esp_err_t web_register(const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *), web_auth_t auth)
{
    route_t *rt = malloc(sizeof(route_t));
    rt->fn = handler;
    rt->auth = auth;
    httpd_uri_t u = {.uri = uri, .method = method, .handler = route_dispatch, .user_ctx = rt};
    esp_err_t r = httpd_register_uri_handler(s_srv, &u);
    if (r != ESP_OK) ESP_LOGE(TAG, "register %s failed: %s", uri, esp_err_to_name(r));
    return r;
}

esp_err_t web_register_ws(const char *uri, esp_err_t (*handler)(httpd_req_t *))
{
    httpd_uri_t u = {.uri = uri, .method = HTTP_GET, .handler = handler, .is_websocket = true,
                     .handle_ws_control_frames = false};
    return httpd_register_uri_handler(s_srv, &u);
}

httpd_handle_t web_server(void) { return s_srv; }

/* ================= session endpoints ================= */
static esp_err_t h_login(httpd_req_t *req)
{
    int64_t now = esp_timer_get_time();
    if (now < s_lock_until) return web_send_err(req, 429, "too_many");
    cJSON *j = web_read_json(req);
    if (!j) return ESP_OK;
    const char *user = cJSON_GetStringValue(cJSON_GetObjectItem(j, "user"));
    const char *pass = cJSON_GetStringValue(cJSON_GetObjectItem(j, "pass"));
    bool ok = user && pass && strcmp(user, DEFAULT_USER) == 0 && auth_check_pw(pass);
    cJSON_Delete(j);
    if (!ok) {
        if (++s_fail_count >= 5) {
            s_fail_count = 0;
            s_lock_until = now + 30LL * 1000000;
        }
        return web_send_err(req, 401, "auth");
    }
    s_fail_count = 0;
    int slot = 0;
    for (int i = 0; i < MAX_SESS; i++) {
        if (!s_sess[i].id[0]) { slot = i; break; }
        if (s_sess[i].last < s_sess[slot].last) slot = i;
    }
    random_hex(s_sess[slot].id, 16);
    s_sess[slot].last = now;
    char hdr[96];
    snprintf(hdr, sizeof(hdr), "sid=%s; Path=/; HttpOnly; SameSite=Strict", s_sess[slot].id);
    httpd_resp_set_hdr(req, "Set-Cookie", hdr);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddBoolToObject(r, "def_pw", s_def_pw);
    return web_send_json(req, r);
}

static esp_err_t h_logout(httpd_req_t *req)
{
    sess_t *s = find_session(req);
    if (s) s->id[0] = 0;
    httpd_resp_set_hdr(req, "Set-Cookie", "sid=; Path=/; Max-Age=0");
    return web_send_ok(req);
}

static esp_err_t h_session(httpd_req_t *req)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "user", DEFAULT_USER);
    cJSON_AddBoolToObject(r, "def_pw", s_def_pw);
    return web_send_json(req, r);
}

static esp_err_t h_password(httpd_req_t *req)
{
    cJSON *j = web_read_json(req);
    if (!j) return ESP_OK;
    const char *old = cJSON_GetStringValue(cJSON_GetObjectItem(j, "old"));
    const char *nw = cJSON_GetStringValue(cJSON_GetObjectItem(j, "new"));
    esp_err_t r;
    if (!old || !auth_check_pw(old)) r = web_send_err(req, 403, "bad_old");
    else if (!nw || strlen(nw) < 6 || strlen(nw) > 32) r = web_send_err(req, 400, "bad_value");
    else {
        auth_set_password(nw);
        sess_t *me = find_session(req);
        for (int i = 0; i < MAX_SESS; i++)
            if (&s_sess[i] != me) s_sess[i].id[0] = 0;
        r = web_send_ok(req);
    }
    cJSON_Delete(j);
    return r;
}

/* ================= ping (no auth) ================= */
/* GET /api/ping: lets the PC agent check that an IP really is a MeowBox, and which one, before using it. */
static esp_err_t h_ping(httpd_req_t *req)
{
    uint8_t m[6];
    char mac[18];
    esp_read_mac(m, ESP_MAC_WIFI_STA);
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "name", wifi_mgr_ap_ssid());
    cJSON_AddStringToObject(r, "host", wifi_mgr_hostname());
    cJSON_AddStringToObject(r, "ver", esp_app_get_description()->version);
    cJSON_AddNumberToObject(r, "proto", MB_PROTO);
    cJSON_AddNumberToObject(r, "proto_min", MB_PROTO_MIN);
    cJSON_AddStringToObject(r, "mac", mac); /* for a DHCP reservation on the router */
    return web_send_json(req, r);
}

/* ================= status / config ================= */
static esp_err_t h_status(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    char ssid[33], ip[16], build[24];
    int rssi;
    wifi_mgr_info(ssid, sizeof(ssid), ip, sizeof(ip), &rssi);
    snprintf(build, sizeof(build), "%s %.5s", app->date, app->time);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "ver", app->version);
    cJSON_AddStringToObject(r, "build", build);
    char elf[10];
    esp_app_get_elf_sha256(elf, sizeof(elf)); /* changes with every build, unlike the compile time above */
    cJSON_AddStringToObject(r, "elf", elf);
    cJSON_AddNumberToObject(r, "up", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(r, "mode", wifi_mgr_mode_str());
    cJSON_AddStringToObject(r, "ssid", ssid);
    cJSON_AddStringToObject(r, "ip", ip);
    cJSON_AddNumberToObject(r, "rssi", rssi);
    cJSON_AddNumberToObject(r, "heap", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(r, "heap_min", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(r, "psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(r, "psram_tot", heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(r, "time", net_time_valid() ? (double)time(NULL) : 0);
    cJSON_AddBoolToObject(r, "def_pw", s_def_pw);
    /* defaults that feature modules override through the hook */
    cJSON_AddNumberToObject(r, "bat", -1);
    cJSON_AddBoolToObject(r, "chg", false);
    cJSON_AddBoolToObject(r, "usb", false);
    cJSON_AddNumberToObject(r, "vol", 0);
    cJSON_AddNumberToObject(r, "sleep", 0);
    cJSON_AddBoolToObject(r, "pc", false);
    cJSON *play = cJSON_AddObjectToObject(r, "play");
    cJSON_AddStringToObject(play, "st", "stop");
    cJSON_AddNumberToObject(play, "id", 0);
    cJSON_AddStringToObject(play, "name", "");
    cJSON_AddStringToObject(play, "title", "");
    if (s_status_hook) s_status_hook(r);
    return web_send_json(req, r);
}

/* GET /api/debug: memory and per-task numbers for tools/memwatch.py (docs/firmware_v0.2.0.md, 5.1).
 * Task "rt" is the raw run-time counter; CPU % = delta(rt) / delta(rt_total) between two calls. */
static void add_caps(cJSON *r, const char *name, uint32_t caps)
{
    cJSON *o = cJSON_AddObjectToObject(r, name);
    cJSON_AddNumberToObject(o, "free", heap_caps_get_free_size(caps));
    cJSON_AddNumberToObject(o, "min", heap_caps_get_minimum_free_size(caps));
    cJSON_AddNumberToObject(o, "big", heap_caps_get_largest_free_block(caps));
    cJSON_AddNumberToObject(o, "tot", heap_caps_get_total_size(caps));
}

static esp_err_t h_debug(httpd_req_t *req)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "up_ms", (double)(esp_timer_get_time() / 1000));
    add_caps(r, "int", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    add_caps(r, "dma", MALLOC_CAP_DMA);
    add_caps(r, "psram", MALLOC_CAP_SPIRAM);
    UBaseType_t n = uxTaskGetNumberOfTasks() + 4;
    TaskStatus_t *ts = heap_caps_malloc(n * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM);
    if (ts) {
        configRUN_TIME_COUNTER_TYPE total = 0;
        n = uxTaskGetSystemState(ts, n, &total);
        cJSON_AddNumberToObject(r, "rt_total", (double)total);
        cJSON *a = cJSON_AddArrayToObject(r, "tasks");
        for (UBaseType_t i = 0; i < n; i++) {
            cJSON *t = cJSON_CreateObject();
            cJSON_AddStringToObject(t, "name", ts[i].pcTaskName);
            cJSON_AddNumberToObject(t, "prio", ts[i].uxCurrentPriority);
            BaseType_t core = xTaskGetCoreID(ts[i].xHandle);
            cJSON_AddNumberToObject(t, "core", core == tskNO_AFFINITY ? -1 : (int)core);
            cJSON_AddNumberToObject(t, "stack_free", ts[i].usStackHighWaterMark); /* bytes on ESP-IDF */
            cJSON_AddBoolToObject(t, "stack_ext", esp_ptr_external_ram(ts[i].pxStackBase));
            cJSON_AddNumberToObject(t, "rt", (double)ts[i].ulRunTimeCounter);
            cJSON_AddItemToArray(a, t);
        }
        free(ts);
    }
    return web_send_json(req, r);
}

static const char *group_from_uri(httpd_req_t *req, char *buf, size_t len)
{
    const char *g = req->uri + strlen("/api/config/");
    size_t n = strcspn(g, "?/");
    if (n == 0 || n >= len) return NULL;
    memcpy(buf, g, n);
    buf[n] = 0;
    return buf;
}

static void apply_cfg_hook(const char *group, cJSON *doc)
{
    for (int i = 0; i < 8; i++)
        if (s_cfg_hooks[i].hook && strcmp(s_cfg_hooks[i].group, group) == 0) s_cfg_hooks[i].hook(doc);
}

static esp_err_t send_group(httpd_req_t *req, const char *group)
{
    if (strcmp(group, "wifi") == 0) return web_send_json(req, wifi_mgr_get_config());
    cJSON *doc = settings_get(group);
    if (!doc) return web_send_err(req, 404, "not_found");
    if (strcmp(group, "display") == 0) cJSON_AddStringToObject(doc, "tz", "ICT-7");
    apply_cfg_hook(group, doc);
    return web_send_json(req, doc);
}

static esp_err_t h_cfg_get(httpd_req_t *req)
{
    char g[16];
    if (!group_from_uri(req, g, sizeof(g))) return web_send_err(req, 404, "not_found");
    return send_group(req, g);
}

static esp_err_t h_cfg_put(httpd_req_t *req)
{
    char g[16];
    if (!group_from_uri(req, g, sizeof(g))) return web_send_err(req, 404, "not_found");
    cJSON *j = web_read_json(req);
    if (!j) return ESP_OK;
    esp_err_t r = ESP_OK;
    if (strcmp(g, "wifi") == 0) {
        r = wifi_mgr_set_config(j);
    } else {
        for (int i = 0; i < 4; i++) {
            if (s_put_hooks[i].hook && strcmp(s_put_hooks[i].group, g) == 0 && !s_put_hooks[i].hook(j)) {
                cJSON_Delete(j);
                return web_send_err(req, 400, "bad_value");
            }
        }
        cJSON_DeleteItemFromObject(j, "token"); /* token only changes through regen */
        cJSON_DeleteItemFromObject(j, "max");
        if (strcmp(g, "notify") == 0 && cJSON_IsTrue(cJSON_GetObjectItem(j, "regen"))) {
            static const char al[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
            char tok[17];
            for (int i = 0; i < 16; i++) tok[i] = al[esp_random() % (sizeof(al) - 1)];
            tok[16] = 0;
            cJSON_AddStringToObject(j, "token", tok);
        }
        r = settings_update(g, j);
    }
    cJSON_Delete(j);
    if (r == ESP_ERR_INVALID_ARG) return web_send_err(req, strcmp(g, "wifi") ? 404 : 400, strcmp(g, "wifi") ? "not_found" : "bad_value");
    if (r != ESP_OK) return web_send_err(req, 500, "busy");
    return send_group(req, g);
}

/* ================= wifi ================= */
static esp_err_t h_scan(httpd_req_t *req)
{
    cJSON *r = wifi_mgr_scan();
    if (!r) return web_send_err(req, 503, "busy");
    return web_send_json(req, r);
}

static esp_err_t h_connect(httpd_req_t *req)
{
    cJSON *j = web_read_json(req);
    if (!j) return ESP_OK;
    const char *ss = cJSON_GetStringValue(cJSON_GetObjectItem(j, "ssid"));
    const char *pw = cJSON_GetStringValue(cJSON_GetObjectItem(j, "pass"));
    esp_err_t r = wifi_mgr_connect(ss, pw);
    cJSON_Delete(j);
    if (r == ESP_ERR_NOT_FOUND) return web_send_err(req, 400, "bad_value");
    if (r != ESP_OK) return web_send_err(req, 400, "bad_value");
    return web_send_ok(req);
}

/* ================= system ================= */
static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static void reboot_later(void) { xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL); }

static esp_err_t h_reboot(httpd_req_t *req)
{
    web_send_ok(req);
    reboot_later();
    return ESP_OK;
}

/* POST /api/factory_reset {"confirm":true,"scope":"all"|"data"}.
 * "data": battery log, update outcome and the like go at the next boot; settings, Wi-Fi, stations, alarms, password stay.
 * "all" (default): /data is formatted, and the networks xiaozhi left in NVS are dropped too, or the next boot would
 * import them again instead of opening the setup hotspot. */
static esp_err_t h_factory(httpd_req_t *req)
{
    cJSON *j = web_read_json(req);
    if (!j) return ESP_OK;
    bool ok = cJSON_IsTrue(cJSON_GetObjectItem(j, "confirm"));
    const char *scope = cJSON_GetStringValue(cJSON_GetObjectItem(j, "scope"));
    bool data_only = scope && !strcmp(scope, "data");
    if (scope && !data_only && strcmp(scope, "all")) ok = false;
    cJSON_Delete(j);
    if (!ok) return web_send_err(req, 400, "bad_value");
    if (data_only) {
        if (settings_request_data_wipe() != ESP_OK) return web_send_err(req, 500, "busy");
        web_send_ok(req);
        reboot_later();
        return ESP_OK;
    }
    web_send_ok(req);
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    settings_factory_reset();
    reboot_later();
    return ESP_OK;
}

static esp_err_t h_ota(httpd_req_t *req)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) return web_send_err(req, 500, "busy");
    if (req->content_len == 0 || req->content_len > part->size) return web_send_err(req, 413, "too_large");
    esp_ota_handle_t h;
    if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK) return web_send_err(req, 500, "busy");
    char *buf = malloc(4096);
    int left = req->content_len;
    bool first = true, ok = true;
    while (left > 0) {
        int r = httpd_req_recv(req, buf, left < 4096 ? left : 4096);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) { ok = false; break; }
        if (first && (uint8_t)buf[0] != 0xE9) { ok = false; break; }
        first = false;
        if (esp_ota_write(h, buf, r) != ESP_OK) { ok = false; break; }
        left -= r;
    }
    free(buf);
    if (!ok || esp_ota_end(h) != ESP_OK) {
        if (ok == false) esp_ota_abort(h);
        return web_send_err(req, 400, "bad_image");
    }
    if (esp_ota_set_boot_partition(part) != ESP_OK) return web_send_err(req, 400, "bad_image");
    ESP_LOGI(TAG, "OTA ok -> %s, rebooting", part->label);
    web_send_ok(req);
    reboot_later();
    return ESP_OK;
}

/* ================= static files + captive portal ================= */
static const char *mime(const char *path)
{
    const char *e = strrchr(path, '.');
    if (!e) return "application/octet-stream";
    if (!strcmp(e, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(e, ".css")) return "text/css";
    if (!strcmp(e, ".js")) return "application/javascript";
    if (!strcmp(e, ".json")) return "application/json";
    if (!strcmp(e, ".svg")) return "image/svg+xml";
    if (!strcmp(e, ".png")) return "image/png";
    if (!strcmp(e, ".ico")) return "image/x-icon";
    if (!strcmp(e, ".woff2")) return "font/woff2";
    if (!strcmp(e, ".bmp")) return "image/bmp";
    return "application/octet-stream";
}

static esp_err_t h_static(httpd_req_t *req)
{
    /* captive portal: in AP mode, send foreign hosts to our page */
    if (wifi_mgr_mode() == NET_MODE_AP || wifi_mgr_mode() == NET_MODE_CONNECTING) {
        char host[64] = "";
        httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
        if (host[0] && strcmp(host, "192.168.4.1") != 0 && strncmp(host, "meowbox", 7) != 0) {
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
            return httpd_resp_send(req, NULL, 0);
        }
    }
    if (strncmp(req->uri, "/api/", 5) == 0) return web_send_err(req, 404, "not_found");

    /* files come from the pack embedded in the app (components/webui, built by web/build.py) */
    size_t ul = strcspn(req->uri, "?#");
    const char *path = ul <= 1 ? "index.html" : req->uri + 1;
    size_t pl = ul <= 1 ? strlen("index.html") : ul - 1;
    webui_file_t f;
    if (strstr(req->uri, "..") || !webui_find(path, pl, &f)) {
        if (ul <= 1) {
            httpd_resp_set_type(req, "text/html; charset=utf-8");
            return httpd_resp_sendstr(req, "<h1>MeowBox</h1><p>This firmware was built without the web UI "
                                           "(run web/build.py). API is up at /api/status.</p>");
        }
        return web_send_err(req, 404, "not_found");
    }
    char mpath[64];
    snprintf(mpath, sizeof(mpath), "%.*s", (int)pl, path);
    char etag[16], inm[24] = "";
    snprintf(etag, sizeof(etag), "\"%s\"", f.etag);
    httpd_resp_set_hdr(req, "ETag", etag);
    /* hashed names never change; fonts and theme previews rarely; index.html must be revalidated (ETag -> 304) */
    httpd_resp_set_hdr(req, "Cache-Control", f.immutable                      ? "public, max-age=31536000, immutable"
                                             : strncmp(mpath, "fonts/", 6) == 0 ||
                                                       strncmp(mpath, "themes/", 7) == 0
                                                 ? "max-age=604800"
                                                 : "no-cache");
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof(inm)) == ESP_OK && strcmp(inm, etag) == 0) {
        httpd_resp_set_status(req, "304 Not Modified");
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_type(req, mime(mpath));
    if (f.gzip) httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, (const char *)f.data, f.len);
}

void web_ready(void)
{
    httpd_uri_t u = {.uri = "/*", .method = HTTP_GET, .handler = h_static};
    httpd_register_uri_handler(s_srv, &u);
    ESP_LOGI(TAG, "web server ready");
}

esp_err_t web_start(void)
{
    auth_init();
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 48;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 7;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    esp_err_t r = httpd_start(&s_srv, &cfg);
    if (r != ESP_OK) return r;

    web_register("/api/login", HTTP_POST, h_login, WEB_AUTH_NONE);
    web_register("/api/ping", HTTP_GET, h_ping, WEB_AUTH_NONE);
    web_register("/api/logout", HTTP_POST, h_logout, WEB_AUTH_NONE);
    web_register("/api/session", HTTP_GET, h_session, WEB_AUTH_SESSION);
    web_register("/api/password", HTTP_POST, h_password, WEB_AUTH_SESSION);
    web_register("/api/status", HTTP_GET, h_status, WEB_AUTH_SESSION);
    web_register("/api/debug", HTTP_GET, h_debug, WEB_AUTH_SESSION);
    web_register("/api/config/*", HTTP_GET, h_cfg_get, WEB_AUTH_SESSION);
    web_register("/api/config/*", HTTP_PUT, h_cfg_put, WEB_AUTH_SESSION);
    web_register("/api/wifi/scan", HTTP_GET, h_scan, WEB_AUTH_SESSION);
    web_register("/api/wifi/connect", HTTP_POST, h_connect, WEB_AUTH_SESSION);
    web_register("/api/reboot", HTTP_POST, h_reboot, WEB_AUTH_SESSION);
    web_register("/api/factory_reset", HTTP_POST, h_factory, WEB_AUTH_SESSION);
    web_register("/api/ota", HTTP_POST, h_ota, WEB_AUTH_SESSION);
    return ESP_OK;
}
