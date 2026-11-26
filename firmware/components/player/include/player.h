/*
 * Network audio player: HTTP/HTTPS streams (MP3, AAC/ADTS, ICY metadata), .pls/.m3u playlists and HLS
 * (.m3u8 with TS or AAC segments). PCM goes to audio_hw (mono, 44.1 kHz) after downmix + resampling.
 * All functions are thread-safe.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { PLAYER_STOP, PLAYER_BUF, PLAYER_PLAY, PLAYER_ERR } player_state_t;

void player_init(void);
/* Starts playing url (stops the current stream). name/id are just remembered for the UI/API. */
esp_err_t player_play(const char *url, const char *name, int id);
void player_stop(void);
/* Plays an in-memory WAV/MP3/AAC file (takes ownership of data, freed with free()). */
esp_err_t player_play_mem(uint8_t *data, size_t len, const char *name);
bool player_is_mem(void);
player_state_t player_state(void);
const char *player_state_str(void);        /* "stop" | "buf" | "play" | "err" */
int player_id(void);
void player_name(char *buf, size_t len);
void player_title(char *buf, size_t len);  /* ICY StreamTitle, may be empty */
int player_buffer_pct(void);

/* Numbers about the current network stream (docs/firmware_v0.2.0.md, 4.3). */
typedef struct {
    uint16_t kbps;      /* download rate, 5 s average */
    uint16_t br;        /* stream bitrate (icy-br header or decoder), 0 = unknown */
    uint16_t buf_ds;    /* audio in the buffer, 0.1 s units (estimated with br, 128 kbps if unknown) */
    uint8_t pre_s;      /* seconds collected before playing (3, 6 or 10, adapts to underruns) */
    uint8_t fill_pct;   /* while buffering: progress towards pre_s; 100 otherwise */
    uint16_t under;     /* underruns while playing this stream */
    uint16_t reconn;    /* reconnects */
    const char *fmt;    /* "mp3" | "aac" | "ts" | "hls" | "" */
    char err[16];       /* last error: "connect", "http_404", "read", "stall", "decode"; "" = none */
} player_net_t;
/* false when no network stream is active (stopped, or playing an in-memory sound). */
bool player_net(player_net_t *o);

#ifdef __cplusplus
}
#endif
