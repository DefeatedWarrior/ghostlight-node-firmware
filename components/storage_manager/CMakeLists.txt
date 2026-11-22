idf_component_register(
    SRCS "storage_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES littlefs esp_partition
)