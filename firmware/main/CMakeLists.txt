idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES board audio settings net ui apps nvs_flash app_update esp_app_format
)

# Assets image (web UI, sounds, sprites) from firmware/storage -> "storage" partition.
# `idf.py flash` writes it too; user data lives in the separate "data" partition.
littlefs_create_partition_image(storage ../storage FLASH_IN_PROJECT)
