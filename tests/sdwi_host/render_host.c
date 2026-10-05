/* evlog render + heartbeat storage rendering with the new diagnostic fields
 * (contract C-32): production evq_render.c and payload_v3.c. */
#include <stdio.h>
#include <string.h>
#include "event_log.h"
#include "payload_v3.h"

int main(void)
{
    evlog_health_t h;
    memset(&h, 0, sizeof h);
    h.available = true;
    h.pending = 0;
    h.pending_exact = false;           /* a floor: must never read as exact-empty */
    h.sd_rename_ambiguous = 3;
    h.sd_verify_fail = 2;
    char text[1536];
    if (evq_render_health_text(&h, text, sizeof text) < 0) return 1;
    for (char *p = text; *p; p++) if (*p == '\r' || *p == '\n') *p = '|';
    printf("EVLOG=%s\n", text);

    payload_v3_telemetry_input_t in;
    memset(&in, 0, sizeof in);
    in.measure_id = 7;
    in.device = "28:37:2F:FF:E7:04";
    in.observed_utc_ms = 1785965213985LL;
    in.evq_valid = true;
    in.evq_pending = 5;
    in.evq_pending_exact = false;
    in.evq_sd_state = "absent";
    in.evq_head_block = "none";
    in.evq_blocked_reason = "flash_full_sd_unavailable";
    in.evq_corrupt_medium = "none";
    in.evq_refused_full = 7;
    in.evq_sd_rename_ambiguous = 3;
    in.evq_sd_verify_fail = 2;
    in.sd_diag_json = "{\"exact\":false,\"epoch\":2,\"boot\":9,\"faults\":{\"sdlog.fsync\":1},"
                      "\"refused\":{\"full\":7,\"media\":0,\"too_large\":0,\"unavailable\":0,\"first_id\":100,\"last_id\":106}}";
    in.sdlog_json = "{\"quarantined\":false,\"rolled_back\":12}";
    char out[6144], err[128];
    if (!payload_v3_build_telemetry(out, sizeof out, &in, err, sizeof err)) { printf("ERR=%s\n", err); return 1; }
    printf("TELEMETRY=%s\n", out);
    in.sd_diag_json = NULL;
    in.sdlog_json = NULL;
    if (!payload_v3_build_telemetry(out, sizeof out, &in, err, sizeof err)) { printf("ERR=%s\n", err); return 1; }
    printf("TELEMETRY_NODIAG=%s\n", out);
    return 0;
}
