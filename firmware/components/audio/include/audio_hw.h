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

/* Output: 16-bit mono PCM at audio_hw_sample_rate(). Blocks until queued.
 * audio_hw_write is the main output (the player, one task at a time); audio_hw_write_fx is for short UI sounds: while
 * the main output runs they are mixed into it, otherwise they play directly. */
esp_err_t audio_hw_write(const int16_t *pcm, size_t samples);
esp_err_t audio_hw_write_fx(const int16_t *pcm, size_t samples);
/* Lower the main output (about -10 dB, 50 ms ramps) while UI sounds play. */
void audio_hw_duck(bool on);
/* esp_timer time of the last sample sent to the codec (to ignore our own sound on the mics). */
int64_t audio_hw_last_output_us(void);
void audio_hw_set_volume(int volume); /* 0..100 */
int audio_hw_get_volume(void);
void audio_hw_set_mute(bool mute);
/* Call periodically. on: switch the speaker amplifier off once nothing has played for 3 s (the next write turns it
 * back on); off: amplifier on now. */
void audio_hw_power_save(bool on);

/* Blocking sine tone, for beeps and hardware tests. */
void audio_hw_tone(int freq_hz, int ms, int amplitude_pct);

/* Input: 16-bit, AUDIO_MIC_CHANNELS interleaved frames. */
esp_err_t audio_hw_mic_enable(bool enable);
esp_err_t audio_hw_mic_read(int16_t *frames, size_t frame_count);

#ifdef __cplusplus
}
#endif
