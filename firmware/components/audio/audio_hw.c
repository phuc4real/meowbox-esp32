#include "audio_hw.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "ostb_xiaozhi_3st.h"

static const char *TAG = "audio_hw";

static i2s_chan_handle_t s_tx, s_rx;
static esp_codec_dev_handle_t s_out, s_in;
static SemaphoreHandle_t s_lock;
static int s_rate;
static int s_volume = 60;
static bool s_mic_on;

/* Speaker amplifier (PA) power save, used while the screen sleeps: off after AMP_IDLE_US without output, back on
 * (with a short settle) by the next audio_hw_write(). esp_codec_dev turned it on when the output was opened. */
#define AMP_IDLE_US   (3 * 1000000LL)
#define AMP_SETTLE_MS 10
static volatile bool s_amp_on = true;
static volatile int64_t s_last_write;

static esp_err_t create_i2s(int rate)
{
    i2s_chan_config_t chan = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 6,
        .dma_frame_num = 240,
        .auto_clear_after_cb = true,
    };
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan, &s_tx, &s_rx), TAG, "i2s channel");

    i2s_std_config_t std = {
        .clk_cfg = {.sample_rate_hz = rate, .clk_src = I2S_CLK_SRC_DEFAULT, .mclk_multiple = I2S_MCLK_MULTIPLE_256},
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .bit_shift = true,
            .left_align = true,
        },
        .gpio_cfg = {
            .mclk = OSTB_I2S_MCLK, .bclk = OSTB_I2S_BCLK, .ws = OSTB_I2S_WS,
            .dout = OSTB_I2S_DOUT, .din = I2S_GPIO_UNUSED,
        },
    };
    i2s_tdm_config_t tdm = {
        .clk_cfg = {.sample_rate_hz = rate, .clk_src = I2S_CLK_SRC_DEFAULT,
                    .mclk_multiple = I2S_MCLK_MULTIPLE_256, .bclk_div = 8},
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3,
            .ws_width = I2S_TDM_AUTO_WS_WIDTH,
            .bit_shift = true,
            .total_slot = I2S_TDM_AUTO_SLOT_NUM,
        },
        .gpio_cfg = {
            .mclk = OSTB_I2S_MCLK, .bclk = OSTB_I2S_BCLK, .ws = OSTB_I2S_WS,
            .dout = I2S_GPIO_UNUSED, .din = OSTB_I2S_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std), TAG, "tx std");
    ESP_RETURN_ON_ERROR(i2s_channel_init_tdm_mode(s_rx, &tdm), TAG, "rx tdm");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "tx en");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "rx en");
    return ESP_OK;
}

esp_err_t audio_hw_init(i2c_master_bus_handle_t bus, int sample_rate)
{
    s_rate = sample_rate;
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_ERROR(create_i2s(sample_rate), TAG, "i2s");

    audio_codec_i2s_cfg_t i2s_cfg = {.port = I2S_NUM_0, .rx_handle = s_rx, .tx_handle = s_tx};
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    audio_codec_i2c_cfg_t i2c_cfg = {.port = OSTB_I2C_PORT, .addr = OSTB_I2C_ADDR_ES8311 << 1, .bus_handle = bus};
    const audio_codec_ctrl_if_t *out_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    es8311_codec_cfg_t es8311 = {
        .ctrl_if = out_ctrl,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = OSTB_PA_ENABLE,
        .use_mclk = true,
        .hw_gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3},
    };
    const audio_codec_if_t *out_if = es8311_codec_new(&es8311);
    ESP_RETURN_ON_FALSE(out_if, ESP_FAIL, TAG, "es8311 not found");
    esp_codec_dev_cfg_t dev = {.dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = out_if, .data_if = data_if};
    s_out = esp_codec_dev_new(&dev);

    i2c_cfg.addr = OSTB_I2C_ADDR_ES7210 << 1;
    const audio_codec_ctrl_if_t *in_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    es7210_codec_cfg_t es7210 = {
        .ctrl_if = in_ctrl,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4,
    };
    const audio_codec_if_t *in_if = es7210_codec_new(&es7210);
    ESP_RETURN_ON_FALSE(in_if, ESP_FAIL, TAG, "es7210 not found");
    dev.dev_type = ESP_CODEC_DEV_TYPE_IN;
    dev.codec_if = in_if;
    s_in = esp_codec_dev_new(&dev);

    esp_codec_dev_sample_info_t fs = {.bits_per_sample = 16, .channel = 1, .sample_rate = sample_rate};
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_out, &fs) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "open out");
    esp_codec_dev_set_out_vol(s_out, s_volume);
    ESP_LOGI(TAG, "ES8311 + ES7210 ready, %d Hz", sample_rate);
    return ESP_OK;
}

