#pragma once

/* SCH_Schematic1_2026-10-04.pdf, pages 2-4. GPIO numbers, not module pin numbers. */
#define BOARD_LCD_MOSI       1
#define BOARD_LCD_MISO       2  /* H1 pin 12 is reserved; not connected to LCD SDO. */
#define BOARD_LCD_DC         3
#define BOARD_LCD_CS         4
#define BOARD_LCD_SCLK       5
#define BOARD_LCD_BL         6
#define BOARD_TOUCH_SCL      7
#define BOARD_TOUCH_SDA      8
#define BOARD_TOUCH_INT      9
#define BOARD_DISPLAY_RST   10  /* LCD RESET and GT911 CTP_RST share RES. */
#define BOARD_RS4851_TX     17
#define BOARD_RS4851_RX     18
#define BOARD_RS4851_DE_RE  21  /* U5, CN1 pins 3/4; high = TX. */
#define BOARD_RS4852_TX     11
#define BOARD_RS4852_RX     12
#define BOARD_RS4852_DE_RE  14  /* U7, CN1 pins 1/2; high = TX. */
#define BOARD_KEY1          41  /* Reserved input, 200 ms stable filter. */
#define BOARD_KEY2          40  /* Hold 2 s to toggle touch input. */
#define BOARD_SERIAL_TX     43
#define BOARD_SERIAL_RX     44
#define BOARD_LCD_NATIVE_WIDTH   320
#define BOARD_LCD_NATIVE_HEIGHT  480
#define BOARD_LCD_WIDTH          480
#define BOARD_LCD_HEIGHT         320
#define BOARD_TOUCH_ADDR  0x5D  /* Seven-bit address (datasheet 0xBA/0xBB). */
