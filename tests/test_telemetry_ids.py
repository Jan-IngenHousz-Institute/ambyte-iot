"""Run production ID initialization/allocation without a mounted event store."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    start = source.index(f"esp_err_t {name}(")
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class TelemetryIdsTest(unittest.TestCase):
    def test_offline_allocator_is_idempotent_and_reserves_across_reboot(self):
        source = (ROOT / "components/event_log/event_log.c").read_text()
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#define ESP_ERR_TIMEOUT 0x107
#define ESP_LOGW(...) ((void)0)
#define NVS_NS "evlog"
#define NVS_KEY_NID "nid"
#define NVS_READONLY 0
#define EVLOG_ID_BLOCK 64
#define pdMS_TO_TICKS(n) (n)
#define pdTRUE 1
static void *s_mtx;
static int s_mtx_storage;
static int64_t s_next_id = 1, s_id_limit = 1;
static bool s_available, mounted;
static uint64_t persisted = 129;
typedef int nvs_handle_t;
static void *xSemaphoreCreateMutexStatic(int *p) { return p; }
static int xSemaphoreTake(void *p, int timeout) { (void)timeout; return p != NULL; }
static void xSemaphoreGive(void *p) { assert(p); }
static int nvs_open(const char *ns, int mode, nvs_handle_t *h) {
    (void)ns; (void)mode; *h = 1; return ESP_OK;
}
static void nvs_get_u64(nvs_handle_t h, const char *key, uint64_t *out) {
    (void)h; (void)key; *out = persisted;
}
static void nvs_close(nvs_handle_t h) { (void)h; }
static void evlog_persist_nid_locked(void) { persisted = s_id_limit; }
static esp_err_t evlog_allocate_line_buffer(void) { return ESP_OK; }
static esp_err_t evlog_open_locked(void) { return mounted ? ESP_OK : ESP_FAIL; }
'''
        harness += "\n".join(function(source, n) for n in ("event_log_init_ids", "event_log_init", "event_log_next_id"))
        harness += r'''
int main(void) {
    int64_t first, second, after_reboot;
    assert(event_log_init_ids() == ESP_OK);
    assert(!s_available); /* No mount and no event_log_init needed for heartbeat IDs. */
    assert(event_log_next_id(&first) == ESP_OK && first == 129);
    assert(persisted == 193);
    assert(event_log_init_ids() == ESP_OK); /* Must not reset back to the NVS frontier. */
    assert(event_log_init() == ESP_OK && !s_available);
    assert(event_log_next_id(&second) == ESP_OK && second == first + 1);
    mounted = true;
    assert(event_log_init() == ESP_OK && s_available);
    assert(event_log_next_id(&second) == ESP_OK && second == first + 2);
    s_mtx = NULL; s_available = false; s_next_id = s_id_limit = 1; /* Simulated reset. */
    assert(event_log_init_ids() == ESP_OK);
    assert(event_log_next_id(&after_reboot) == ESP_OK && after_reboot == 193);
    assert(after_reboot > second);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "ids.c"
            path.write_text(harness)
            binary = Path(tmp) / "ids"
            subprocess.run([os.environ.get("CC", shutil.which("clang") or "cc"), "-std=c11",
                            "-Wall", "-Wextra", "-Werror", f"-I{ROOT / 'tests/heartbeat_stubs'}",
                            str(path), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
