#include "diag_crash.h"

#include "diag_json.h"

int diag_crash_render_json(const diag_crash_t *c, char *out, size_t cap)
{
    diag_jw_t w;
    diag_jw_init(&w, out, cap);
    if (c == NULL || !c->present) {
        /* An unreadable/absent partition with a reason still says WHY. */
        diag_jw_raw(&w, "{\"present\":false");
        if (c != NULL && c->error != NULL) {
            diag_jw_raw(&w, ",\"error\":");
            diag_jw_str(&w, c->error);
        }
        diag_jw_raw(&w, "}");
        return w.ok ? (int)w.len : -1;
    }
    diag_jw_raw(&w, "{\"present\":true");
    if (c->id != 0) {
        /* Hex string, not a JSON number: 64-bit values lose precision in
         * double-based consumers (Spark/JS), and this is an identity, not a
         * quantity. */
        diag_jw_raw(&w, ",\"id\":\"%08lx%08lx\"",
                    (unsigned long)(c->id >> 32), (unsigned long)(c->id & 0xFFFFFFFFu));
    }
    if (c->first_seen_valid) {
        diag_jw_raw(&w, ",\"first_seen_boot\":%lu", (unsigned long)c->first_seen_boot);
    }
    if (c->error != NULL) {
        diag_jw_raw(&w, ",\"error\":");
        diag_jw_str(&w, c->error);
    } else if (c->summary_valid) {
        diag_jw_raw(&w, ",\"app_elf_sha256\":");
        diag_jw_strn(&w, c->app_elf_sha256, DIAG_CRASH_SHA_HEX);
        diag_jw_raw(&w, ",\"task\":");
        diag_jw_strn(&w, c->task, sizeof c->task - 1);
        /* Addresses as "0x…" strings: what addr2line / idf.py monitor take. */
        diag_jw_raw(&w, ",\"pc\":\"0x%08lx\",\"exc_cause\":%lu,\"exc_vaddr\":\"0x%08lx\",\"backtrace\":[",
                    (unsigned long)c->pc, (unsigned long)c->exc_cause, (unsigned long)c->exc_vaddr);
        uint32_t depth = c->bt_depth > DIAG_CRASH_BT_MAX ? DIAG_CRASH_BT_MAX : c->bt_depth;
        for (uint32_t i = 0; i < depth; i++) {
            diag_jw_raw(&w, "%s\"0x%08lx\"", i ? "," : "", (unsigned long)c->backtrace[i]);
        }
        diag_jw_raw(&w, "],\"bt_corrupted\":%s", c->bt_corrupted ? "true" : "false");
    }
    diag_jw_raw(&w, "}");
    return w.ok ? (int)w.len : -1;
}
