/* MeowBox feature apps. Each *_init() builds its tile (call with the LVGL lock held). */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Clock + lunar calendar + weather + alarms (P2) */
void clock_app_init(void);
bool clock_alarm_ringing(void);
void clock_alarm_stop(bool snooze);   /* thread-safe */

/* Radio (P3) */
#include "cJSON.h"
void radio_app_init(void);
int radio_play_id(int id);            /* 0 ok, -1 not found, -2 YouTube needs the PC relay */
void radio_toggle(void);
bool radio_playing(void);
void radio_set_relay(const char *host_port);
bool radio_relay_hostport(char *hp, size_t len); /* "ip:port" of the YouTube relay in use, false if none */
void radio_status_hook(cJSON *status);

/* Notifications (P4) */
void notify_app_init(void);
int notify_post(const char *title, const char *text, const char *sound, const char *prio, int duration, const char *src);

/* PC screen (P5) */
void pc_app_init(void);
bool pc_online(void);

/* Meow Coding: the cat tile shows the AI coding agents running on the PC (cat_app.c) */
#include "lvgl.h"
void cat_app_init(void);
void cat_mini_attach(lv_obj_t *parent, int x, int y, int scale); /* small live cat for the clock face */
const char *cat_mood_text(void);    /* "coding", "needs you"... for the clock's terminal line */
void cat_hear_clap(void);           /* thread-safe: the cat jumps (setting cat.clap) */
void cat_coding_update(cJSON *msg); /* thread-safe: {"t":"coding","s":[...]} from the PC agent */

/* Battery runtime measurement */
void battery_app_init(void);
void battery_status_hook(cJSON *status);
int battery_pct(void);          /* smoothed 0..100 */
int battery_mv(void);           /* -1 unknown */
int battery_eta_min(void);      /* estimated minutes left, -1 unknown/charging */
int battery_session_min(void);  /* minutes on battery so far, -1 if charging */

/* Firmware updates from GitHub Releases (update_app.c) */
void update_app_init(void);
void update_status_hook(cJSON *status);   /* adds "upd":"x.y.z" when a newer release was found */
const char *update_available(void);       /* newer version found by the last check, or NULL */

/* System/diagnostics tile */
void system_app_init(void);
/* Open the Wi-Fi setup hotspot; its QR-code card shows within 0.5 s (in_lvgl is kept for callers, unused) */
void system_wifi_setup(bool in_lvgl);

#ifdef __cplusplus
}
#endif
