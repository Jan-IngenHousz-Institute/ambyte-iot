#pragma once
#include <assert.h>
#include "esp_err.h"
#define ESP_RETURN_ON_FALSE(test,error,tag,...) do { (void)(tag); if (!(test)) return (error); } while(0)
