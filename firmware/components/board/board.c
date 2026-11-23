#include "board.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_nv3023.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_lvgl_port.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "iot_button.h"
#include "button_gpio.h"

#include "nv3023_init_cmds.h"

static const char *TAG = "board";

static i2c_master_bus_handle_t s_i2c;
static esp_lcd_panel_io_handle_t s_lcd_io;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;
static lv_display_t *s_disp;
static lv_indev_t *s_indev;
static int s_brightness = -1;

static board_btn_cb_t s_btn_cb;
static void *s_btn_arg;
static button_handle_t s_btn[BOARD_BTN_COUNT];
static const int s_btn_gpio[BOARD_BTN_COUNT] = {OSTB_BTN_MAIN, OSTB_BTN_VOL_UP, OSTB_BTN_VOL_DOWN};
static const int s_btn_active[BOARD_BTN_COUNT] = {OSTB_BTN_MAIN_ACTIVE, 0, 0};
static const char *s_btn_names[BOARD_BTN_COUNT] = {"MAIN", "VOL+", "VOL-"};

static adc_oneshot_unit_handle_t s_adc[2];

/* ---------------- I2C ---------------- */
static esp_err_t init_i2c(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = OSTB_I2C_PORT,
        .sda_io_num = OSTB_I2C_SDA,
        .scl_io_num = OSTB_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = OSTB_I2C_INTERNAL_PULLUP,
    };
    return i2c_new_master_bus(&cfg, &s_i2c);
}

/* ---------------- Backlight ---------------- */
static esp_err_t init_backlight(void)
{
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 25000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&t), TAG, "ledc timer");
    ledc_channel_config_t c = {
        .gpio_num = OSTB_LCD_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .flags.output_invert = OSTB_LCD_BACKLIGHT_INVERT,
    };
    return ledc_channel_config(&c);
}

void board_set_brightness(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_brightness = percent;
    uint32_t duty = (1023 * percent) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

int board_get_brightness(void) { return s_brightness; }

/* ---------------- LCD + LVGL ---------------- */
static esp_err_t init_lcd(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = OSTB_LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = OSTB_LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = OSTB_LCD_WIDTH * 80 * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    esp_lcd_panel_io_spi_config_t io_cfg = NV3023_PANEL_IO_SPI_CONFIG(OSTB_LCD_CS, OSTB_LCD_DC, NULL, NULL);
    io_cfg.pclk_hz = OSTB_LCD_PCLK_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI3_HOST, &io_cfg, &s_lcd_io), TAG, "lcd io");

    nv3023_vendor_config_t vendor = {
        .init_cmds = ostb_nv3023_init_cmds,
        .init_cmds_size = sizeof(ostb_nv3023_init_cmds) / sizeof(ostb_nv3023_init_cmds[0]),
    };
    esp_lcd_panel_dev_config_t pcfg = {
        .reset_gpio_num = OSTB_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = OSTB_LCD_BITS_PER_PIXEL,
        .vendor_config = &vendor,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_nv3023(s_lcd_io, &pcfg, &s_panel), TAG, "panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
    esp_lcd_panel_invert_color(s_panel, OSTB_LCD_INVERT_COLOR);
    esp_lcd_panel_disp_on_off(s_panel, true);

    lvgl_port_cfg_t lcfg = ESP_LVGL_PORT_INIT_CONFIG();
    lcfg.task_priority = 4;
    lcfg.task_stack = 8192;
    lcfg.task_affinity = 1;
    lcfg.timer_period_ms = 5;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lcfg), TAG, "lvgl port");

    const lvgl_port_display_cfg_t dcfg = {
        .io_handle = s_lcd_io,
        .panel_handle = s_panel,
        .buffer_size = OSTB_LCD_WIDTH * 24,
        .double_buffer = true,
        .hres = OSTB_LCD_WIDTH,
        .vres = OSTB_LCD_HEIGHT,
        .rotation = {
            .swap_xy = OSTB_LCD_SWAP_XY,
            .mirror_x = OSTB_LCD_MIRROR_X,
            .mirror_y = OSTB_LCD_MIRROR_Y,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = 1,
            .swap_bytes = 1,
        },
    };
    s_disp = lvgl_port_add_disp(&dcfg);
    ESP_RETURN_ON_FALSE(s_disp, ESP_FAIL, TAG, "add disp");
    lv_display_set_offset(s_disp, OSTB_LCD_OFFSET_X, OSTB_LCD_OFFSET_Y);
    return ESP_OK;
}

