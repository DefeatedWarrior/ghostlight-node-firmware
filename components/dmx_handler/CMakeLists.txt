idf_component_register(
    SRCS "dmx_handler.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_dmx driver config_manager board esp_system esp_timer
)
