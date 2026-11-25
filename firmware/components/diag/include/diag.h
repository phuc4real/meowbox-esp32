#pragma once
/* Crash and error log that survives a reset (0.2.3).
 *
 * Every ESP_LOGE / ESP_LOGW line also goes to a 2 KB ring in RTC memory (RTC_NOINIT: kept across software resets,
 * panics and watchdog resets, lost on power off), with a snapshot of uptime and free internal RAM every 2 s. At the
 * next boot that ring becomes the "previous boot" log, read with the reset reason through GET /api/log. A UI task
 * that gets no CPU for 10 s is logged as an error, so a frozen screen leaves a trace even without a reset. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool valid;        /* false after power-on or when the RTC copy did not survive */
    uint32_t up_s;     /* how long the previous boot ran (2 s resolution) */
    uint32_t int_free; /* internal RAM free / lowest at that snapshot */
    uint32_t int_min;
} diag_prev_t;

void diag_init(void);        /* first thing in app_main: reset reason, previous ring, log hook */
void diag_boot_record(void); /* after /data is mounted: append this boot to /data/boots.json (last 10) */
void diag_ui_alive(void);    /* call from the UI task on every UI tick */

int diag_boot_no(void);        /* 1, 2, ... counted in /data/boots.json */
const char *diag_reason(void); /* "power_on" | "sw" | "panic" | "task_wdt" | "int_wdt" | "wdt" | "brownout" | ... */
diag_prev_t diag_prev(void);
const char *diag_prev_log(void);        /* E/W lines of the previous boot, oldest first, '\n'-separated ("" if none) */
size_t diag_log(char *out, size_t len); /* E/W lines of this boot so far (the newest ~2 KB) */
const char *diag_boots_path(void);      /* JSON array [{"n","reason","up_s","int_min"}], newest first */

#ifdef __cplusplus
}
#endif
