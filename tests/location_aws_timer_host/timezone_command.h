#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "device_location.h"
bool timezone_command_offer_live(double, double, double, int64_t, device_live_source_t);
