/*
 * MeowBox networking: Wi-Fi manager (STA + SoftAP captive portal), mDNS, SNTP, HTTP server core.
 * REST contract: firmware/web/API.md.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Wi-Fi ---------- */
typedef enum { NET_MODE_CONNECTING, NET_MODE_STA, NET_MODE_AP } net_mode_t;

esp_err_t wifi_mgr_start(void);
net_mode_t wifi_mgr_mode(void);
const char *wifi_mgr_mode_str(void);        /* "conn" | "sta" | "ap" */
bool wifi_mgr_connected(void);
void wifi_mgr_info(char *ssid, size_t ssid_len, char *ip, size_t ip_len, int *rssi);
const char *wifi_mgr_ap_ssid(void);         /* MeowBox-XXXX */
const char *wifi_mgr_ap_pass(void);         /* WPA2 password of that hotspot */
cJSON *wifi_mgr_get_config(void);           /* API.md GET /api/config/wifi */
esp_err_t wifi_mgr_set_config(const cJSON *body);
cJSON *wifi_mgr_scan(void);                 /* NULL if busy */
esp_err_t wifi_mgr_connect(const char *ssid, const char *pass /* NULL = saved */);
void wifi_mgr_start_portal(void);           /* open the setup hotspot by hand */
bool wifi_mgr_stop_portal(void);            /* close it; refused (false) while offline */
bool wifi_mgr_portal_on(void);
/* Seconds until a hand-opened hotspot closes on its own: -1 = not applicable (closed, or offline),
 * -2 = paused because a client is connected */
int wifi_mgr_portal_left_s(void);

/* Keep Wi-Fi out of power save while something streams (docs/firmware_v0.2.0.md, 4.1). Power save makes throughput
 * and latency jumpy on many routers. Each holder sets its own bit; power save returns 10 s after the last one lets go. */
typedef enum { WIFI_HOLD_RADIO = 1 << 0, WIFI_HOLD_OTA = 1 << 1 } wifi_hold_t;
void wifi_mgr_hold_awake(wifi_hold_t who, bool on);

typedef void (*wifi_mgr_cb_t)(net_mode_t mode, void *arg);
void wifi_mgr_on_change(wifi_mgr_cb_t cb, void *arg);

/* ---------- time ---------- */
bool net_time_valid(void);

/* ---------- HTTP server ---------- */
typedef enum {
    WEB_AUTH_NONE,    /* public */
    WEB_AUTH_SESSION, /* web UI session cookie */
    WEB_AUTH_TOKEN,   /* notify bearer/?token=, session also accepted */
} web_auth_t;

esp_err_t web_start(void);                  /* starts httpd + core endpoints */
esp_err_t web_register(const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *), web_auth_t auth);
esp_err_t web_register_ws(const char *uri, esp_err_t (*handler)(httpd_req_t *));
void web_ready(void);                       /* call after all web_register(): adds the static/captive catch-all */
httpd_handle_t web_server(void);

/* Helpers for handlers */
esp_err_t web_send_json(httpd_req_t *req, cJSON *json); /* takes ownership */
esp_err_t web_send_ok(httpd_req_t *req);
esp_err_t web_send_err(httpd_req_t *req, int status, const char *code);
cJSON *web_read_json(httpd_req_t *req);     /* NULL + error already sent on failure */
bool web_query(httpd_req_t *req, const char *key, char *out, size_t len); /* URL-decoded */

/* Hooks so feature modules can add live data to core endpoints */
typedef void (*web_json_hook_t)(cJSON *obj);
void web_set_status_hook(web_json_hook_t hook);                     /* adds play/bat/... to /api/status */
void web_set_config_hook(const char *group, web_json_hook_t hook);  /* adds read-only fields to GET config */
/* Validates/normalises a PUT body before it is stored; return false to reject with 400 bad_value. */
typedef bool (*web_put_hook_t)(cJSON *body);
void web_set_put_hook(const char *group, web_put_hook_t hook);

#ifdef __cplusplus
}
#endif
