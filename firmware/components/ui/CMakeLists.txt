idf_component_register(
    SRCS "ui.c" "themes_gen.c" "ui_assets.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl__lvgl fonts board settings esp_timer
)
