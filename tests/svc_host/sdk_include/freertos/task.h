#pragma once
#include "../../include/freertos/task.h"
typedef int TimeOut_t;
void vTaskSetTimeOutState(TimeOut_t *);
BaseType_t xTaskCheckForTimeOut(TimeOut_t *, TickType_t *);
