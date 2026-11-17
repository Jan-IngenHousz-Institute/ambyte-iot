#include "cli_iin_limit.h"
#include "device_commands.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cli_cmd_iin_limit(int argc, char **argv)
{
    power_iin_limit_t limit;
    cmd_result_t result;
    if (argc == 1) {
        result = cmd_get_iin_limit(&limit);
    } else if (argc == 3 && strcmp(argv[1], "set") == 0) {
        char *end = NULL;
        errno = 0;
        /* Signed parsing rejects negatives before conversion to uint16_t:
         * strtoul would wrap e.g. -4294966296 to 1000 on the ESP32. */
        long ma = strtol(argv[2], &end, 10);
        if (errno != 0 || end == argv[2] || *end != '\0' ||
            ma < (long)POWER_IIN_LIMIT_MIN_MA || ma > (long)POWER_IIN_LIMIT_MAX_MA ||
            !power_iin_limit_valid_ma((uint32_t)ma)) {
            printf("iin_limit: expected 100..3250 mA in 50 mA steps\r\n");
            return 1;
        }
        result = cmd_set_iin_limit((uint16_t)ma, &limit);
    } else {
        printf("Usage: iin_limit | iin_limit set <mA>  (e.g. iin_limit set 1000)\r\n");
        return 1;
    }
    printf("%s\r\n", result.message);
    return result.status == ESP_OK ? 0 : 1;
}
