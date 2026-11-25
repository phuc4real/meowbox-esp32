#include "diag.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "cJSON.h"

static const char *TAG = "diag";

#define RING_SZ   2048
#define RTC_MAGIC 0x4D42444Cu /* "MBDL" */
#define DIAG_LINE 120
#define BOOTS_MAX 10
#define BOOTS     "/data/boots.json"
#define STALL_S   10

/* Kept across software resets, panics and watchdog resets (not across power off). */
typedef struct {
    uint32_t magic;
    uint32_t head;    /* next write position */
    uint32_t wrapped; /* the ring has been filled once */
    uint32_t up_s, int_free, int_min;
    char buf[RING_SZ];
} rtc_log_t;
static RTC_NOINIT_ATTR rtc_log_t s_rtc;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_prev_vprintf;
static const char *s_reason = "unknown";
static diag_prev_t s_prev;
static const char *s_prev_log = "";
static int s_boot_no;
static volatile uint32_t s_ui_ticks;

static const char *reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_EXT: return "ext";
    case ESP_RST_SW: return "sw";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    case ESP_RST_USB: return "usb";
    case ESP_RST_JTAG: return "jtag";
    default: return "unknown";
    }
}

/* Linear copy of the ring, oldest first, starting at a line boundary. */
static size_t ring_copy(const rtc_log_t *r, char *out, size_t len)
{
    char *tmp = heap_caps_malloc(RING_SZ, MALLOC_CAP_SPIRAM); /* not internal RAM: it has no margin */
    if (!tmp || !len) {
        free(tmp);
        if (len) out[0] = 0;
        return 0;
    }
    taskENTER_CRITICAL(&s_mux);
    uint32_t head = r->head % RING_SZ, n = r->wrapped ? RING_SZ : head;
    if (r->wrapped) {
        memcpy(tmp, r->buf + head, RING_SZ - head);
        memcpy(tmp + RING_SZ - head, r->buf, head);
    } else {
        memcpy(tmp, r->buf, head);
    }
    taskEXIT_CRITICAL(&s_mux);
    size_t start = 0;
    if (r->wrapped) { /* drop the partial first line */
        while (start < n && tmp[start] != '\n') start++;
        if (start < n) start++;
    }
    size_t m = n - start;
    if (m >= len) {
        start += m - (len - 1);
        m = len - 1;
    }
    memcpy(out, tmp + start, m);
    out[m] = 0;
    for (size_t i = 0; i < m; i++)
        if (out[i] == 0) out[i] = '?'; /* garbage from a half-valid RTC copy */
    free(tmp);
    return m;
}

