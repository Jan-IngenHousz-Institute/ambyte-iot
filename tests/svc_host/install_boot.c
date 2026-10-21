/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L

/* Exercise the runner's real boot read, including recovery, without creating
 * a task. The install adapter supplies its own live-runner ports; unrelated
 * task/identity paths in this translation unit are discarded by section GC. */
#define sched_runner_identity install_boot_unused_identity
#define sched_runner_is_running install_boot_unused_running
#include "../../components/sched_runner/sched_runner.c"

esp_err_t install_boot_read(char **bytes, size_t *length)
{
    return read_schedule_file(SCHED_PATH, bytes, length);
}
