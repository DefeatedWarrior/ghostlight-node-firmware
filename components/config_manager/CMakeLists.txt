idf_component_register(
    SRCS "config_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES board json nvs_flash storage_manager esp_hw_support mbedtls
)