/* ---------------- Touch ---------------- */
static esp_err_t init_touch(void)
{
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr = OSTB_I2C_ADDR_CST816S,
        .control_phase_bytes = 1,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 0,
        .flags.disable_control_phase = 1,
        .scl_speed_hz = OSTB_I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c, &io_cfg, &io), TAG, "touch io");
    esp_lcd_touch_config_t tcfg = {
        .x_max = OSTB_LCD_WIDTH,
        .y_max = OSTB_LCD_HEIGHT,
        .rst_gpio_num = OSTB_TOUCH_RST,
        .int_gpio_num = OSTB_TOUCH_INT,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_cst816s(io, &tcfg, &s_touch), TAG, "cst816s");
    const lvgl_port_touch_cfg_t lt = {.disp = s_disp, .handle = s_touch};
    s_indev = lvgl_port_add_touch(&lt);
    ESP_RETURN_ON_FALSE(s_indev, ESP_FAIL, TAG, "add touch");
    return ESP_OK;
}

/* ---------------- Buttons ---------------- */
static void btn_event(void *handle, void *usr)
{
    board_btn_t b = (board_btn_t)(intptr_t)usr;
    button_event_t e = iot_button_get_event((button_handle_t)handle);
    board_btn_ev_t ev;
    switch (e) {
    case BUTTON_PRESS_DOWN: ev = BOARD_BTN_EV_DOWN; break;
    case BUTTON_PRESS_UP: ev = BOARD_BTN_EV_UP; break;
    case BUTTON_SINGLE_CLICK: ev = BOARD_BTN_EV_CLICK; break;
    case BUTTON_DOUBLE_CLICK: ev = BOARD_BTN_EV_DOUBLE; break;
    case BUTTON_LONG_PRESS_START: ev = BOARD_BTN_EV_LONG; break;
    case BUTTON_LONG_PRESS_HOLD: ev = BOARD_BTN_EV_LONG_HOLD; break;
    default: return;
    }
    ESP_LOGD(TAG, "button %s (gpio %d) ev %d", s_btn_names[b], s_btn_gpio[b], ev);
    if (s_btn_cb) s_btn_cb(b, ev, s_btn_arg);
}

static esp_err_t init_buttons(void)
{
    static const button_event_t evs[] = {BUTTON_PRESS_DOWN, BUTTON_PRESS_UP, BUTTON_SINGLE_CLICK,
                                         BUTTON_DOUBLE_CLICK, BUTTON_LONG_PRESS_START, BUTTON_LONG_PRESS_HOLD};
    for (int i = 0; i < BOARD_BTN_COUNT; i++) {
        button_config_t bc = {.long_press_time = 800, .short_press_time = 50};
        button_gpio_config_t gc = {.gpio_num = s_btn_gpio[i], .active_level = s_btn_active[i]};
        ESP_RETURN_ON_ERROR(iot_button_new_gpio_device(&bc, &gc, &s_btn[i]), TAG, "button %d", i);
        for (size_t k = 0; k < sizeof(evs) / sizeof(evs[0]); k++) {
            iot_button_register_cb(s_btn[i], evs[k], NULL, btn_event, (void *)(intptr_t)i);
        }
    }
    return ESP_OK;
}

void board_set_button_cb(board_btn_cb_t cb, void *arg)
{
    s_btn_arg = arg;
    s_btn_cb = cb;
}

int board_btn_gpio(board_btn_t btn) { return s_btn_gpio[btn]; }
const char *board_btn_name(board_btn_t btn) { return s_btn_names[btn]; }

