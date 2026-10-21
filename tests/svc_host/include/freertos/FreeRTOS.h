#pragma once
#include <stdint.h>
typedef int BaseType_t;
typedef uint32_t TickType_t;
typedef unsigned UBaseType_t;
typedef uint8_t StackType_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define pdFALSE 0
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#ifndef portTICK_PERIOD_MS
#define portTICK_PERIOD_MS 1
#endif
#define pdMS_TO_TICKS(n) ((TickType_t)((n) / portTICK_PERIOD_MS))
void svc_test_enter(void);
void svc_test_exit(void);
#define portENTER_CRITICAL(m) ((void)(m),svc_test_enter())
#define portEXIT_CRITICAL(m) ((void)(m),svc_test_exit())
#define portENTER_CRITICAL_ISR(m) portENTER_CRITICAL(m)
#define portEXIT_CRITICAL_ISR(m) portEXIT_CRITICAL(m)
