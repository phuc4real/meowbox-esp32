#include "audio_sfx.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_random.h"
#include "audio_hw.h"
#include "cat_voice.h"

static QueueHandle_t s_q;
static volatile bool s_stop;

/* Every sound is a cat (cat_voice.c): written in blocks to the mixer, stopping as soon as audio_sfx_stop() asks */
static bool fx_out(const int16_t *pcm, int n, void *ctx)
{
    if (s_stop) return false;
    audio_hw_write_fx(pcm, n);
    return !s_stop;
}

static void play(sfx_t s)
{
    static const cv_sound_t map[] = {[SFX_BEEP_HI] = CV_MIP, [SFX_BEEP_LO] = CV_MUP, [SFX_BOOT] = CV_BOOT,
                                     [SFX_CHIME] = CV_CHIRP, [SFX_ALERT] = CV_DEMAND, [SFX_ALARM] = CV_ALARM,
                                     [SFX_MEOW] = CV_MEOW, [SFX_PURR] = CV_PURR};
    if ((unsigned)s < sizeof(map) / sizeof(map[0])) cv_play(map[s], audio_hw_sample_rate(), esp_random(), fx_out, NULL);
}

static void sfx_task(void *arg)
{
    sfx_t s;
    while (xQueueReceive(s_q, &s, portMAX_DELAY)) {
        s_stop = false;
        audio_hw_duck(true);
        play(s);
        if (uxQueueMessagesWaiting(s_q) == 0) audio_hw_duck(false);
    }
}

void audio_sfx_init(void)
{
    if (s_q) return;
    s_q = xQueueCreate(8, sizeof(sfx_t));
    xTaskCreatePinnedToCore(sfx_task, "sfx", 4096, NULL, 6, NULL, 0);
}

void audio_sfx_play(sfx_t s)
{
    if (s_q) xQueueSend(s_q, &s, 0);
}

void audio_sfx_stop(void)
{
    if (!s_q) return;
    xQueueReset(s_q);
    s_stop = true;
}