static void ring_put(const char *s, int n)
{
    if (n <= 0) return;
    if (n > DIAG_LINE) n = DIAG_LINE;
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) n--;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i <= n; i++) {
        s_rtc.buf[s_rtc.head] = i < n ? s[i] : '\n';
        if (++s_rtc.head >= RING_SZ) {
            s_rtc.head = 0;
            s_rtc.wrapped = 1;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
}

/* Log v1 without colours: an error / warning line's format starts with "E (" / "W (". Only those get formatted
 * here, so ordinary log lines cost nothing extra on the calling task's stack. */
static int log_hook(const char *fmt, va_list ap)
{
    if ((fmt[0] == 'E' || fmt[0] == 'W') && fmt[1] == ' ' && fmt[2] == '(') {
        char line[DIAG_LINE + 8];
        va_list cp;
        va_copy(cp, ap);
        int n = vsnprintf(line, sizeof(line), fmt, cp);
        va_end(cp);
        ring_put(line, n < (int)sizeof(line) ? n : (int)sizeof(line) - 1);
    }
    return s_prev_vprintf ? s_prev_vprintf(fmt, ap) : vprintf(fmt, ap);
}

/* Every 2 s: RAM snapshot and the frozen-UI check. */
static void tick(void *arg)
{
    static uint32_t last_ticks, stuck_s;
    static bool reported;
    s_rtc.up_s = (uint32_t)(esp_timer_get_time() / 1000000); /* the previous boot's uptime is then within 2 s */
    s_rtc.int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_rtc.int_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    uint32_t t = s_ui_ticks;
    if (t == 0) return; /* UI not started yet */
    if (t != last_ticks) {
        if (reported) ESP_LOGW(TAG, "UI task running again after %lu s", (unsigned long)stuck_s);
        last_ticks = t;
        stuck_s = 0;
        reported = false;
        return;
    }
    stuck_s += 2;
    if (stuck_s >= STALL_S && !reported) {
        reported = true;
        ESP_LOGE(TAG, "UI task got no CPU for %lu s (screen frozen)", (unsigned long)stuck_s);
    }
}

void diag_init(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    s_reason = reason_str(r);
    bool kept = r != ESP_RST_POWERON && r != ESP_RST_BROWNOUT && s_rtc.magic == RTC_MAGIC && s_rtc.head < RING_SZ;
    if (kept) {
        s_prev = (diag_prev_t){true, s_rtc.up_s, s_rtc.int_free, s_rtc.int_min};
        char *p = heap_caps_malloc(RING_SZ + 1, MALLOC_CAP_SPIRAM);
        if (p) {
            ring_copy(&s_rtc, p, RING_SZ + 1);
            s_prev_log = p;
        }
    }
    memset(&s_rtc, 0, sizeof(s_rtc));
    s_rtc.magic = RTC_MAGIC;
    s_prev_vprintf = esp_log_set_vprintf(log_hook);
    const esp_timer_create_args_t ta = {.callback = tick, .name = "diag"};
    esp_timer_handle_t th;
    if (esp_timer_create(&ta, &th) == ESP_OK) esp_timer_start_periodic(th, 2000000);
    ESP_LOGI(TAG, "reset reason %s%s", s_reason, s_prev.valid ? ", previous boot log kept" : "");
}

void diag_boot_record(void)
{
    cJSON *arr = NULL;
    FILE *f = fopen(BOOTS, "r");
    if (f) {
        char buf[1536];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = 0;
        arr = cJSON_Parse(buf);
    }
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        arr = cJSON_CreateArray();
    }
    cJSON *last = cJSON_GetArrayItem(arr, 0);
    cJSON *ln = last ? cJSON_GetObjectItem(last, "n") : NULL;
    s_boot_no = (cJSON_IsNumber(ln) ? ln->valueint : 0) + 1;
    if (last && s_prev.valid) { /* how the previous boot ended, now that we know */
        cJSON_ReplaceItemInObject(last, "up_s", cJSON_CreateNumber(s_prev.up_s));
        cJSON_ReplaceItemInObject(last, "int_min", cJSON_CreateNumber(s_prev.int_min));
    }
    cJSON *e = cJSON_CreateObject();
    cJSON_AddNumberToObject(e, "n", s_boot_no);
    cJSON_AddStringToObject(e, "reason", s_reason);
    cJSON_AddNumberToObject(e, "up_s", 0);
    cJSON_AddNumberToObject(e, "int_min", 0);
    cJSON_InsertItemInArray(arr, 0, e);
    while (cJSON_GetArraySize(arr) > BOOTS_MAX) cJSON_DeleteItemFromArray(arr, BOOTS_MAX);
    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!s) return;
    f = fopen(BOOTS, "w");
    if (f) {
        fputs(s, f);
        fclose(f);
    }
    free(s);
}

void diag_ui_alive(void) { s_ui_ticks++; }
int diag_boot_no(void) { return s_boot_no; }
const char *diag_reason(void) { return s_reason; }
diag_prev_t diag_prev(void) { return s_prev; }
const char *diag_prev_log(void) { return s_prev_log; }
size_t diag_log(char *out, size_t len) { return ring_copy(&s_rtc, out, len); }
const char *diag_boots_path(void) { return BOOTS; }
