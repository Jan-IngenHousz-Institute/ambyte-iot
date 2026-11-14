#include "diag_reset.h"

const char *diag_reset_reason_name(esp_reset_reason_t reason)
{
    /* A switch, not an index table: the IDF enum is not guaranteed dense or
     * stable across releases, and an out-of-range value from a newer IDF must
     * read "unknown", never a neighbour's name. */
    switch (reason) {
    case ESP_RST_POWERON:    return "poweron";
    case ESP_RST_BROWNOUT:   return "brownout";
    case ESP_RST_PANIC:      return "panic";
    case ESP_RST_INT_WDT:    return "int_wdt";
    case ESP_RST_TASK_WDT:   return "task_wdt";
    case ESP_RST_WDT:        return "other_wdt";
    case ESP_RST_SW:         return "sw";
    case ESP_RST_DEEPSLEEP:  return "deepsleep";
    case ESP_RST_EXT:        return "ext";
    case ESP_RST_SDIO:       return "sdio";
    case ESP_RST_USB:        return "usb";
    case ESP_RST_JTAG:       return "jtag";
    case ESP_RST_EFUSE:      return "efuse";
    case ESP_RST_PWR_GLITCH: return "pwr_glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    case ESP_RST_UNKNOWN:
    default:                 return "unknown";
    }
}
