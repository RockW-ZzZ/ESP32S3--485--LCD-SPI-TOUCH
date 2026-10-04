idf_component_register(SRCS "charger_codec.c" "charger_policy.c" "charger_service.c"
    "charger_web.c" "charger_network.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES board_drivers esp_timer nvs_flash esp_wifi esp_netif esp_event
        esp_http_server app_update lwip mdns
    EMBED_FILES "web/index.html")
