#pragma once
#include "mock_idf.h"
typedef void *TaskHandle_t;
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
int xTaskCreate(void (*fn)(void *), const char *, unsigned, void *, unsigned, TaskHandle_t *);
void xTaskNotifyGive(TaskHandle_t);
unsigned ulTaskNotifyTake(int, unsigned);
const char *esp_err_to_name(int);
