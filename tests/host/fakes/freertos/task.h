#pragma once
#include "freertos/FreeRTOS.h"
typedef void *TaskHandle_t;
TaskHandle_t xTaskGetCurrentTaskHandle(void);
void vTaskDelay(TickType_t ticks);
