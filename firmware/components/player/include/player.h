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
/* Pause output while UI sounds play (sfx hook). */
void player_hold(bool hold);

#ifdef __cplusplus
}
#endif
