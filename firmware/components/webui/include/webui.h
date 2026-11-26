#pragma once
/* Web UI files embedded in the app (components/webui/web.pack, written by web/build.py). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *data;  /* in flash (memory-mapped), valid for the lifetime of the app */
    uint32_t len;
    bool gzip;            /* stored gzipped: send with Content-Encoding: gzip */
    bool immutable;       /* name carries a content hash: cache forever */
    const char *etag;     /* short hash of the stored bytes, NUL-terminated */
} webui_file_t;

/* Look up a served path (without leading '/', e.g. "index.html", "app.3f9a1c2b.js"); len = chars of path to use. */
bool webui_find(const char *path, size_t len, webui_file_t *out);
