#pragma once

/* Deterministic synthetic payload for the SD-overflow hardware verification
 * (docs/evq-sd-overflow-hil-contract.md §1.4). Pure C, compiled unchanged by
 * the device (evq_hil) and by the host tests (PAY-1), so the host recomputes
 * every accepted payload from (run, k, pad_len) alone.
 *
 * Canonical compact JSON, exactly:
 *   {"evq_hil":"<run>","k":<k>,"pad":"<p>"}
 * <p> is pad_len chars of [a-z0-9] from xorshift64 seeded by
 * fnv1a64(run) XOR (k * 0x9E3779B97F4A7C15), incompressible and reproducible. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint64_t evq_hil_fnv1a64(const char *s);

/* Writes the payload + NUL into out. Returns its length (without NUL), or 0
 * when cap is too small or run contains a character outside [A-Za-z0-9._-]. */
size_t evq_hil_payload(const char *run, uint32_t k, size_t pad_len, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
