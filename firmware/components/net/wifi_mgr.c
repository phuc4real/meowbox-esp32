#include "net.h"

#include <string.h>
#include <time.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif_sntp.h"
#include "mdns.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "settings.h"

static const char *TAG = "wifi";

#define WIFI_FILE "/data/wifi.json"
#define MAX_NETS 5
#define AP_PASS "meowbox2026" /* WPA2 password of the MeowBox-XXXX setup hotspot */
#define BIT_GOT_IP BIT0
#define BIT_FAIL BIT1
#define BIT_RECHECK BIT2

typedef struct { char ssid[33]; char pass[65]; } net_t;

static net_t s_nets[MAX_NETS];
static int s_nnets;
static net_mode_t s_mode = NET_MODE_CONNECTING;
static bool s_ap_on;
static char s_ap_ssid[20];
static char s_cur_ssid[33];
static esp_netif_t *s_sta_if, *s_ap_if;
static EventGroupHandle_t s_ev;
static SemaphoreHandle_t s_lock;
static bool s_scanning;
static volatile int s_force_idx = -1;
static bool s_time_valid;
static TaskHandle_t s_dns_task;

#define MAX_CB 4
static struct { wifi_mgr_cb_t cb; void *arg; } s_cbs[MAX_CB];

/* ---------- persistence ---------- */
static void load_nets(void)
{
    s_nnets = 0;
    char *txt = settings_read_file(WIFI_FILE, NULL);
    if (!txt) return;
    cJSON *root = cJSON_Parse(txt);
    free(txt);
    cJSON *it, *arr = cJSON_GetObjectItem(root, "nets");
    cJSON_ArrayForEach(it, arr) {
        const char *ss = cJSON_GetStringValue(cJSON_GetObjectItem(it, "ssid"));
        const char *pw = cJSON_GetStringValue(cJSON_GetObjectItem(it, "pass"));
        if (!ss || !*ss || s_nnets >= MAX_NETS) continue;
        snprintf(s_nets[s_nnets].ssid, 33, "%s", ss);
        snprintf(s_nets[s_nnets].pass, 65, "%s", pw ? pw : "");
        s_nnets++;
    }
    cJSON_Delete(root);
}

static void save_nets(void);

/* First boot after xiaozhi: import its saved networks (NVS namespace "wifi", ssid/password, ssid1/password1...). */
static void import_xiaozhi_nets(void)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return;
    for (int i = 0; i < 10 && s_nnets < MAX_NETS; i++) {
        char ks[12], kp[12];
        if (i == 0) { strcpy(ks, "ssid"); strcpy(kp, "password"); }
        else { snprintf(ks, sizeof(ks), "ssid%d", i); snprintf(kp, sizeof(kp), "password%d", i); }
        size_t l1 = 33, l2 = 65;
        net_t n = {0};
        if (nvs_get_str(h, ks, n.ssid, &l1) != ESP_OK || !n.ssid[0]) continue;
        if (nvs_get_str(h, kp, n.pass, &l2) != ESP_OK) n.pass[0] = 0;
        s_nets[s_nnets++] = n;
        ESP_LOGI(TAG, "imported network %s from xiaozhi", n.ssid);
    }
    nvs_close(h);
    if (s_nnets) save_nets();
}

static void save_nets(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "nets");
    for (int i = 0; i < s_nnets; i++) {
        cJSON *n = cJSON_CreateObject();
        cJSON_AddStringToObject(n, "ssid", s_nets[i].ssid);
        cJSON_AddStringToObject(n, "pass", s_nets[i].pass);
        cJSON_AddItemToArray(arr, n);
    }
    char *txt = cJSON_PrintUnformatted(root);
    settings_write_file(WIFI_FILE, txt, strlen(txt));
    cJSON_free(txt);
    cJSON_Delete(root);
}

static const char *find_pass(const char *ssid)
{
    for (int i = 0; i < s_nnets; i++)
        if (strcmp(s_nets[i].ssid, ssid) == 0) return s_nets[i].pass;
    return NULL;
}

/* ---------- state ---------- */
static void set_mode(net_mode_t m)
{
    if (s_mode == m) return;
    s_mode = m;
    ESP_LOGI(TAG, "mode -> %s", wifi_mgr_mode_str());
    for (int i = 0; i < MAX_CB; i++)
        if (s_cbs[i].cb) s_cbs[i].cb(m, s_cbs[i].arg);
}

