idf_component_register(
    SRCS "diag.c"
    INCLUDE_DIRS "include"
    REQUIRES log esp_timer espressif__cjson
)
