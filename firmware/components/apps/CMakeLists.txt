idf_component_register(
    SRCS "clock_app.c" "radio_app.c" "notify_app.c" "pc_app.c" "cat_app.c" "battery_app.c" "system_app.c"
    INCLUDE_DIRS "include"
    REQUIRES cat_sprites player ui board net settings lunar audio fonts esp_http_client mbedtls espressif__cjson esp_app_format lwip esp_timer
)