void wifi_mgr_on_change(wifi_mgr_cb_t cb, void *arg)
{
    for (int i = 0; i < MAX_CB; i++) {
        if (!s_cbs[i].cb) {
            s_cbs[i].arg = arg;
            s_cbs[i].cb = cb;
            return;
        }
    }
}

net_mode_t wifi_mgr_mode(void) { return s_mode; }
const char *wifi_mgr_mode_str(void)
{
    return s_mode == NET_MODE_STA ? "sta" : s_mode == NET_MODE_AP ? "ap" : "conn";
}
bool wifi_mgr_connected(void) { return s_mode == NET_MODE_STA; }
const char *wifi_mgr_ap_ssid(void) { return s_ap_ssid; }
const char *wifi_mgr_ap_pass(void) { return AP_PASS; }
bool net_time_valid(void) { return s_time_valid || time(NULL) > 1700000000; }

void wifi_mgr_info(char *ssid, size_t ssid_len, char *ip, size_t ip_len, int *rssi)
{
    esp_netif_ip_info_t info = {0};
    if (s_mode == NET_MODE_STA) {
        esp_netif_get_ip_info(s_sta_if, &info);
        if (ssid) snprintf(ssid, ssid_len, "%s", s_cur_ssid);
        wifi_ap_record_t ap;
        if (rssi) *rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
    } else {
        esp_netif_get_ip_info(s_ap_if, &info);
        if (ssid) snprintf(ssid, ssid_len, "%s", s_ap_on ? s_ap_ssid : "");
        if (rssi) *rssi = 0;
    }
    if (ip) snprintf(ip, ip_len, IPSTR, IP2STR(&info.ip));
}

/* ---------- captive DNS: answer every A query with 192.168.4.1 ---------- */
static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "dns bind failed");
        if (sock >= 0) close(sock);
        s_dns_task = NULL;
        vTaskDelete(NULL);
    }
    struct timeval tv = {.tv_sec = 1};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint8_t buf[512];
    while (s_ap_on) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &fl);
        if (n < 12) continue;
        /* find end of question */
        int p = 12;
        while (p < n && buf[p]) p += buf[p] + 1;
        p += 5; /* zero label + qtype + qclass */
        if (p > n) continue;
        buf[2] = 0x81; buf[3] = 0x80;          /* response, recursion available, no error */
        buf[6] = 0; buf[7] = 1;                /* ancount = 1 */
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        uint8_t ans[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1};
        memcpy(buf + p, ans, sizeof(ans));
        sendto(sock, buf, p + sizeof(ans), 0, (struct sockaddr *)&from, fl);
    }
    close(sock);
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

static void ap_enable(bool on)
{
    if (on == s_ap_on) return;
    s_ap_on = on;
    if (on) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        wifi_config_t ap = {0};
        strcpy((char *)ap.ap.ssid, s_ap_ssid);
        ap.ap.ssid_len = strlen(s_ap_ssid);
        ap.ap.channel = 1;
        ap.ap.max_connection = 4;
        strcpy((char *)ap.ap.password, AP_PASS);
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
        ap.ap.pairwise_cipher = WIFI_CIPHER_TYPE_CCMP;
        esp_wifi_set_config(WIFI_IF_AP, &ap);
        if (!s_dns_task) xTaskCreate(dns_task, "dns", 3072, NULL, 3, &s_dns_task);
        ESP_LOGI(TAG, "SoftAP %s up, WPA2 (http://192.168.4.1)", s_ap_ssid);
    } else {
        esp_wifi_set_mode(WIFI_MODE_STA);
        ESP_LOGI(TAG, "SoftAP off");
    }
}

/* ---------- events ---------- */
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_mode == NET_MODE_STA) {
            ESP_LOGW(TAG, "disconnected from %s", s_cur_ssid);
            set_mode(s_ap_on ? NET_MODE_AP : NET_MODE_CONNECTING);
        }
        xEventGroupSetBits(s_ev, BIT_FAIL);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "connected to %s, ip " IPSTR, s_cur_ssid, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_ev, BIT_GOT_IP);
    }
}

static void time_synced(struct timeval *tv)
{
    s_time_valid = true;
    ESP_LOGI(TAG, "time synced");
}

