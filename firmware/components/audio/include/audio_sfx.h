/* Synthesised UI sounds, played on their own task (non-blocking, thread-safe). */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SFX_BEEP_HI,
    SFX_BEEP_LO,
    SFX_BOOT,
    SFX_CHIME,  /* notification */
    SFX_ALERT,  /* urgent notification */
    SFX_ALARM,  /* one ~2 s cycle of the alarm pattern; repeat for ringing */
    SFX_MEOW,
    SFX_PURR,
} sfx_t;

void audio_sfx_init(void);
void audio_sfx_play(sfx_t s);
void audio_sfx_stop(void);       /* drops queued sounds and cuts the current one short */
bool audio_sfx_busy(void);
/* Sounds are mixed over playing music, which is lowered (ducked) meanwhile (audio_hw_write_fx). */

#ifdef __cplusplus
}
#endif
