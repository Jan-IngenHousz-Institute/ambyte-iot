#pragma once
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
int cli_cmd_sensors(int argc, char **argv);
#if CONFIG_AMBYTE_SENSOR_RECOVERY_HIL
int cli_cmd_sensor_recovery_hil(int argc, char **argv);
#endif
