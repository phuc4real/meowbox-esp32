/*
 * MeowBox — clock, internet radio, notifications, PC screen and a cat that codes along with Claude Code
 * for the OSTB xiaozhi-3st speaker. See docs/plan.md.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_pm.h"
#include "nvs_flash.h"
#include "board.h"
#include "audio_hw.h"
#include "audio_sfx.h"
#include "settings.h"
#include "net.h"
#include "ui.h"
#include "apps.h"
#include "diag.h"
#include "player.h"

static const char *TAG = "meowbox";

#define SAMPLE_RATE 44100

/* Holding VOL+ and VOL- together for 3 s opens the Wi-Fi setup hotspot (the middle button is a hardware power switch
 * the firmware can't read). Volume is frozen while both are down. */
#define COMBO_HOLD_US (3 * 1000000LL)
static bool s_vol_down[2];
static bool s_combo;          /* both were down at some point; stays set until both are released */
static int64_t s_combo_since;
static bool s_combo_fired;

static bool vol_combo(board_btn_t b, board_btn_ev_t ev)
{
    int i = b == BOARD_BTN_VOL_UP ? 0 : 1;
    if (ev == BOARD_BTN_EV_DOWN || ev == BOARD_BTN_EV_UP) {
        s_vol_down[i] = ev == BOARD_BTN_EV_DOWN;
        bool both = s_vol_down[0] && s_vol_down[1];
        if (both && !s_combo) { s_combo = true; s_combo_since = esp_timer_get_time(); s_combo_fired = false; }
        bool swallow = s_combo;
        if (!s_vol_down[0] && !s_vol_down[1]) s_combo = false;
        return swallow;
    }
    if (!s_combo) return false;
    if (s_vol_down[0] && s_vol_down[1] && !s_combo_fired && esp_timer_get_time() - s_combo_since >= COMBO_HOLD_US) {
        s_combo_fired = true;
        ESP_LOGI(TAG, "VOL+ and VOL- held: opening the Wi-Fi setup hotspot");
        system_wifi_setup(false);
    }
    return true;
}

/* A press that wakes the sleeping screen only wakes it: its click/hold does nothing else. */
static bool s_wake_press[BOARD_BTN_COUNT];
/* A press that stopped or snoozed the alarm: its later hold events don't change the volume. */
static bool s_alarm_press[BOARD_BTN_COUNT];

static void on_button(board_btn_t b, board_btn_ev_t ev, void *arg)
{
    if (ev == BOARD_BTN_EV_DOWN) {
        s_alarm_press[b] = false;
        s_wake_press[b] = ui_asleep() && !clock_alarm_ringing();
        ui_wake();
    }
    if (vol_combo(b, ev)) return;
    if (ev == BOARD_BTN_EV_DOWN || ev == BOARD_BTN_EV_UP) return;
    if (s_wake_press[b]) return;

    /* alarm ringing: click = snooze, hold = stop (the middle button is only a power switch, the ESP32 can't see it) */
    if (clock_alarm_ringing() && (ev == BOARD_BTN_EV_CLICK || ev == BOARD_BTN_EV_LONG)) {
        clock_alarm_stop(ev == BOARD_BTN_EV_CLICK);
        s_alarm_press[b] = true;
        return;
    }
    if (s_alarm_press[b]) return; /* the rest of that hold: no volume change */
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
    update_status_hook(r);
    cJSON_ReplaceItemInObject(r, "pc", cJSON_CreateBool(pc_online()));
}

/* ---- power: CPU at 240 MHz while the screen is on or audio plays, 80 MHz while the screen sleeps (DFS) ---- */
static esp_pm_lock_handle_t s_cpu_lock;
static bool s_low_power;

static void init_pm(void)
{
    esp_pm_config_t pm = {.max_freq_mhz = 240, .min_freq_mhz = 80, .light_sleep_enable = false};
    esp_err_t r = esp_pm_configure(&pm);
    if (r == ESP_OK) r = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "awake", &s_cpu_lock);
    if (r == ESP_OK) r = esp_pm_lock_acquire(s_cpu_lock);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "power management off (%s)", esp_err_to_name(r));
        s_cpu_lock = NULL;
    }
}

static void on_sleep_tick(bool asleep)
{
    diag_ui_alive(); /* runs on every UI tick: the frozen-screen check counts these */
    player_state_t ps = player_state();
    bool low = asleep && ps != PLAYER_PLAY && ps != PLAYER_BUF;
    audio_hw_power_save(low);
    if (low == s_low_power) return;
    s_low_power = low;
    if (s_cpu_lock) {
        if (low) esp_pm_lock_release(s_cpu_lock);
        else esp_pm_lock_acquire(s_cpu_lock);
    }
    ESP_LOGI(TAG, "low power %s", low ? "on" : "off");
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
    diag_init(); /* first: reset reason, the previous boot's error log, then capture this boot's */
    ESP_LOGI(TAG, "MeowBox %s starting", esp_app_get_description()->version);
    init_nvs();
    init_pm();
    ESP_ERROR_CHECK(settings_init());
    diag_boot_record();
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
    update_app_init();
    ui_build_all();
    ui_set_sleep_hook(on_sleep_tick);
    board_lvgl_unlock();
    board_set_button_cb(on_button, NULL);

    web_ready();
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "up");
}
