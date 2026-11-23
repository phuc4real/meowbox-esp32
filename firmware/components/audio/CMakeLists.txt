idf_component_register(
    SRCS "audio_hw.c" "audio_sfx.c"
    INCLUDE_DIRS "include" "../../../hw"
    REQUIRES esp_driver_i2s esp_driver_i2c espressif__esp_codec_dev
)
