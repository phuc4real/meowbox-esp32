/*
 * Low-level audio for the OSTB board: ES8311 DAC (mono speaker) + ES7210 4-mic ADC
 * sharing one full-duplex I2S port (std TX, TDM RX).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_MIC_CHANNELS 4

esp_err_t audio_hw_init(i2c_master_bus_handle_t bus, int sample_rate);
int audio_hw_sample_rate(void);

/* Output: 16-bit mono PCM at audio_hw_sample_rate(). Blocks until queued. */
esp_err_t audio_hw_write(const int16_t *pcm, size_t samples);
void audio_hw_set_volume(int volume); /* 0..100 */
int audio_hw_get_volume(void);
void audio_hw_set_mute(bool mute);

/* Blocking sine tone, for beeps and hardware tests. */
void audio_hw_tone(int freq_hz, int ms, int amplitude_pct);

/* Input: 16-bit, AUDIO_MIC_CHANNELS interleaved frames. */
esp_err_t audio_hw_mic_enable(bool enable);
esp_err_t audio_hw_mic_read(int16_t *frames, size_t frame_count);

#ifdef __cplusplus
}
#endif
