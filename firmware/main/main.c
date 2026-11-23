/*
 * MeowBox — clock, internet radio, notifications, PC screen and a virtual cat
 * for the OSTB xiaozhi-3st speaker. See docs/plan.md.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "board.h"
#include "audio_hw.h"
#include "audio_sfx.h"
#include "settings.h"
#include "net.h"
#include "ui.h"
#include "apps.h"

static const char *TAG = "meowbox";

#define SAMPLE_RATE 44100

static void on_button(board_btn_t b, board_btn_ev_t ev, void *arg)
{
    if (ev == BOARD_BTN_EV_DOWN) ui_wake();
    if (ev == BOARD_BTN_EV_DOWN || ev == BOARD_BTN_EV_UP) return;

    if (b == BOARD_BTN_MAIN) {
        /* middle button: holding it powers off in hardware, so only short/double presses */
        if (ev == BOARD_BTN_EV_CLICK) {
            if (clock_alarm_ringing()) { clock_alarm_stop(false); return; }
            if (ui_overlay_visible() && board_lvgl_lock(100)) {
                ui_overlay_hide();
                board_lvgl_unlock();
                return;
            }
            radio_toggle();
        } else if (ev == BOARD_BTN_EV_DOUBLE) {
            if (board_lvgl_lock(100)) {
                ui_show_tile(UI_TILE_CLOCK, true);
                board_lvgl_unlock();
            }
        }
        return;
    }
    if (clock_alarm_ringing()) { clock_alarm_stop(true); return; }
    int vol = audio_hw_get_volume();
    int step = (ev == BOARD_BTN_EV_CLICK || ev == BOARD_BTN_EV_LONG_HOLD) ? 5 : 0;
    if (!step) return;
    vol += b == BOARD_BTN_VOL_UP ? step : -step;
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    audio_hw_set_volume(vol);
    if (ev == BOARD_BTN_EV_CLICK) audio_sfx_play(b == BOARD_BTN_VOL_UP ? SFX_BEEP_HI : SFX_BEEP_LO);
    ESP_LOGI(TAG, "volume %d", vol);
}

static void status_hook(cJSON *r)
{
    cJSON_ReplaceItemInObject(r, "bat", cJSON_CreateNumber(board_battery_percent()));
    cJSON_ReplaceItemInObject(r, "chg", cJSON_CreateBool(board_is_charging()));
    cJSON_ReplaceItemInObject(r, "usb", cJSON_CreateBool(board_is_charging()));
    cJSON_ReplaceItemInObject(r, "vol", cJSON_CreateNumber(audio_hw_get_volume()));
    battery_status_hook(r);
    radio_status_hook(r);
    cJSON_ReplaceItemInObject(r, "pc", cJSON_CreateBool(pc_online()));
}

static void init_nvs(void)
{
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "MeowBox %s starting", esp_app_get_description()->version);
    init_nvs();
    ESP_ERROR_CHECK(settings_init());
    ESP_ERROR_CHECK(board_init());

    if (audio_hw_init(board_i2c(), SAMPLE_RATE) == ESP_OK) {
        audio_sfx_init();
        audio_sfx_play(SFX_BOOT);
    } else {
        ESP_LOGE(TAG, "audio init failed");
    }

    wifi_mgr_start();
    web_start();
    web_set_status_hook(status_hook);

    board_lvgl_lock(0);
    ui_init();
    clock_app_init();
    radio_app_init();
    notify_app_init();
    pc_app_init();
    cat_app_init();
    battery_app_init();
    system_app_init();
    ui_build_all();
    board_lvgl_unlock();
    board_set_button_cb(on_button, NULL);

    web_ready();
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "up");
}
