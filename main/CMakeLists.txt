idf_component_register(
    SRCS "main.c" "app_routing.c" "app_artnet.c" "app_config.c" "app_scenes.c" "app_indicator.c"
    INCLUDE_DIRS "."
    REQUIRES driver storage_manager config_manager led_manager network_manager dmx_handler
             artnet_receiver sacn_receiver artnet_transmitter sacn_transmitter
             merge_engine web_server scene_manager rdm_bridge dmx_tools dmx_tester diag board
             lwip esp_wifi esp_timer app_update nvs_flash mdns
)
