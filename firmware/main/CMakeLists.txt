idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES board audio settings net ui apps player nvs_flash app_update esp_app_format esp_pm
)

