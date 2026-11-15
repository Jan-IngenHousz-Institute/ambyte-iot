/* Host stand-in: single-threaded test, so critical sections are no-ops. */
#ifndef PP_HOST_FREERTOS_H
#define PP_HOST_FREERTOS_H
#include <stdint.h>
typedef int BaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY 0xffffffffu
typedef struct { int depth; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
#define taskENTER_CRITICAL(m) ((void)(m)->depth++)
#define taskEXIT_CRITICAL(m) ((void)(m)->depth--)
#endif
