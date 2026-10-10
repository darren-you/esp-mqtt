#pragma once
#include <stddef.h>
#include <stdint.h>
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(milliseconds) ((TickType_t)(milliseconds))
typedef struct { unsigned unused; } portMUX_TYPE;
#define portMUX_INITIALIZE(mux) (*(mux) = (portMUX_TYPE){0})
void emqtt_test_enter_critical(portMUX_TYPE *mux);
void emqtt_test_exit_critical(portMUX_TYPE *mux);
#define portENTER_CRITICAL(mux) emqtt_test_enter_critical(mux)
#define portEXIT_CRITICAL(mux) emqtt_test_exit_critical(mux)
