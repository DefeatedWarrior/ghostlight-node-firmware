idf_component_register(
    SRCS "network_manager.c" "network_auto_fallback.c" "captive_dns.c" "dns_reply.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_netif esp_eth esp_wifi esp_event esp_timer esp_system driver config_manager board lwip
)
