# Local LVGL 8.4.0

LVGL **8.4.0** is fully bundled in this directory. It was originally imported from
the old reference project; that project is no longer needed and may be deleted.
The imported manifest identifies upstream commit
`4495f428630cc1741bd8bfd977f080e8460e8e8d`. No upgrade was performed.

Unmodified files: `src/`, `demos/benchmark/`, `demos/lv_demos.h`, `lvgl.h`,
`Kconfig`, and `LICENCE.txt`. The MIT license is retained. `CMakeLists.txt` is the
local ESP-IDF component integration. Examples and other demos are deliberately
not bundled; enabling them produces a configuration error with this explanation.
All 398 original files are listed in [SOURCE_SHA256.json](SOURCE_SHA256.json).
Verify them with `python tests/check_dependencies.py` from the firmware root;
verification does not need the original reference project.

Configuration uses the original Kconfig via `CONFIG_LV_CONF_SKIP=y`; there is no
second `lv_conf.h`. The project's `sdkconfig.defaults` enables RGB565, benchmark,
compressed fonts, malloc/free with ESP-IDF PSRAM support, and USB console logs.
`LV_COLOR_16_SWAP` is disabled because `lcd_draw_rgb565()` performs the byte swap.

The separate `board_lvgl` component adapts LVGL 8's display and pointer callbacks
to the current ST7796 / GT911 drivers. It does not copy the reference board's
FT6336 / TCA9554 / AXP2101 dependencies. A 5 ms tick and a single GUI task drive
LVGL; synchronous SPI completion precedes `lv_disp_flush_ready()`.
Touch input is gated by the board driver. GPIO40's two-second hold toggles input;
the GUI callback resets a prior gesture on a mode change without synthesizing a
click. GPIO41 is a reserved, filtered input and is not bound to LVGL.

See the [dependency inventory](../../DEPENDENCIES.md) for local tools and the
standalone build procedure.
