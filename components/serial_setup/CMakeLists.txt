idf_component_register(
    SRCS "serial_setup.c"
    INCLUDE_DIRS "include"
    REQUIRES config_manager network_manager dmx_tester board json driver vfs esp_app_format esp_wifi esp_system
)
