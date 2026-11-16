#pragma once
#include <stdint.h>
typedef unsigned TickType_t;
typedef int BaseType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) (ms)
typedef struct {int held;} portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
void h_critical_enter(portMUX_TYPE *m);
void h_critical_exit(portMUX_TYPE *m);
#define portENTER_CRITICAL(m) h_critical_enter(m)
#define portEXIT_CRITICAL(m) h_critical_exit(m)
