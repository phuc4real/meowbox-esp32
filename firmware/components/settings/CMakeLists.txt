idf_component_register(
    SRCS "settings.c"
    INCLUDE_DIRS "include"
    REQUIRES espressif__cjson joltwallet__littlefs vfs
)
