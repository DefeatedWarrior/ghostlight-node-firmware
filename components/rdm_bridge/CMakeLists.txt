idf_component_register(
    SRCS "rdm_bridge.c"
    INCLUDE_DIRS "include"
    REQUIRES dmx_handler artnet_receiver esp_dmx esp_timer
)
