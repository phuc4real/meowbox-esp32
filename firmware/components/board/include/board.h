/*
 * MeowBox board layer for the OSTB xiaozhi-3st speaker.
 * Pins come from hw/ostb_xiaozhi_3st.h (repo root).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "lvgl.h"
#include "ostb_xiaozhi_3st.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BOARD_BTN_MAIN = 0,   /* middle (power) button, GPIO1 */
    BOARD_BTN_VOL_UP,
    BOARD_BTN_VOL_DOWN,
    BOARD_BTN_COUNT,
} board_btn_t;

typedef enum {
    BOARD_BTN_EV_DOWN = 0,
    BOARD_BTN_EV_UP,
    BOARD_BTN_EV_CLICK,
    BOARD_BTN_EV_DOUBLE,
    BOARD_BTN_EV_LONG,        /* fired once after long_press_ms */
    BOARD_BTN_EV_LONG_HOLD,   /* repeats while still held */
} board_btn_ev_t;

typedef void (*board_btn_cb_t)(board_btn_t btn, board_btn_ev_t ev, void *arg);

/* Brings up I2C, SPI LCD + LVGL, touch, backlight, buttons and the power ADC. */
esp_err_t board_init(void);

i2c_master_bus_handle_t board_i2c(void);
lv_display_t *board_display(void);
lv_indev_t *board_touch(void);
int board_btn_gpio(board_btn_t btn);
const char *board_btn_name(board_btn_t btn);

/* LVGL is not thread safe: wrap every LVGL call made outside LVGL callbacks. */
bool board_lvgl_lock(int timeout_ms);
void board_lvgl_unlock(void);

void board_set_brightness(int percent); /* 0..100 */
int board_get_brightness(void);
/* Panel display on/off (screen sleep). Call with LVGL drawing paused (or from the LVGL task). */
void board_lcd_on(bool on);

void board_set_button_cb(board_btn_cb_t cb, void *arg);

/* Power */
bool board_is_charging(void);
int board_battery_raw(void);      /* averaged ADC raw, -1 on error */
int board_battery_percent(void);  /* 0..100, -1 on error */
int board_battery_mv(void);       /* calibrated battery voltage in mV (assumes OSTB_BATTERY_DIVIDER), -1 on error */
/* Diagnostics: one-shot raw read of any ADC-capable GPIO (1..20), -1 if not possible. */
int board_adc_probe(int gpio);

#ifdef __cplusplus
}
#endif
