#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Call during single-threaded startup. Idempotent shared reset + GT911 address
 * selection. LCD/touch init both call this; neither resets the other later. */
esp_err_t board_display_prepare(void);
#ifdef __cplusplus
}
#endif
