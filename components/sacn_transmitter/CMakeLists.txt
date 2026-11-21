idf_component_register(
    SRCS "sacn_transmitter.c"
    INCLUDE_DIRS "include"
    REQUIRES lwip esp_timer esp_hw_support
)
