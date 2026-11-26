#include "player.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_audio_dec_default.h"
#include "audio_hw.h"
#include "utf8.h"

static const char *TAG = "player";

#define RB_SIZE (256 * 1024)
#define URL_MAX 512

typedef enum { FMT_UNKNOWN, FMT_MP3, FMT_AAC, FMT_TS, FMT_WAV } fmt_t;

static SemaphoreHandle_t s_lock;
static RingbufHandle_t s_rb;
static volatile uint32_t s_gen;          /* session generation; bump to stop */
static volatile player_state_t s_state = PLAYER_STOP;
static volatile fmt_t s_fmt;
static volatile bool s_eof;              /* fetcher finished (non-live file) */
static TaskHandle_t s_dec_task;
static char s_url[URL_MAX], s_name[64], s_title[128];
static int s_id;
static uint8_t *s_mem;
static size_t s_mem_len;
static volatile bool s_is_mem;

/* Stream numbers for /api/status and the System tile (docs/firmware_v0.2.0.md, 4.3). Reset per session. */
static volatile uint32_t s_rx;           /* bytes fetched */
static volatile uint16_t s_under;        /* underruns while playing */
static volatile uint16_t s_reconn;       /* reconnects */
static volatile int s_icy_br, s_dec_br;  /* kbps from the icy-br header / the decoder, 0 = unknown */
static volatile bool s_hls;
static volatile uint16_t s_kbps;         /* download rate, 5 s average (decode task) */
static char s_err[16];                   /* last error: connect, http_404, read, decode */

/* Adaptive prebuffer (4.2): seconds of audio to collect before playing. One step up after each underrun,
 * one step down after 5 min without one; kept across stations, reset when Wi-Fi (re)connects. */
static const uint8_t PRE_S[] = {3, 6, 10};
static volatile uint8_t s_lvl;
static int64_t s_clean_since;            /* us; playing without underrun since (decode task) */

/* ---------------- small helpers ---------------- */
static void set_state(player_state_t st)
{
    if (s_state != st) ESP_LOGI(TAG, "state %s -> %d", player_state_str(), st);
    s_state = st;
}

static bool alive(uint32_t gen) { return gen == s_gen; }

/* State change from a worker task (fetch / decode): only if its session is still current. Checked and set under
 * s_lock, the lock player_play/player_stop hold while they start a new session: a plain alive() check followed by
 * set_state() let a decode pass that was waiting on the ring buffer overwrite a fresh STOP with BUF. */
static void set_state_gen(uint32_t gen, player_state_t st)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (gen == s_gen) set_state(st);
    xSemaphoreGive(s_lock);
}

static void set_err(uint32_t gen, const char *fmt, int v)
{
    if (!alive(gen)) return;
    snprintf(s_err, sizeof(s_err), fmt, v);
}

static int stream_kbps(void) { return s_icy_br > 0 ? s_icy_br : s_dec_br > 0 ? s_dec_br : 128; }

static size_t pre_bytes(void)
{
    size_t b = (size_t)PRE_S[s_lvl] * stream_kbps() * 125; /* kbit/s -> bytes per second */
    return b > RB_SIZE * 7 / 10 ? RB_SIZE * 7 / 10 : b;
}

/* Session check and send under s_lock, the lock player_play / player_stop flush the ring under: a blocking send
 * started before a flush used to land after it, putting a few KB of the old stream in front of the new one (an
 * audio notification then failed with "Not a WAV file" halfway through). */
static void rb_put(uint32_t gen, const uint8_t *d, size_t n)
{
    while (n) {
        size_t chunk = n > 4096 ? 4096 : n;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (!alive(gen)) {
            xSemaphoreGive(s_lock);
            return;
        }
        BaseType_t ok = xRingbufferSend(s_rb, d, chunk, 0);
        xSemaphoreGive(s_lock);
        if (ok == pdTRUE) {
            s_rx += chunk;
            d += chunk;
            n -= chunk;
        } else {
            vTaskDelay(pdMS_TO_TICKS(20)); /* ring full: let the decoder catch up */
        }
    }
}

static void rb_flush(void)
{
    size_t n;
    void *p;
    while ((p = xRingbufferReceiveUpTo(s_rb, &n, 0, RB_SIZE)) != NULL) vRingbufferReturnItem(s_rb, p);
}

