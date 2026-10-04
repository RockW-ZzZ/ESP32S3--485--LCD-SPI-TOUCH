#pragma once
#include <stdbool.h>
#include "esp_err.h"
esp_err_t buzzer_init(void);
esp_err_t buzzer_set(bool on);
bool buzzer_is_on(void);
