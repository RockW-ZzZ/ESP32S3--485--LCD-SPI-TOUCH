idf_component_register(SRCS "board_lvgl.c" INCLUDE_DIRS "include"
                       REQUIRES board_drivers lvgl esp_timer freertos)