int audio_hw_sample_rate(void) { return s_rate; }

esp_err_t audio_hw_write(const int16_t *pcm, size_t samples)
{
    if (!s_out) return ESP_ERR_INVALID_STATE;
    s_last_write = esp_timer_get_time();
    if (!s_amp_on) {
        gpio_set_level(OSTB_PA_ENABLE, 1);
        s_amp_on = true;
        vTaskDelay(pdMS_TO_TICKS(AMP_SETTLE_MS));
    }
    return esp_codec_dev_write(s_out, (void *)pcm, samples * sizeof(int16_t)) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

void audio_hw_set_volume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    s_volume = volume;
    if (s_out) esp_codec_dev_set_out_vol(s_out, volume);
}

int audio_hw_get_volume(void) { return s_volume; }

void audio_hw_power_save(bool on)
{
    if (!s_out) return;
    if (on && s_amp_on && esp_timer_get_time() - s_last_write > AMP_IDLE_US) {
        gpio_set_level(OSTB_PA_ENABLE, 0);
        s_amp_on = false;
        ESP_LOGI(TAG, "amplifier off (idle)");
    } else if (!on && !s_amp_on) {
        gpio_set_level(OSTB_PA_ENABLE, 1);
        s_amp_on = true;
    }
}

void audio_hw_set_mute(bool mute)
{
    if (s_out) esp_codec_dev_set_out_mute(s_out, mute);
}

void audio_hw_tone(int freq_hz, int ms, int amplitude_pct)
{
    const int chunk = 240;
    int16_t buf[240];
    int total = s_rate * ms / 1000;
    float amp = 32767.0f * amplitude_pct / 100.0f;
    float phase = 0, step = 2.0f * (float)M_PI * freq_hz / s_rate;
    int fade = s_rate / 200; /* 5 ms fade in/out avoids clicks */
    for (int done = 0; done < total; done += chunk) {
        int n = total - done < chunk ? total - done : chunk;
        for (int i = 0; i < n; i++) {
            int pos = done + i;
            float env = 1.0f;
            if (pos < fade) env = (float)pos / fade;
            else if (total - pos < fade) env = (float)(total - pos) / fade;
            buf[i] = (int16_t)(sinf(phase) * amp * env);
            phase += step;
            if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        }
        audio_hw_write(buf, n);
    }
}

esp_err_t audio_hw_mic_enable(bool enable)
{
    if (!s_in) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t r = ESP_OK;
    if (enable && !s_mic_on) {
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = AUDIO_MIC_CHANNELS,
            .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1) |
                            ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(3),
            .sample_rate = s_rate,
        };
        r = esp_codec_dev_open(s_in, &fs) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
        if (r == ESP_OK) {
            esp_codec_dev_set_in_gain(s_in, 30.0);
            s_mic_on = true;
        }
    } else if (!enable && s_mic_on) {
        esp_codec_dev_close(s_in);
        s_mic_on = false;
    }
    xSemaphoreGive(s_lock);
    return r;
}

esp_err_t audio_hw_mic_read(int16_t *frames, size_t frame_count)
{
    if (!s_mic_on) return ESP_ERR_INVALID_STATE;
    int bytes = frame_count * AUDIO_MIC_CHANNELS * sizeof(int16_t);
    return esp_codec_dev_read(s_in, frames, bytes) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}
