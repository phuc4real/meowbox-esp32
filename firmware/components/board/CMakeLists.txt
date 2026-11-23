idf_component_register(
    SRCS "board.c"
    INCLUDE_DIRS "include" "../../../hw"
    REQUIRES esp_driver_i2c esp_driver_spi esp_driver_gpio esp_driver_ledc esp_lcd esp_adc
             lvgl__lvgl espressif__esp_lvgl_port 78__esp_lcd_nv3023
             espressif__esp_lcd_touch_cst816s espressif__button
)