static bool try_connect(const net_t *n)
{
    wifi_config_t c = {0};
    snprintf((char *)c.sta.ssid, sizeof(c.sta.ssid), "%s", n->ssid);
    snprintf((char *)c.sta.password, sizeof(c.sta.password), "%s", n->pass);
    c.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    c.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    c.sta.threshold.authmode = n->pass[0] ? WIFI_AUTH_WEP : WIFI_AUTH_OPEN;
    snprintf(s_cur_ssid, sizeof(s_cur_ssid), "%s", n->ssid);
    esp_wifi_disconnect();
    xEventGroupClearBits(s_ev, BIT_GOT_IP | BIT_FAIL);
    esp_wifi_set_config(WIFI_IF_STA, &c);
    esp_wifi_connect();
    ESP_LOGI(TAG, "trying %s", n->ssid);
    /* one disconnect event may come from esp_wifi_disconnect() above; wait for IP up to 12 s */
    TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(12000);
    while (xTaskGetTickCount() < until) {
        EventBits_t b = xEventGroupWaitBits(s_ev, BIT_GOT_IP | BIT_FAIL, pdTRUE, pdFALSE, pdMS_TO_TICKS(500));
        if (b & BIT_GOT_IP) return true;
        if (b & BIT_FAIL) {
            vTaskDelay(pdMS_TO_TICKS(300));
            esp_wifi_connect();
        }
    }
    return false;
}

static void conn_task(void *arg)
{
    int fails = 0;
    while (1) {
        if (s_mode == NET_MODE_STA) {
            xEventGroupWaitBits(s_ev, BIT_FAIL | BIT_RECHECK, pdFALSE, pdFALSE, portMAX_DELAY);
            if (s_force_idx < 0 && s_mode == NET_MODE_STA) {
                xEventGroupClearBits(s_ev, BIT_RECHECK);
                continue;
            }
        }
        xEventGroupClearBits(s_ev, BIT_RECHECK);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        int n = s_nnets;
        net_t nets[MAX_NETS];
        memcpy(nets, s_nets, sizeof(nets));
        xSemaphoreGive(s_lock);

        if (n == 0) {
            ap_enable(true);
            set_mode(NET_MODE_AP);
            xEventGroupWaitBits(s_ev, BIT_RECHECK, pdFALSE, pdFALSE, portMAX_DELAY);
            continue;
        }
        bool ok = false;
        int start = s_force_idx >= 0 ? s_force_idx : 0;
        s_force_idx = -1;
        for (int k = 0; k < n && !ok; k++) ok = try_connect(&nets[(start + k) % n]);
        if (ok) {
            fails = 0;
            set_mode(NET_MODE_STA);
            if (s_ap_on) {
                vTaskDelay(pdMS_TO_TICKS(5000)); /* let the portal page show the new IP */
                ap_enable(false);
            }
            continue;
        }
        fails++;
        if (fails >= 1 && !s_ap_on) ap_enable(true); /* nobody reachable: open the portal while retrying */
        set_mode(s_ap_on ? NET_MODE_AP : NET_MODE_CONNECTING);
        xEventGroupWaitBits(s_ev, BIT_RECHECK, pdFALSE, pdFALSE, pdMS_TO_TICKS(fails < 5 ? 15000 : 60000));
    }
}

/* ---------- API ---------- */
cJSON *wifi_mgr_get_config(void)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "cur", s_mode == NET_MODE_STA ? s_cur_ssid : "");
    cJSON_AddNumberToObject(r, "max", MAX_NETS);
    cJSON *arr = cJSON_AddArrayToObject(r, "nets");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_nnets; i++) {
        cJSON *n = cJSON_CreateObject();
        cJSON_AddStringToObject(n, "ssid", s_nets[i].ssid);
        cJSON_AddItemToArray(arr, n);
    }
    xSemaphoreGive(s_lock);
    return r;
}

