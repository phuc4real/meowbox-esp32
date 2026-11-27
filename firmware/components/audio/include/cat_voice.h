/* Cat voice synthesis for the UI sounds (audio_sfx.c). Plain C with no ESP-IDF calls, so a host build renders the
 * very same samples to WAV files (tools/cat_voice/render.c) to listen to them before flashing. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Receives 16-bit mono PCM; returns false to stop the sound early. */
typedef bool (*cv_out_t)(const int16_t *pcm, int n, void *ctx);

/* One voiced call (meow, mew, trill): a band-limited glottal source shaped by formants that glide between three
 * vowel targets, with a pitch curve, an envelope, vibrato, breath noise and an optional "rrr" flutter. */
typedef struct {
    uint16_t ms;                 /* length */
    float f0[3];                 /* pitch at the start, at the peak and at the end (Hz) */
    float peak;                  /* where the pitch peaks, 0..1 */
    float fmt[3][2];             /* formants F1, F2 at the start, the middle and the end (Hz): the vowel */
    uint16_t attack_ms, release_ms;
    float trill_hz, trill_ms;    /* "rrr": amplitude flutter rate, only during the first trill_ms; 0 = none */
    float vib;                   /* vibrato depth, fraction of the pitch */
    float breath;                /* share of noise in the source, 0..1 */
    float gain;                  /* output level, about 0..1 */
} cv_call_t;

/* pitch and stretch scale the call (1 = as written); seed varies the jitter and noise. False if out() stopped it. */
bool cv_call(const cv_call_t *c, int rate, float pitch, float stretch, uint32_t seed, cv_out_t out, void *ctx);
/* Purr: band noise pulsed at about 25 Hz, breaths out and in */
bool cv_purr(int ms, int rate, float gain, uint32_t seed, cv_out_t out, void *ctx);
bool cv_silence(int ms, int rate, cv_out_t out, void *ctx);

/* The speaker's sounds (audio_sfx.c maps sfx_t onto these) */
typedef enum {
    CV_MIP,     /* volume up: a tiny high "mip" */
    CV_MUP,     /* volume down: a tiny low "mup" */
    CV_BOOT,    /* "mrrrp-mew" */
    CV_CHIRP,   /* notification: a friendly questioning trill, "mrrp?" */
    CV_DEMAND,  /* urgent notification: two insistent "mrrAOW!" */
    CV_ALARM,   /* one ~2 s cycle of the alarm: "meow... MEEOOW!" */
    CV_MEOW,    /* a meow, a little different every time (seed) */
    CV_PURR,
    CV_COUNT
} cv_sound_t;

bool cv_play(cv_sound_t s, int rate, uint32_t seed, cv_out_t out, void *ctx);
const char *cv_name(cv_sound_t s);

#ifdef __cplusplus
}
#endif
