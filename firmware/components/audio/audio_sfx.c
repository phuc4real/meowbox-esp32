#include "audio_sfx.h"

#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_random.h"
#include "audio_hw.h"

static QueueHandle_t s_q;
static volatile bool s_stop, s_busy;

typedef struct { float f0, f1; int ms; float amp; uint8_t wave; } seg_t; /* wave 0 sine, 1 triangle, 2 noise */

static void play_seg(const seg_t *s)
{
    int rate = audio_hw_sample_rate();
    int total = rate * s->ms / 1000, fade = rate / 250;
    int16_t buf[256];
    float ph = 0;
    for (int done = 0; done < total && !s_stop; done += 256) {
        int n = total - done < 256 ? total - done : 256;
        for (int i = 0; i < n; i++) {
            int p = done + i;
            float f = s->f0 + (s->f1 - s->f0) * p / total;
            ph += 2.0f * (float)M_PI * f / rate;
            if (ph > 2.0f * (float)M_PI) ph -= 2.0f * (float)M_PI;
            float v;
            if (s->wave == 1) v = 2.0f * fabsf(ph / (float)M_PI - 1.0f) - 1.0f;
            else if (s->wave == 2) v = ((int)(esp_random() & 0xFFFF) - 32768) / 32768.0f * (0.5f + 0.5f * sinf(ph));
            else v = sinf(ph);
            float env = 1;
            if (p < fade) env = (float)p / fade;
            else if (total - p < fade) env = (float)(total - p) / fade;
            buf[i] = (int16_t)(v * env * s->amp * 32767.0f);
        }
        audio_hw_write_fx(buf, n);
    }
}

static void silence(int ms)
{
    seg_t s = {0, 0, ms, 0, 0};
    play_seg(&s);
}

#define SEQ(...) do { static const seg_t q[] = {__VA_ARGS__}; for (size_t i = 0; i < sizeof(q) / sizeof(q[0]) && !s_stop; i++) { if (q[i].amp == 0) silence(q[i].ms); else play_seg(&q[i]); } } while (0)

static void play(sfx_t s)
{
    switch (s) {
    case SFX_BEEP_HI: SEQ({1320, 1320, 50, 0.35f, 0}); break;
    case SFX_BEEP_LO: SEQ({660, 660, 50, 0.35f, 0}); break;
    case SFX_BOOT: SEQ({523, 523, 110, 0.4f, 1}, {659, 659, 110, 0.4f, 1}, {784, 784, 110, 0.4f, 1}, {1047, 1047, 220, 0.4f, 1}); break;
    case SFX_CHIME: SEQ({1175, 1175, 140, 0.45f, 0}, {0, 0, 30, 0, 0}, {1568, 1568, 260, 0.4f, 0}); break;
    case SFX_ALERT: SEQ({1760, 1760, 90, 0.5f, 1}, {0, 0, 60, 0, 0}, {1760, 1760, 90, 0.5f, 1}, {0, 0, 60, 0, 0}, {1760, 1760, 90, 0.5f, 1}); break;
    case SFX_ALARM:
        SEQ({988, 988, 120, 0.55f, 1}, {0, 0, 80, 0, 0}, {988, 988, 120, 0.55f, 1}, {0, 0, 80, 0, 0},
            {988, 988, 120, 0.55f, 1}, {0, 0, 80, 0, 0}, {988, 988, 120, 0.55f, 1}, {0, 0, 1000, 0, 0});
        break;
    case SFX_MEOW: SEQ({520, 900, 120, 0.4f, 1}, {900, 600, 260, 0.4f, 1}); break;
    case SFX_PURR: SEQ({28, 26, 900, 0.5f, 2}); break;
    }
}

static void sfx_task(void *arg)
{
    sfx_t s;
    while (xQueueReceive(s_q, &s, portMAX_DELAY)) {
        s_stop = false;
        s_busy = true;
        audio_hw_duck(true);
        play(s);
        if (uxQueueMessagesWaiting(s_q) == 0) {
            audio_hw_duck(false);
            s_busy = false;
        }
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

bool audio_sfx_busy(void) { return s_busy; }