static fmt_t fmt_from(const char *ctype, const char *url)
{
    char c[64] = "";
    for (int i = 0; ctype && ctype[i] && i < 63; i++) c[i] = (char)tolower((int)ctype[i]);
    if (strstr(c, "mpeg") && !strstr(c, "mpegurl")) return FMT_MP3;
    if (strstr(c, "aac") || strstr(c, "mp4a")) return FMT_AAC;
    if (strstr(c, "mp2t")) return FMT_TS;
    if (strstr(url, ".mp3")) return FMT_MP3;
    if (strstr(url, ".aac")) return FMT_AAC;
    if (strstr(url, ".ts")) return FMT_TS;
    return FMT_UNKNOWN;
}

static bool is_playlist(const char *ctype, const char *url)
{
    char c[64] = "";
    for (int i = 0; ctype && ctype[i] && i < 63; i++) c[i] = (char)tolower((int)ctype[i]);
    return strstr(c, "mpegurl") || strstr(c, "scpls") || strstr(url, ".m3u") || strstr(url, ".pls");
}

/* Resolve `ref` relative to `base` into out. */
static void resolve_url(const char *base, const char *ref, char *out, size_t len)
{
    if (strstr(ref, "://")) {
        snprintf(out, len, "%s", ref);
        return;
    }
    if (ref[0] == '/') {
        const char *p = strstr(base, "://");
        p = p ? strchr(p + 3, '/') : NULL;
        int hostlen = p ? (int)(p - base) : (int)strlen(base);
        snprintf(out, len, "%.*s%s", hostlen, base, ref);
        return;
    }
    const char *q = strchr(base, '?');
    int blen = q ? (int)(q - base) : (int)strlen(base);
    const char *slash = base + blen;
    while (slash > base && *slash != '/') slash--;
    snprintf(out, len, "%.*s/%s", (int)(slash - base), base, ref);
}

/* ---------------- HTTP ---------------- */
typedef struct { char ctype[64]; int metaint; } resp_hdrs_t;
static resp_hdrs_t s_hdrs; /* response headers of the last http_open() (fetch task only) */

static esp_err_t on_http_evt(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_HEADER && e->header_key && e->header_value) {
        if (strcasecmp(e->header_key, "Content-Type") == 0) snprintf(s_hdrs.ctype, sizeof(s_hdrs.ctype), "%s", e->header_value);
        else if (strcasecmp(e->header_key, "icy-metaint") == 0) s_hdrs.metaint = atoi(e->header_value);
        else if (strcasecmp(e->header_key, "icy-br") == 0) s_icy_br = atoi(e->header_value);
    }
    return ESP_OK;
}

static esp_http_client_handle_t http_open(const char *url, bool icy, int *status, int64_t *clen)
{
    memset(&s_hdrs, 0, sizeof(s_hdrs));
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 8000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .user_agent = "MeowBox/0.1",
        .keep_alive_enable = true,
        .event_handler = on_http_evt,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return NULL;
    if (icy) esp_http_client_set_header(c, "Icy-MetaData", "1");
    for (int redirects = 0; redirects < 5; redirects++) {
        if (esp_http_client_open(c, 0) != ESP_OK) break;
        *clen = esp_http_client_fetch_headers(c);
        *status = esp_http_client_get_status_code(c);
        if (*status >= 300 && *status < 400) {
            memset(&s_hdrs, 0, sizeof(s_hdrs));
            esp_http_client_set_redirection(c);
            esp_http_client_close(c);
            continue;
        }
        return c;
    }
    esp_http_client_cleanup(c);
    return NULL;
}

/* Read a whole small text body (playlists). */
static char *http_get_text(const char *url, char *final_url, size_t fu_len)
{
    int st = 0;
    int64_t cl = 0;
    esp_http_client_handle_t c = http_open(url, false, &st, &cl);
    if (!c) return NULL;
    char *buf = NULL;
    if (st == 200) {
        int cap = 16384, n = 0, r;
        buf = heap_caps_malloc(cap + 1, MALLOC_CAP_SPIRAM);
        while (buf && n < cap && (r = esp_http_client_read(c, buf + n, cap - n)) > 0) n += r;
        if (buf) buf[n] = 0;
        if (final_url) esp_http_client_get_url(c, final_url, fu_len);
    }
    esp_http_client_cleanup(c);
    return buf;
}

