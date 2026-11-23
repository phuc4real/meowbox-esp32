#include "webui.h"

#include <stddef.h>
#include <string.h>
#include "esp_log.h"

/* Pack layout: see web/build.py. */
#define PATH_MAX_LEN 56
#define F_GZIP 1
#define F_IMMUTABLE 2

typedef struct {
    char path[PATH_MAX_LEN];
    uint32_t off, len, flags;
    char etag[12];
} entry_t;
_Static_assert(sizeof(entry_t) == 80, "pack entry must be 80 bytes");

typedef struct {
    char magic[4];
    uint32_t version, count, data_off;
} head_t;

extern const uint8_t s_pack_start[] asm("_binary_web_pack_start");
extern const uint8_t s_pack_end[] asm("_binary_web_pack_end");

static const char *TAG = "webui";

/* The embedded blob has no alignment guarantee: copy structs out with memcpy instead of casting. */
static bool s_ok;
static head_t s_head;

static void entry(uint32_t i, entry_t *e) { memcpy(e, s_pack_start + sizeof(head_t) + i * sizeof(entry_t), sizeof(*e)); }

static bool load(void)
{
    static int done;
    if (done) return s_ok;
    done = 1;
    size_t size = s_pack_end - s_pack_start;
    if (size < sizeof(head_t)) goto bad;
    memcpy(&s_head, s_pack_start, sizeof(s_head));
    if (memcmp(s_head.magic, "MBWP", 4) != 0 || s_head.version != 1 ||
        s_head.data_off != sizeof(head_t) + s_head.count * sizeof(entry_t) || s_head.data_off > size)
        goto bad;
    for (uint32_t i = 0; i < s_head.count; i++) {
        entry_t e;
        entry(i, &e);
        if (e.path[PATH_MAX_LEN - 1] != 0 || e.etag[11] != 0 || (size_t)s_head.data_off + e.off + e.len > size) goto bad;
    }
    ESP_LOGI(TAG, "%lu files, %u bytes", (unsigned long)s_head.count, (unsigned)size);
    return s_ok = true;
bad:
    ESP_LOGE(TAG, "web.pack is missing or corrupt");
    return s_ok = false;
}

int webui_count(void) { return load() ? (int)s_head.count : 0; }

bool webui_find(const char *path, size_t len, webui_file_t *out)
{
    if (!load() || len == 0 || len >= PATH_MAX_LEN) return false;
    for (uint32_t i = 0; i < s_head.count; i++) {
        const char *p = (const char *)(s_pack_start + sizeof(head_t) + i * sizeof(entry_t));
        if (strncmp(p, path, len) != 0 || p[len] != 0) continue;
        entry_t e;
        entry(i, &e);
        out->data = s_pack_start + s_head.data_off + e.off;
        out->len = e.len;
        out->gzip = e.flags & F_GZIP;
        out->immutable = e.flags & F_IMMUTABLE;
        out->etag = p + offsetof(entry_t, etag); /* NUL-terminated in the pack (checked in load) */
        return true;
    }
    return false;
}
