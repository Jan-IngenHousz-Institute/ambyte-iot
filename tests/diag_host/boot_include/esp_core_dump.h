#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
/* Layout of IDF 5.5's ELF-format summary (xtensa port) — only the members
 * diag_boot.c reads. */
typedef struct { uint32_t bt[16]; uint32_t depth; bool corrupted; } esp_core_dump_bt_info_t;
typedef struct { uint32_t exc_cause; uint32_t exc_vaddr; uint32_t exc_a[16]; uint32_t epcx[7];
                 uint8_t epcx_reg_bits; } esp_core_dump_summary_extra_info_t;
typedef struct {
    uint32_t exc_tcb;
    char exc_task[16];
    uint32_t exc_pc;
    esp_core_dump_bt_info_t exc_bt_info;
    uint32_t core_dump_version;
    uint8_t app_elf_sha256[65];
    esp_core_dump_summary_extra_info_t ex_info;
} esp_core_dump_summary_t;
esp_err_t esp_core_dump_image_check(void);
esp_err_t esp_core_dump_get_summary(esp_core_dump_summary_t *summary);