/* ---------------- Power ---------------- */
static int adc_read_gpio(int gpio, int samples)
{
    adc_unit_t unit;
    adc_channel_t ch;
    if (adc_oneshot_io_to_channel(gpio, &unit, &ch) != ESP_OK) return -1;
    if (!s_adc[unit]) {
        adc_oneshot_unit_init_cfg_t ucfg = {.unit_id = unit, .ulp_mode = ADC_ULP_MODE_DISABLE};
        if (adc_oneshot_new_unit(&ucfg, &s_adc[unit]) != ESP_OK) return -1;
    }
    adc_oneshot_chan_cfg_t ccfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
    if (adc_oneshot_config_channel(s_adc[unit], ch, &ccfg) != ESP_OK) return -1;
    int sum = 0, n = 0;
    for (int i = 0; i < samples; i++) {
        int v;
        if (adc_oneshot_read(s_adc[unit], ch, &v) == ESP_OK) {
            sum += v;
            n++;
        }
    }
    return n ? sum / n : -1;
}

int board_adc_probe(int gpio) { return adc_read_gpio(gpio, 8); }

int board_battery_raw(void) { return adc_read_gpio(OSTB_BATTERY_ADC_GPIO, 16); }

static adc_cali_handle_t s_bat_cali;

int board_battery_mv(void)
{
    int raw = adc_read_gpio(OSTB_BATTERY_ADC_GPIO, 32);
    if (raw < 0) return -1;
    if (!s_bat_cali) {
        adc_cali_curve_fitting_config_t c = {
            .unit_id = OSTB_BATTERY_ADC_UNIT == 2 ? ADC_UNIT_2 : ADC_UNIT_1,
            .chan = OSTB_BATTERY_ADC_CHANNEL,
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_12,
        };
        if (adc_cali_create_scheme_curve_fitting(&c, &s_bat_cali) != ESP_OK) return -1;
    }
    int mv = 0;
    if (adc_cali_raw_to_voltage(s_bat_cali, raw, &mv) != ESP_OK) return -1;
    return mv * OSTB_BATTERY_DIVIDER; /* pin voltage -> battery voltage */
}

int board_battery_percent(void)
{
    static const int lvl[] = {1970, 2062, 2154, 2246, 2338, 2430};
    int raw = board_battery_raw();
    if (raw < 0) return -1;
    if (raw <= lvl[0]) return 0;
    if (raw >= lvl[5]) return 100;
    for (int i = 0; i < 5; i++) {
        if (raw < lvl[i + 1]) return i * 20 + (raw - lvl[i]) * 20 / (lvl[i + 1] - lvl[i]);
    }
    return 100;
}

bool board_is_charging(void) { return gpio_get_level(OSTB_CHARGE_DETECT) == 1; }

static void init_power(void)
{
    gpio_config_t g = {
        .pin_bit_mask = 1ULL << OSTB_CHARGE_DETECT,
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&g);
}

/* ---------------- Public ---------------- */
i2c_master_bus_handle_t board_i2c(void) { return s_i2c; }
lv_display_t *board_display(void) { return s_disp; }
lv_indev_t *board_touch(void) { return s_indev; }
bool board_lvgl_lock(int timeout_ms) { return lvgl_port_lock(timeout_ms); }
void board_lvgl_unlock(void) { lvgl_port_unlock(); }

esp_err_t board_init(void)
{
    ESP_RETURN_ON_ERROR(init_i2c(), TAG, "i2c");
    ESP_RETURN_ON_ERROR(init_backlight(), TAG, "backlight");
    ESP_RETURN_ON_ERROR(init_lcd(), TAG, "lcd");
    if (init_touch() != ESP_OK) {
        ESP_LOGW(TAG, "touch init failed, continuing without touch");
    }
    ESP_RETURN_ON_ERROR(init_buttons(), TAG, "buttons");
    init_power();
    ESP_LOGI(TAG, "board ready");
    return ESP_OK;
}
