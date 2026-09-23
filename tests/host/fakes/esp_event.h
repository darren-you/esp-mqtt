#pragma once
#include <stdint.h>
#define ESP_EVENT_DECLARE_BASE(name) extern const char *name
typedef const char *esp_event_base_t;
typedef void (*esp_event_handler_t)(void *, esp_event_base_t, int32_t, void *);
