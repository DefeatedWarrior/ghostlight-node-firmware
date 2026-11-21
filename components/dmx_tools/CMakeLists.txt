idf_component_register(
    SRCS "dmx_tools.c"
    INCLUDE_DIRS "include"
    REQUIRES dmx_handler config_manager esp_timer
)
