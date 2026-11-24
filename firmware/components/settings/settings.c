#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_littlefs.h"

static const char *TAG = "settings";

#define CFG_DIR "/data/cfg"

typedef struct {
    const char *name;
    const char *defaults;
    cJSON *doc; /* cached, defaults merged */
} group_t;

static group_t s_groups[] = {
    {"radio", "{\"vol\":40,\"last\":3,\"max\":50,\"st\":["
              "{\"id\":1,\"name\":\"Radio Paradise\",\"url\":\"http://stream.radioparadise.com/mp3-128\",\"type\":\"stream\"},"
              "{\"id\":2,\"name\":\"SomaFM Groove Salad\",\"url\":\"http://ice1.somafm.com/groovesalad-128-mp3\",\"type\":\"stream\"},"
              "{\"id\":3,\"name\":\"Lofi Girl\",\"url\":\"https://www.youtube.com/@LofiGirl/live\",\"type\":\"youtube\"},"
              "{\"id\":4,\"name\":\"VOV3\",\"url\":\"https://str.vov.gov.vn/vovlive/vov3.sdp_aac/playlist.m3u8\",\"type\":\"hls\"}"
              "]}", NULL},
    {"alarms", "{\"snooze\":5,\"max\":5,\"al\":[]}", NULL},
    {"weather", "{\"on\":true,\"name\":\"Hà Nội\",\"lat\":21.0245,\"lon\":105.8412,\"unit\":\"c\"}", NULL},
    {"display", "{\"bri\":80,\"idle_bri\":20,\"idle_s\":60,\"h24\":true,\"night\":true,\"n_from\":22,\"n_to\":7,"
                "\"n_bri\":5,\"lang\":\"vi\",\"theme\":\"swiss\",\"night_mode\":\"auto\",\"fx\":\"full\"}", NULL},
    {"cat", "{\"name\":\"Mướp\",\"fur\":\"tabby\",\"diff\":\"normal\"}", NULL},
    {"notify", "{\"token\":\"\",\"dur\":10,\"vol\":70,\"quiet_low\":true,\"ntfy\":\"\"}", NULL},
    {"pc", "{\"host\":\"\",\"port\":8765,\"auto\":true}", NULL},
    {"update", "{\"auto\":false}", NULL}, /* auto = check GitHub for a new release every day (off: only when asked) */
};
#define NGROUPS (sizeof(s_groups) / sizeof(s_groups[0]))

#define MAX_CB 8
static struct { settings_cb_t cb; void *arg; } s_cbs[MAX_CB];
static SemaphoreHandle_t s_lock;

static group_t *find(const char *name)
{
    for (size_t i = 0; i < NGROUPS; i++)
        if (strcmp(s_groups[i].name, name) == 0) return &s_groups[i];
    return NULL;
}

char *settings_read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(n + 1);
    if (buf && fread(buf, 1, n, f) == (size_t)n) {
        buf[n] = 0;
        if (len) *len = n;
    } else {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    return buf;
}

esp_err_t settings_write_file(const char *path, const void *data, size_t len)
{
    char tmp[96];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return ESP_FAIL;
    bool ok = fwrite(data, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    if (!ok) {
        unlink(tmp);
        return ESP_FAIL;
    }
    unlink(path);
    return rename(tmp, path) == 0 ? ESP_OK : ESP_FAIL;
}

/* Copy keys of `src` into `dst` if the key exists in `dst` with a compatible type. */
static void merge_known(cJSON *dst, const cJSON *src)
{
    const cJSON *it;
    cJSON_ArrayForEach(it, src) {
        cJSON *cur = cJSON_GetObjectItemCaseSensitive(dst, it->string);
        if (!cur) continue;
        bool same = (cJSON_IsNumber(cur) && cJSON_IsNumber(it)) || (cJSON_IsString(cur) && cJSON_IsString(it)) ||
                    (cJSON_IsBool(cur) && cJSON_IsBool(it)) || (cJSON_IsArray(cur) && cJSON_IsArray(it)) ||
                    (cJSON_IsObject(cur) && cJSON_IsObject(it));
        if (!same) continue;
        cJSON_ReplaceItemInObjectCaseSensitive(dst, it->string, cJSON_Duplicate(it, true));
    }
}

static void load_group(group_t *g)
{
    g->doc = cJSON_Parse(g->defaults);
    if (!g->doc) {
        ESP_LOGE(TAG, "bad defaults for %s", g->name);
        g->doc = cJSON_CreateObject();
        return;
    }
    char path[64];
    snprintf(path, sizeof(path), CFG_DIR "/%s.json", g->name);
    char *txt = settings_read_file(path, NULL);
    if (!txt) return;
    cJSON *saved = cJSON_Parse(txt);
    free(txt);
    if (saved) {
        merge_known(g->doc, saved);
        cJSON_Delete(saved);
    } else {
        ESP_LOGW(TAG, "%s is corrupt, using defaults", path);
    }
}

static esp_err_t save_group(group_t *g)
{
    char path[64];
    snprintf(path, sizeof(path), CFG_DIR "/%s.json", g->name);
    char *txt = cJSON_PrintUnformatted(g->doc);
    if (!txt) return ESP_ERR_NO_MEM;
    esp_err_t r = settings_write_file(path, txt, strlen(txt));
    cJSON_free(txt);
    return r;
}

static esp_err_t mount(const char *label, const char *base, bool format_if_fail)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = base,
        .partition_label = label,
        .format_if_mount_failed = format_if_fail,
        .dont_mount = false,
    };
    esp_err_t r = esp_vfs_littlefs_register(&conf);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "mount %s failed: %s", label, esp_err_to_name(r));
        return r;
    }
    size_t total = 0, used = 0;
    esp_littlefs_info(label, &total, &used);
    ESP_LOGI(TAG, "%s mounted at %s: %u/%u KB used", label, base, (unsigned)(used / 1024), (unsigned)(total / 1024));
    return ESP_OK;
}

