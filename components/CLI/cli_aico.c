#include "cli_aico.h"
#include "device_commands.h"

#include <stdio.h>
#include <string.h>

/* aico | aico on | aico off | aico reset (docs/charger-aico.md). */
int cli_cmd_aico(int argc, char **argv)
{
    power_aico_t aico;
    cmd_result_t result;
    if (argc == 1) {
        result = cmd_get_aico(&aico);
    } else if (argc == 2 && strcmp(argv[1], "on") == 0) {
        result = cmd_set_aico(true, &aico);
    } else if (argc == 2 && strcmp(argv[1], "off") == 0) {
        result = cmd_set_aico(false, &aico);
    } else if (argc == 2 && strcmp(argv[1], "reset") == 0) {
        result = cmd_reset_aico(&aico);
    } else {
        printf("Usage: aico | aico on | aico off | aico reset\r\n");
        return 1;
    }
    printf("%s\r\n", result.message);
    return result.status == ESP_OK ? 0 : 1;
}
