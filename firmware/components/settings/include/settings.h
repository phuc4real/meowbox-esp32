/*
 * Settings store: one JSON document per group, in the "data" LittleFS partition (/data/cfg/<group>.json).
 * Groups and defaults follow firmware/web/API.md: radio, alarms, weather, display, cat, notify, pc.
 * Wi-Fi credentials and the admin password are kept by their own modules (also in /data).
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Mounts /data (user data, LittleFS) and loads every settings group. */
esp_err_t settings_init(void);

/* Returns a copy of the group (defaults merged in). Caller must cJSON_Delete(). NULL for unknown group. */
cJSON *settings_get(const char *group);

/* Shallow merge: only keys present in the defaults are accepted; type must match (numbers/bools/strings/arrays).
 * Persists the result and fires change callbacks. Returns ESP_ERR_INVALID_ARG on unknown group. */
esp_err_t settings_update(const char *group, const cJSON *patch);

/* Convenience readers (no allocation for the caller). */
int settings_get_int(const char *group, const char *key, int def);
double settings_get_num(const char *group, const char *key, double def);
bool settings_get_bool(const char *group, const char *key, bool def);
/* Copies a string value into buf. Returns buf. */
char *settings_get_str(const char *group, const char *key, char *buf, size_t len, const char *def);

typedef void (*settings_cb_t)(const char *group, void *arg);
void settings_on_change(settings_cb_t cb, void *arg);

/* Small helpers for whole-file blobs in /data (used by wifi, auth, cat). */
char *settings_read_file(const char *path, size_t *len); /* malloc'd, NUL terminated; NULL if missing */
esp_err_t settings_write_file(const char *path, const void *data, size_t len); /* atomic (tmp + rename) */

/* Erases /data completely (factory reset). */
esp_err_t settings_factory_reset(void);

/* "Clear data": everything in /data except the configuration (cfg/, auth.json, wifi.json) is deleted at the next
 * boot, before any app opens its files: battery log, update outcome... Call it, then restart. */
esp_err_t settings_request_data_wipe(void);

#ifdef __cplusplus
}
#endif