#define WIPE_FLAG "/data/.wipe"

/* what "clear data" keeps: the configuration */
static bool is_config(const char *name)
{
    return !strcmp(name, "cfg") || !strcmp(name, "auth.json") || !strcmp(name, "wifi.json");
}

static void rm_tree(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        struct dirent *e;
        char sub[288]; /* d_name can be up to 255 */
        while (d && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
            rm_tree(sub);
        }
        if (d) closedir(d);
        rmdir(path);
    } else unlink(path);
}

static void wipe_data_if_requested(void)
{
    struct stat st;
    if (stat(WIPE_FLAG, &st) != 0) return;
    DIR *d = opendir("/data");
    struct dirent *e;
    char path[272];
    while (d && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || is_config(e->d_name)) continue;
        snprintf(path, sizeof(path), "/data/%s", e->d_name);
        rm_tree(path);
    }
    if (d) closedir(d);
    unlink(WIPE_FLAG);
    ESP_LOGW(TAG, "data cleared (configuration kept)");
}

esp_err_t settings_request_data_wipe(void) { return settings_write_file(WIPE_FLAG, "1", 1); }

esp_err_t settings_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    esp_err_t r = mount("data", "/data", true);
    if (r != ESP_OK) return r;
    wipe_data_if_requested();
    mkdir(CFG_DIR, 0775);
    for (size_t i = 0; i < NGROUPS; i++) load_group(&s_groups[i]);
    return ESP_OK;
}

cJSON *settings_get(const char *group)
{
    group_t *g = find(group);
    if (!g) return NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON *c = cJSON_Duplicate(g->doc, true);
    xSemaphoreGive(s_lock);
    return c;
}

esp_err_t settings_update(const char *group, const cJSON *patch)
{
    group_t *g = find(group);
    if (!g || !cJSON_IsObject(patch)) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    merge_known(g->doc, patch);
    esp_err_t r = save_group(g);
    xSemaphoreGive(s_lock);
    for (int i = 0; i < MAX_CB; i++)
        if (s_cbs[i].cb) s_cbs[i].cb(group, s_cbs[i].arg);
    return r;
}

static const cJSON *peek(const char *group, const char *key)
{
    group_t *g = find(group);
    return g ? cJSON_GetObjectItemCaseSensitive(g->doc, key) : NULL;
}

int settings_get_int(const char *group, const char *key, int def)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const cJSON *v = peek(group, key);
    int r = cJSON_IsNumber(v) ? v->valueint : def;
    xSemaphoreGive(s_lock);
    return r;
}

double settings_get_num(const char *group, const char *key, double def)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const cJSON *v = peek(group, key);
    double r = cJSON_IsNumber(v) ? v->valuedouble : def;
    xSemaphoreGive(s_lock);
    return r;
}

bool settings_get_bool(const char *group, const char *key, bool def)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const cJSON *v = peek(group, key);
    bool r = cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
    xSemaphoreGive(s_lock);
    return r;
}

char *settings_get_str(const char *group, const char *key, char *buf, size_t len, const char *def)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const cJSON *v = peek(group, key);
    snprintf(buf, len, "%s", cJSON_IsString(v) ? v->valuestring : (def ? def : ""));
    xSemaphoreGive(s_lock);
    return buf;
}

void settings_on_change(settings_cb_t cb, void *arg)
{
    for (int i = 0; i < MAX_CB; i++) {
        if (!s_cbs[i].cb) {
            s_cbs[i].arg = arg;
            s_cbs[i].cb = cb;
            return;
        }
    }
}

esp_err_t settings_factory_reset(void)
{
    esp_vfs_littlefs_unregister("data");
    esp_err_t r = esp_littlefs_format("data");
    ESP_LOGW(TAG, "factory reset: %s", esp_err_to_name(r));
    return r;
}
