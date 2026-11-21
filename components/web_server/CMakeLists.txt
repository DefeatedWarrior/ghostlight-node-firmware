idf_component_register(
    SRCS "web_server.c" "web_tools.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server json config_manager network_manager dmx_handler artnet_receiver sacn_receiver merge_engine app_update scene_manager esp_wifi mbedtls esp_timer lwip dmx_tools diag board
)
