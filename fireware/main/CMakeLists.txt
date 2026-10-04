idf_component_register(SRCS "app_main.c" INCLUDE_DIRS "."
                       PRIV_REQUIRES board_drivers board_lvgl charger nvs_flash esp_psram esp_system)
