idf_component_register(
    SRCS "board_display.c" "lcd_st7796.c" "touch_gt911.c" "buttons.c"
         "serial_port.c" "rs485.c" "modbus_rtu.c" "modbus_codec.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_driver_spi esp_driver_i2c esp_driver_uart
             esp_driver_ledc esp_timer freertos)
