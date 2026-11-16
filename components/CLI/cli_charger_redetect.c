#include "cli_charger_redetect.h"
#include "device_commands.h"

#include <stdio.h>
#include <string.h>

/* docs/charger-redetect.md. The pulse itself lives in the MP2731 driver;
 * this file only parses and prints the before/after register evidence. */

static const char *const SNAP_NAMES[POWER_REDETECT_SNAPSHOTS] = {"before", "after", "+1.5s", "+5s"};

static void print_snapshot(const char *name, const power_charger_snapshot_t *s)
{
    printf("  %-6s t=%6ld ms", name, (long)s->t_ms);
    if (s->mask & (1u << 0x0C)) {
        const uint8_t st = s->regs[0x0C];
        printf("  REG0C=0x%02X VIN_STAT=%u CHG_STAT=%u", st, (st >> 5) & 7u, (st >> 3) & 3u);
    }
    if (s->mask & (1u << 0x14)) printf(" VINPPM=%u", (s->regs[0x14] >> 7) & 1u);
    if (s->mask & (1u << 0x0D)) printf(" REG0D=0x%02X", s->regs[0x0D]);
    printf("\r\n         regs");
    for (unsigned r = 0; r < POWER_CHARGER_REG_COUNT; r++) {
        if (s->mask & (1u << r)) printf(" %02X", s->regs[r]);
        else printf(" --");
    }
    printf("\r\n");
}

int cli_cmd_charger_redetect(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "auto") == 0) {
        int set = -1;
        if (argc == 3 && strcmp(argv[2], "on") == 0) set = 1;
        else if (argc == 3 && strcmp(argv[2], "off") == 0) set = 0;
        else if (argc != 2) {
            printf("Usage: charger_redetect auto [on|off]\r\n");
            return 1;
        }
        power_redetect_auto_t st;
        const cmd_result_t r = cmd_charger_redetect_auto(set, &st);
        printf("%s\r\n", r.message);
        return r.status == ESP_OK ? 0 : 1;
    }
    power_redetect_method_t method = POWER_REDETECT_BG_EN;
    if (argc == 2 && strcmp(argv[1], "dpdm") == 0) {
        method = POWER_REDETECT_DPDM;
    } else if (argc > 2 || (argc == 2 && strcmp(argv[1], "bg_en") != 0)) {
        printf("Usage: charger_redetect [bg_en|dpdm] | charger_redetect auto [on|off]\r\n");
        return 1;
    }
    printf("charger_redetect %s: pulsing, then reading REG00..REG17 for ~5 s...\r\n",
           method == POWER_REDETECT_BG_EN ? "bg_en" : "dpdm");
    power_redetect_report_t rep;
    memset(&rep, 0, sizeof rep);
    const cmd_result_t r = cmd_charger_redetect(method, &rep);
    printf("%s\r\n", r.message);
    for (unsigned i = 0; i < rep.snapshots && i < POWER_REDETECT_SNAPSHOTS; i++) {
        print_snapshot(SNAP_NAMES[i], &rep.snap[i]);
    }
    return r.status == ESP_OK ? 0 : 1;
}