esp_err_t wifi_mgr_set_config(const cJSON *body)
{
    const cJSON *arr = cJSON_GetObjectItem(body, "nets");
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) > MAX_NETS) return ESP_ERR_INVALID_ARG;
    net_t nn[MAX_NETS];
    int cnt = 0;
    const cJSON *it;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON_ArrayForEach(it, arr) {
        const char *ss = cJSON_GetStringValue(cJSON_GetObjectItem(it, "ssid"));
        const char *pw = cJSON_GetStringValue(cJSON_GetObjectItem(it, "pass"));
        if (!ss || !*ss || strlen(ss) > 32 || (pw && strlen(pw) > 64)) continue;
        if (!pw) pw = find_pass(ss);
        if (!pw) continue; /* new SSID without password: skip */
        snprintf(nn[cnt].ssid, 33, "%s", ss);
        snprintf(nn[cnt].pass, 65, "%s", pw);
        cnt++;
    }
    memcpy(s_nets, nn, sizeof(nn));
    s_nnets = cnt;
    save_nets();
    xSemaphoreGive(s_lock);
    if (s_mode != NET_MODE_STA) xEventGroupSetBits(s_ev, BIT_RECHECK);
    return ESP_OK;
}

esp_err_t wifi_mgr_connect(const char *ssid, const char *pass)
{
    if (!ssid || !*ssid || strlen(ssid) > 32 || (pass && strlen(pass) > 64)) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const char *saved = find_pass(ssid);
    if (!pass) pass = saved;
    if (!pass) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    net_t n;
    snprintf(n.ssid, 33, "%s", ssid);
    snprintf(n.pass, 65, "%s", pass);
    /* move to the top */
    net_t rest[MAX_NETS];
    int k = 0;
    for (int i = 0; i < s_nnets && k < MAX_NETS - 1; i++)
        if (strcmp(s_nets[i].ssid, ssid) != 0) rest[k++] = s_nets[i];
    s_nets[0] = n;
    memcpy(&s_nets[1], rest, k * sizeof(net_t));
    s_nnets = k + 1;
    save_nets();
    xSemaphoreGive(s_lock);
    s_force_idx = 0;
    xEventGroupSetBits(s_ev, BIT_RECHECK | BIT_FAIL);
    return ESP_OK;
}

void wifi_mgr_start_portal(void) { ap_enable(true); if (s_mode != NET_MODE_STA) set_mode(NET_MODE_AP); }

cJSON *wifi_mgr_scan(void)
{
    if (s_scanning) return NULL;
    s_scanning = true;
    wifi_scan_config_t sc = {.show_hidden = false};
    cJSON *r = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(r, "aps");
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
        uint16_t num = 0;
        esp_wifi_scan_get_ap_num(&num);
        if (num > 30) num = 30;
        wifi_ap_record_t *recs = calloc(num ? num : 1, sizeof(*recs));
        esp_wifi_scan_get_ap_records(&num, recs);
        for (int i = 0; i < num; i++) { /* records come sorted by RSSI; dedupe by SSID */
            if (!recs[i].ssid[0]) continue;
            bool dup = false;
            for (int j = 0; j < i; j++)
                if (strcmp((char *)recs[i].ssid, (char *)recs[j].ssid) == 0) dup = true;
            if (dup) continue;
            cJSON *a = cJSON_CreateObject();
            cJSON_AddStringToObject(a, "ssid", (char *)recs[i].ssid);
            cJSON_AddNumberToObject(a, "rssi", recs[i].rssi);
            cJSON_AddNumberToObject(a, "ch", recs[i].primary);
            cJSON_AddBoolToObject(a, "sec", recs[i].authmode != WIFI_AUTH_OPEN);
            cJSON_AddItemToArray(arr, a);
        }
        free(recs);
    }
    s_scanning = false;
    return r;
}

esp_err_t wifi_mgr_start(void)
{
    s_ev = xEventGroupCreate();
    s_lock = xSemaphoreCreateMutex();
    load_nets();
    if (s_nnets == 0) import_xiaozhi_nets();

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t r = esp_event_loop_create_default();
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) return r;
    s_sta_if = esp_netif_create_default_wifi_sta();
    s_ap_if = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta_if, "meowbox");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "MeowBox-%02X%02X", mac[4], mac[5]);

    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    /* time */
    setenv("TZ", "ICT-7", 1);
    tzset();
    esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sc.sync_cb = time_synced;
    esp_netif_sntp_init(&sc);

    /* mDNS: meowbox.local + service for the PC agent */
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set("meowbox");
        mdns_instance_name_set("MeowBox");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        mdns_service_add(NULL, "_meowbox", "_tcp", 80, NULL, 0);
    }

    xTaskCreate(conn_task, "wifi_conn", 4096, NULL, 4, NULL);
    return ESP_OK;
}
