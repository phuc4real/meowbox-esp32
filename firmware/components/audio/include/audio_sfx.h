/* UI sounds, all synthesised cat voices (cat_voice.c), played on their own task (non-blocking, thread-safe). */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SFX_BEEP_HI, /* volume up: "mip" */
    SFX_BEEP_LO, /* volume down: "mup" */
    SFX_BOOT,    /* "mrrrp-mew" */
    SFX_CHIME,   /* notification: a questioning trill "mrrp?" */
    SFX_ALERT,   /* urgent notification: "mrrAOW! mrrAOW!" */
    SFX_ALARM,   /* one ~2 s cycle of the alarm, "meow... MEEOOW!"; repeat for ringing */
    SFX_MEOW,
    SFX_PURR,
} sfx_t;

void audio_sfx_init(void);
void audio_sfx_play(sfx_t s);
void audio_sfx_stop(void);       /* drops queued sounds and cuts the current one short */
/* Sounds are mixed over playing music, which is lowered (ducked) meanwhile (audio_hw_write_fx). */

#ifdef __cplusplus
}
#endif
