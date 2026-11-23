idf_component_register(
    SRCS "wifi_mgr.c" "web.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event esp_http_server esp_timer app_update esp_app_format nvs_flash
             mbedtls lwip espressif__cjson espressif__mdns settings
)