/* First stream URL in a .pls/.m3u (non-HLS) playlist. */
static bool first_entry(const char *text, char *out, size_t len)
{
    const char *p = text;
    while (p && *p) {
        const char *e = strpbrk(p, "\r\n");
        int l = e ? (int)(e - p) : (int)strlen(p);
        const char *line = p;
        if (strncasecmp(line, "File", 4) == 0 && memchr(line, '=', l)) {
            const char *eq = memchr(line, '=', l);
            snprintf(out, len, "%.*s", (int)(l - (eq + 1 - line)), eq + 1);
            return true;
        }
        if (l > 4 && line[0] != '#' && (strncmp(line, "http", 4) == 0)) {
            snprintf(out, len, "%.*s", l, line);
            return true;
        }
        p = e ? e + 1 : NULL;
        while (p && (*p == '\r' || *p == '\n')) p++;
    }
    return false;
}

/* ---------------- ICY stream ---------------- */
static void parse_icy_meta(const char *m)
{
    const char *t = strstr(m, "StreamTitle='");
    if (!t) return;
    t += 13;
    const char *e = strstr(t, "';");
    int n = e ? (int)(e - t) : (int)strlen(t);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_title, sizeof(s_title), "%.*s", n, t);
    utf8_trim(s_title);
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "title: %s", s_title);
}

static void stream_loop(uint32_t gen, esp_http_client_handle_t c, int metaint)
{
    uint8_t *buf = malloc(4096);
    if (!buf) {
        set_err(gen, "no_mem", 0);
        return;
    }
    int until_meta = metaint;
    /* stall check: while buffering, under a quarter of the bitrate for 10 s -> reconnect (servers that trickle
     * bytes never hit the read timeout; a fresh connection usually comes back at full speed) */
    int64_t win_t = esp_timer_get_time();
    uint32_t win_rx = s_rx;
    while (alive(gen)) {
        int64_t now = esp_timer_get_time();
        if (now - win_t >= 10 * 1000000) {
            uint32_t kbps = (s_rx - win_rx) * 8 / 10 / 1000;
            if (s_state == PLAYER_BUF && kbps < (uint32_t)stream_kbps() / 4) {
                ESP_LOGW(TAG, "stalled (%lu kbps while buffering), reconnecting", (unsigned long)kbps);
                set_err(gen, "stall", 0);
                break;
            }
            win_t = now;
            win_rx = s_rx;
        }
        int want = 4096;
        if (metaint && until_meta < want) want = until_meta;
        int r = esp_http_client_read(c, (char *)buf, want);
        if (r <= 0) {
            if (esp_http_client_is_complete_data_received(c)) s_eof = true;
            else set_err(gen, "read", 0);
            break;
        }
        rb_put(gen, buf, r);
        if (metaint) {
            until_meta -= r;
            if (until_meta == 0) {
                uint8_t lenb;
                if (esp_http_client_read(c, (char *)&lenb, 1) != 1) break;
                int ml = lenb * 16, got = 0;
                static char *meta; /* 4 KB in PSRAM; only the fetch task of the live session uses it */
                if (!meta) meta = heap_caps_malloc(4081, MALLOC_CAP_SPIRAM);
                if (!meta) break;
                while (got < ml) {
                    int k = esp_http_client_read(c, meta + got, ml - got);
                    if (k <= 0) break;
                    got += k;
                }
                meta[got] = 0;
                if (got) parse_icy_meta(meta);
                until_meta = metaint;
            }
        }
    }
    free(buf);
}

