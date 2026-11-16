#include "cli_vin_limit.h"
#include "device_commands.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cli_cmd_vin_limit(int argc, char **argv)
{
    power_vin_limit_t limit;
    cmd_result_t result;
    if (argc == 1) {
        result = cmd_get_vin_limit(&limit);
    } else if (argc == 3 && strcmp(argv[1], "set") == 0) {
        char *end = NULL;
        errno = 0;
        /* Signed parsing rejects negatives before conversion to uint16_t.
         * strtoul would wrap e.g. -4294963296 to 4000 on the ESP32. */
        long mv = strtol(argv[2], &end, 10);
        if (errno != 0 || end == argv[2] || *end != '\0' ||
            mv < (long)POWER_VIN_LIMIT_MIN_MV || mv > (long)POWER_VIN_LIMIT_MAX_MV ||
            !power_vin_limit_valid_mv((uint32_t)mv)) {
            printf("vin_limit: expected 3700..15200 mV in 100 mV steps\r\n");
            return 1;
        }
        result = cmd_set_vin_limit((uint16_t)mv, &limit);
    } else {
        printf("Usage: vin_limit | vin_limit set <mV>  (e.g. vin_limit set 4000)\r\n");
        return 1;
    }
    printf("%s\r\n", result.message);
    return result.status == ESP_OK ? 0 : 1;
}
