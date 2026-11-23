idf_component_register(
    SRCS "player.c"
    INCLUDE_DIRS "include"
    REQUIRES audio esp_http_client mbedtls esp_ringbuf espressif__esp_audio_codec esp_timer esp_event esp_netif
)