/* ---------------- HLS ---------------- */
static void hls_loop(uint32_t gen, const char *playlist_url)
{
    char *url = heap_caps_malloc(URL_MAX, MALLOC_CAP_SPIRAM);
    char *seg = heap_caps_malloc(URL_MAX, MALLOC_CAP_SPIRAM);
    long long last_seq = -1;
    int target = 6;
    uint8_t *buf = malloc(4096);
    int fails = buf && url && seg ? 0 : 5; /* out of memory: skip the loop */
    if (fails) set_err(gen, "no_mem", 0);
    else snprintf(url, URL_MAX, "%s", playlist_url);
    s_hls = true;
    while (alive(gen) && fails < 5) {
        char final[URL_MAX];
        char *txt = http_get_text(url, final, sizeof(final));
        if (!txt) {
            set_err(gen, "connect", 0);
            fails++;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        /* master playlist: take the first variant */
        if (strstr(txt, "#EXT-X-STREAM-INF")) {
            const char *p = strstr(txt, "#EXT-X-STREAM-INF");
            p = strchr(p, '\n');
            while (p && (*p == '\n' || *p == '\r')) p++;
            if (p) {
                int l = (int)strcspn(p, "\r\n");
                char ref[URL_MAX];
                snprintf(ref, sizeof(ref), "%.*s", l, p);
                resolve_url(final, ref, url, URL_MAX);
                ESP_LOGI(TAG, "hls variant %s", url);
            }
            free(txt);
            continue;
        }
        const char *ms = strstr(txt, "#EXT-X-MEDIA-SEQUENCE:");
        long long seq = ms ? atoll(ms + 22) : 0;
        const char *td = strstr(txt, "#EXT-X-TARGETDURATION:");
        if (td) target = atoi(td + 22);
        if (target < 1 || target > 30) target = 6;
        bool endlist = strstr(txt, "#EXT-X-ENDLIST") != NULL;
        /* on first load of a live list, start 3 segments from the end */
        int total = 0;
        for (const char *p = txt; (p = strstr(p, "#EXTINF")); p++) total++;
        if (last_seq < 0 && !endlist && total > 3) last_seq = seq + total - 4;
        const char *p = txt;
        int idx = 0;
        while (alive(gen) && (p = strstr(p, "#EXTINF"))) {
            p = strchr(p, '\n');
            while (p && (*p == '\n' || *p == '\r' || *p == '#')) {
                if (*p == '#') p = strchr(p, '\n'); /* skip other tags */
                else p++;
            }
            if (!p) break;
            int l = (int)strcspn(p, "\r\n");
            long long sn = seq + idx++;
            if (sn <= last_seq) { p += l; continue; }
            char ref[URL_MAX];
            snprintf(ref, sizeof(ref), "%.*s", l, p);
            p += l;
            resolve_url(final, ref, seg, URL_MAX);
            if (s_fmt == FMT_UNKNOWN) s_fmt = strstr(seg, ".aac") ? FMT_AAC : FMT_TS;
            int st = 0;
            int64_t cl = 0;
            esp_http_client_handle_t c = http_open(seg, false, &st, &cl);
            if (c && st == 200) {
                int r;
                while (alive(gen) && (r = esp_http_client_read(c, (char *)buf, 4096)) > 0) rb_put(gen, buf, r);
                fails = 0;
            } else {
                ESP_LOGW(TAG, "segment %lld http %d", sn, st);
                if (st) set_err(gen, "http_%d", st);
                else set_err(gen, "connect", 0);
                fails++;
            }
            if (c) esp_http_client_cleanup(c);
            last_seq = sn;
        }
        free(txt);
        if (endlist) { s_eof = true; break; }
        for (int i = 0; i < target * 5 && alive(gen); i++) vTaskDelay(pdMS_TO_TICKS(100)); /* ~half target */
    }
    free(buf);
    free(url);
    free(seg);
}

/* ---------------- fetch task (one per session) ---------------- */
static volatile int s_fetch_n; /* fetch tasks alive; an old one leaves at its next alive() check */

static void fetch_exit(void)
{
    __atomic_sub_fetch(&s_fetch_n, 1, __ATOMIC_SEQ_CST);
    vTaskDeleteWithCaps(NULL);
}

static void fetch_task(void *arg)
{
    uint32_t gen = (uint32_t)(uintptr_t)arg;
    /* one connection at a time: rapid stop/play used to run several TLS handshakes at once on core 0, long enough
     * to trip the idle task watchdog */
    while (__atomic_load_n(&s_fetch_n, __ATOMIC_SEQ_CST) > 1 && alive(gen)) vTaskDelay(pdMS_TO_TICKS(50));
    if (!alive(gen)) {
        fetch_exit();
        return;
    }
    char *url = heap_caps_malloc(URL_MAX, MALLOC_CAP_SPIRAM);
    if (!url) {
        set_state_gen(gen, PLAYER_ERR);
        fetch_exit();
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(url, URL_MAX, "%s", s_url);
    xSemaphoreGive(s_lock);

    int retries = 0;
    while (alive(gen) && retries < 8) {
        int status = 0;
        int64_t clen = 0;
        bool hls = strstr(url, ".m3u8") != NULL;
        if (hls) {
            hls_loop(gen, url);
        } else {
            esp_http_client_handle_t c = http_open(url, true, &status, &clen);
            if (c && status == 200) {
                const char *ctype = s_hdrs.ctype[0] ? s_hdrs.ctype : NULL;
                int metaint = s_hdrs.metaint;
                if (is_playlist(ctype, url) && !(ctype && strstr(ctype, "audio/mpeg"))) {
                    char *txt = calloc(1, 8193);
                    if (!txt) {
                        esp_http_client_cleanup(c);
                        break;
                    }
                    int n = 0, r;
                    while (n < 8192 && (r = esp_http_client_read(c, txt + n, 8192 - n)) > 0) n += r;
                    esp_http_client_cleanup(c);
                    char next[URL_MAX];
                    bool is_hls = strstr(txt, "#EXTM3U") && (strstr(txt, "#EXT-X-") != NULL);
                    if (is_hls) {
                        free(txt);
                        snprintf(url + strlen(url), URL_MAX - strlen(url), "%s", strchr(url, '?') ? "&.m3u8" : "?.m3u8");
                        continue;
                    }
                    bool ok = first_entry(txt, next, sizeof(next));
                    free(txt);
                    if (!ok) break;
                    char abs[URL_MAX];
                    resolve_url(url, next, abs, sizeof(abs));
                    snprintf(url, URL_MAX, "%s", abs);
                    ESP_LOGI(TAG, "playlist -> %s", url);
                    continue;
                }
                fmt_t f = fmt_from(ctype, url);
                s_fmt = f == FMT_UNKNOWN ? FMT_MP3 : f;
                ESP_LOGI(TAG, "stream %s type=%s fmt=%d metaint=%d", url, ctype ? ctype : "?", s_fmt, metaint);
                stream_loop(gen, c, metaint);
                esp_http_client_cleanup(c);
            } else {
                ESP_LOGW(TAG, "open %s failed (http %d)", url, status);
                if (status) set_err(gen, "http_%d", status);
                else set_err(gen, "connect", 0);
                if (c) esp_http_client_cleanup(c);
            }
        }
        if (s_eof || !alive(gen)) break;
        retries++;
        if (alive(gen)) s_reconn++;
        int wait = retries < 3 ? 1000 : retries < 5 ? 3000 : 8000;
        for (int i = 0; i < wait / 100 && alive(gen); i++) vTaskDelay(pdMS_TO_TICKS(100));
        ESP_LOGI(TAG, "reconnecting (%d)", retries);
    }
    if (!s_eof) set_state_gen(gen, PLAYER_ERR);
    free(url);
    fetch_exit();
}

/* ---------------- decode task (persistent) ---------------- */
static void write_pcm(const int16_t *in, int frames, int ch, int rate, float *pos, int16_t *last)
{
    /* downmix to mono and resample (linear) to the output rate */
    static int16_t out[1024];
    const int orate = audio_hw_sample_rate();
    float step = (float)rate / orate;
    int n = 0;
    float p = *pos;
    while (p < frames) {
        int i = (int)p;
        float fr = p - i;
        int a, b;
        if (ch == 1) { a = i ? in[i - 1] : *last; b = in[i]; }
        else { a = i ? (in[(i - 1) * ch] + in[(i - 1) * ch + 1]) / 2 : *last; b = (in[i * ch] + in[i * ch + 1]) / 2; }
        out[n++] = (int16_t)(a + (b - a) * fr);
        if (n == 1024) {
            audio_hw_write(out, n);
            n = 0;
        }
        p += step;
    }
    if (n) audio_hw_write(out, n);
    *pos = p - frames;
    *last = ch == 1 ? in[frames - 1] : (in[(frames - 1) * ch] + in[(frames - 1) * ch + 1]) / 2;
}

static void decode_task(void *arg)
{
    esp_audio_dec_register_default();
    esp_audio_simple_dec_register_default();
    const int IN_CAP = 8192;
    uint8_t *in = heap_caps_malloc(IN_CAP, MALLOC_CAP_SPIRAM); /* compressed input, read sequentially: PSRAM is fine */
    uint32_t out_cap = 16384;
    uint8_t *out = heap_caps_malloc(out_cap, MALLOC_CAP_SPIRAM);
    uint32_t cur_gen = 0;
    esp_audio_simple_dec_handle_t dec = NULL;
    int in_len = 0;
    float pos = 0;
    int16_t last = 0;
    esp_audio_simple_dec_info_t info = {0};
    uint32_t rx_hist[5] = {0}, rx_last = 0;
    int64_t tick = 0, last_log = 0;
    bool want_more = false; /* the decoder asked for more input than half a buffer */
    int idle_loops = 0;     /* passes without consuming or producing anything */

    while (1) {
        int64_t now = esp_timer_get_time();
        if (now - tick >= 1000000) { /* once a second: download rate, step the prebuffer down, log */
            tick = now;
            uint32_t rx = s_rx;
            memmove(rx_hist, rx_hist + 1, sizeof(rx_hist) - sizeof(rx_hist[0]));
            rx_hist[4] = rx - rx_last;
            rx_last = rx;
            uint32_t sum = 0;
            for (int i = 0; i < 5; i++) sum += rx_hist[i];
            s_kbps = sum * 8 / 5 / 1000;
            if (s_state == PLAYER_PLAY && s_lvl > 0 && now - s_clean_since > 300LL * 1000000) {
                s_lvl--;
                s_clean_since = now;
                ESP_LOGI(TAG, "5 min without underrun, prebuffer %d s", PRE_S[s_lvl]);
            }
            if ((s_state == PLAYER_PLAY || s_state == PLAYER_BUF) && !s_is_mem && now - last_log >= 30LL * 1000000) {
                last_log = now;
                player_net_t n;
                if (player_net(&n))
                    ESP_LOGI(TAG, "net: %u/%u kbps, buffer %u.%u s, prebuffer %u s, underruns %u, reconnects %u%s%s",
                             n.kbps, n.br, n.buf_ds / 10, n.buf_ds % 10, n.pre_s, n.under, n.reconn,
                             n.err[0] ? ", last error " : "", n.err);
            }
        }
        if (s_state == PLAYER_STOP || s_state == PLAYER_ERR) {
            if (dec) { esp_audio_simple_dec_close(dec); dec = NULL; }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
            continue;
        }
        if (cur_gen != s_gen) { /* new session */
            if (dec) { esp_audio_simple_dec_close(dec); dec = NULL; }
            cur_gen = s_gen;
            rx_last = s_rx;
            memset(rx_hist, 0, sizeof(rx_hist));
            in_len = 0;
            want_more = false;
            pos = 0;
            last = 0;
            memset(&info, 0, sizeof(info));
        }
        size_t avail = 0;
        vRingbufferGetInfo(s_rb, NULL, NULL, NULL, NULL, &avail);
        avail = RB_SIZE - xRingbufferGetCurFreeSize(s_rb);
        if (s_state == PLAYER_BUF) {
            if (avail < pre_bytes() && !s_eof) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
            if (s_fmt == FMT_UNKNOWN) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
            s_clean_since = esp_timer_get_time();
            set_state_gen(cur_gen, PLAYER_PLAY);
        }
        if (!dec) {
            esp_audio_simple_dec_cfg_t cfg = {
                .dec_type = s_fmt == FMT_WAV ? ESP_AUDIO_SIMPLE_DEC_TYPE_WAV
                          : s_fmt == FMT_AAC ? ESP_AUDIO_SIMPLE_DEC_TYPE_AAC
                          : s_fmt == FMT_TS  ? ESP_AUDIO_SIMPLE_DEC_TYPE_TS
                                             : ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
            };
            if (esp_audio_simple_dec_open(&cfg, &dec) != ESP_AUDIO_ERR_OK) {
                ESP_LOGE(TAG, "decoder open failed (fmt %d)", s_fmt);
                snprintf(s_err, sizeof(s_err), "decode");
                set_state_gen(cur_gen, PLAYER_ERR);
                continue;
            }
        }
        /* top up input */
        if (in_len < IN_CAP / 2 || (want_more && in_len < IN_CAP)) {
            size_t n = 0;
            uint8_t *p = xRingbufferReceiveUpTo(s_rb, &n, pdMS_TO_TICKS(100), IN_CAP - in_len);
            if (p) {
                memcpy(in + in_len, p, n);
                in_len += n;
                vRingbufferReturnItem(s_rb, p);
            } else if (in_len == 0) {
                if (s_eof) {
                    s_is_mem = false;
                    set_state_gen(cur_gen, PLAYER_STOP);
                    continue;
                }
                if (s_state == PLAYER_PLAY && !s_is_mem) {
                    s_under++;
                    if (s_lvl < sizeof(PRE_S) - 1) s_lvl++;
                    ESP_LOGW(TAG, "underrun %u, prebuffer now %d s", s_under, PRE_S[s_lvl]);
                } else ESP_LOGW(TAG, "underrun");
                set_state_gen(cur_gen, PLAYER_BUF);
                continue;
            }
        }
        esp_audio_simple_dec_raw_t raw = {.buffer = in, .len = in_len, .eos = false};
        esp_audio_simple_dec_out_t o = {.buffer = out, .len = out_cap};
        esp_audio_err_t r = esp_audio_simple_dec_process(dec, &raw, &o);
        if (r == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            out_cap = o.needed_size + 1024;
            free(out);
            out = heap_caps_malloc(out_cap, MALLOC_CAP_SPIRAM);
            continue;
        }
        if (raw.consumed > 0) {
            memmove(in, in + raw.consumed, in_len - raw.consumed);
            in_len -= raw.consumed;
        } else if (r != ESP_AUDIO_ERR_OK && r != ESP_AUDIO_ERR_DATA_LACK) {
            in_len = in_len > 1 ? in_len - 1 : 0; /* skip a byte to resync */
            if (in_len) memmove(in, in + 1, in_len);
        }
        if (o.decoded_size > 0) {
            if (info.sample_rate == 0) {
                esp_audio_simple_dec_get_info(dec, &info);
                if (info.bitrate >= 8000) s_dec_br = info.bitrate / 1000;
                ESP_LOGI(TAG, "decoding %lu Hz, %u ch, %u bit, %lu kbps", (unsigned long)info.sample_rate, info.channel,
                         info.bits_per_sample, (unsigned long)(info.bitrate / 1000));
            }
            if (info.bits_per_sample == 16 && info.channel >= 1 && info.sample_rate) {
                int frames = o.decoded_size / (2 * info.channel);
                write_pcm((int16_t *)out, frames, info.channel, info.sample_rate, &pos, &last);
            }
        } else if (r == ESP_AUDIO_ERR_DATA_LACK && in_len >= IN_CAP - 16) {
            in_len = 0; /* garbage that never decodes */
        }
        /* DATA_LACK with nothing consumed: top up next pass even above half full. Without this an AAC stream could
         * leave 4-8 KB that never decodes and the loop spun at priority 7, starving the UI task on this core. */
        bool progress = raw.consumed > 0 || o.decoded_size > 0;
        want_more = !progress && r == ESP_AUDIO_ERR_DATA_LACK;
        if (progress) {
            idle_loops = 0;
        } else if (++idle_loops >= 8) { /* safety net: never hog the core */
            idle_loops = 0;
            vTaskDelay(1);
        }
    }
}

/* ---------------- in-memory files ---------------- */
static void mem_task(void *arg)
{
    uint32_t gen = (uint32_t)(uintptr_t)arg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint8_t *d = s_mem;
    size_t n = s_mem_len;
    s_mem = NULL;
    xSemaphoreGive(s_lock);
    if (d) {
        rb_put(gen, d, n);
        free(d);
    }
    if (alive(gen)) s_eof = true;
    vTaskDelete(NULL);
}

esp_err_t player_play_mem(uint8_t *data, size_t len, const char *name)
{
    if (!data || len < 16) { free(data); return ESP_ERR_INVALID_ARG; }
    fmt_t f = memcmp(data, "RIFF", 4) == 0 ? FMT_WAV
            : (memcmp(data, "ID3", 3) == 0 || (data[0] == 0xFF && (data[1] & 0xE0) == 0xE0 && (data[1] & 0x06) != 0)) ? FMT_MP3
            : (data[0] == 0xFF && (data[1] & 0xF6) == 0xF0) ? FMT_AAC : FMT_UNKNOWN;
    if (f == FMT_UNKNOWN) { free(data); return ESP_ERR_NOT_SUPPORTED; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_gen++;
    rb_flush();
    free(s_mem);
    s_mem = data;
    s_mem_len = len;
    snprintf(s_name, sizeof(s_name), "%s", name ? name : "");
    s_title[0] = 0;
    s_url[0] = 0;
    s_id = 0;
    s_fmt = f;
    s_eof = false;
    s_is_mem = true;
    uint32_t gen = s_gen;
    set_state(PLAYER_BUF);
    xSemaphoreGive(s_lock);
    if (xTaskCreatePinnedToCore(mem_task, "memplay", 3072, (void *)(uintptr_t)gen, 5, NULL, 0) != pdPASS) {
        set_state(PLAYER_ERR);
        return ESP_ERR_NO_MEM;
    }
    xTaskNotifyGive(s_dec_task);
    return ESP_OK;
}

bool player_is_mem(void) { return s_is_mem; }

/* ---------------- API ---------------- */
static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    s_lvl = 0; /* new network: start again from the smallest prebuffer */
}

void player_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_rb = xRingbufferCreateWithCaps(RB_SIZE, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL);
    /* measured peak use ~1.5 KB (docs/perf/results-0.2.0.md); stays in internal RAM (real-time, high priority) */
    xTaskCreatePinnedToCore(decode_task, "decode", 4096, NULL, 7, &s_dec_task, 1);
}

esp_err_t player_play(const char *url, const char *name, int id)
{
    if (!url || !*url || strlen(url) >= URL_MAX) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_gen++;
    rb_flush();
    snprintf(s_url, sizeof(s_url), "%s", url);
    snprintf(s_name, sizeof(s_name), "%s", name ? name : "");
    s_title[0] = 0;
    s_id = id;
    s_fmt = FMT_UNKNOWN;
    s_eof = false;
    s_is_mem = false;
    s_rx = 0;
    s_under = s_reconn = 0;
    s_icy_br = s_dec_br = 0;
    s_kbps = 0;
    s_hls = strstr(url, ".m3u8") != NULL;
    s_err[0] = 0;
    uint32_t gen = s_gen;
    set_state(PLAYER_BUF);
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "play %s (%s)", url, name ? name : "");
    /* stack in PSRAM: fetch only does network I/O, never writes flash (a PSRAM stack must not) */
    __atomic_add_fetch(&s_fetch_n, 1, __ATOMIC_SEQ_CST);
    if (xTaskCreatePinnedToCoreWithCaps(fetch_task, "fetch", 8192, (void *)(uintptr_t)gen, 5, NULL, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
        __atomic_sub_fetch(&s_fetch_n, 1, __ATOMIC_SEQ_CST);
        set_state(PLAYER_ERR);
        return ESP_ERR_NO_MEM;
    }
    xTaskNotifyGive(s_dec_task);
    return ESP_OK;
}

void player_stop(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_gen++;
    rb_flush();
    s_title[0] = 0;
    s_is_mem = false;
    set_state(PLAYER_STOP);
    xSemaphoreGive(s_lock);
}

player_state_t player_state(void) { return s_state; }

const char *player_state_str(void)
{
    switch (s_state) {
    case PLAYER_BUF: return "buf";
    case PLAYER_PLAY: return "play";
    case PLAYER_ERR: return "err";
    default: return "stop";
    }
}

int player_id(void) { return s_id; }

void player_name(char *buf, size_t len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(buf, len, "%s", s_name);
    xSemaphoreGive(s_lock);
}

void player_title(char *buf, size_t len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(buf, len, "%s", s_title);
    xSemaphoreGive(s_lock);
}

static const char *fmt_str(void)
{
    if (s_hls) return "hls";
    switch (s_fmt) {
    case FMT_MP3: return "mp3";
    case FMT_AAC: return "aac";
    case FMT_TS: return "ts";
    case FMT_WAV: return "wav";
    default: return "";
    }
}

bool player_net(player_net_t *o)
{
    if (s_is_mem || (s_state != PLAYER_PLAY && s_state != PLAYER_BUF && s_state != PLAYER_ERR)) return false;
    size_t avail = s_rb ? RB_SIZE - xRingbufferGetCurFreeSize(s_rb) : 0;
    size_t pre = pre_bytes();
    o->kbps = s_kbps;
    o->br = s_icy_br > 0 ? s_icy_br : s_dec_br;
    o->buf_ds = (uint16_t)(avail * 8 / (stream_kbps() * 100)); /* bits / (kbit/s * 100) = 0.1 s units */
    o->pre_s = PRE_S[s_lvl];
    o->under = s_under;
    o->reconn = s_reconn;
    o->fill_pct = s_state == PLAYER_BUF ? (uint8_t)(avail >= pre ? 100 : avail * 100 / pre) : 100;
    o->fmt = fmt_str();
    snprintf(o->err, sizeof(o->err), "%s", s_err);
    return true;
}

int player_buffer_pct(void)
{
    if (!s_rb) return 0;
    return (int)((RB_SIZE - xRingbufferGetCurFreeSize(s_rb)) * 100 / RB_SIZE);
}

