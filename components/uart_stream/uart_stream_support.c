#include "uart_stream_support.h"

#include <limits.h>
#include <string.h>

static bool relay_deliver(uart_stream_relay_t *relay,
                          uart_stream_relay_sink_fn sink, void *sink_ctx)
{
    if (relay->chunk_len == 0) return true;
    const size_t len = relay->chunk_len;
    if (sink(relay->chunk, len, sink_ctx) != 0) {
        relay->chunk_len = 0;
        return false;
    }
    relay->accepted_bytes += len;
    relay->chunk_len = 0;
    return true;
}

bool uart_stream_relay_init(uart_stream_relay_t *relay, const char *sentinel)
{
    if (relay == NULL || sentinel == NULL) return false;
    const size_t len = strlen(sentinel);
    if (len == 0 || len > UART_STREAM_SENTINEL_MAX_BYTES) return false;
    memset(relay, 0, sizeof(*relay));
    memcpy(relay->sentinel, sentinel, len);
    relay->sentinel_len = len;
    return true;
}

uart_stream_relay_result_t uart_stream_relay_push(
    uart_stream_relay_t *relay, uint8_t byte,
    uart_stream_relay_sink_fn sink, void *sink_ctx)
{
    if (relay == NULL || sink == NULL ||
            relay->chunk_len >= UART_STREAM_CHUNK_MAX) {
        return UART_STREAM_RELAY_SINK_ERROR;
    }

    relay->chunk[relay->chunk_len++] = byte;
    if (byte != '\n') {
        if (relay->tail_len < relay->sentinel_len) {
            relay->tail[relay->tail_len++] = byte;
        } else {
            memmove(relay->tail, relay->tail + 1, relay->sentinel_len - 1U);
            relay->tail[relay->sentinel_len - 1U] = byte;
        }
        if (relay->tail_len == relay->sentinel_len &&
                memcmp(relay->tail, relay->sentinel, relay->sentinel_len) == 0) {
            relay->line_has_sentinel = true;
        }
    }

    const bool line_complete = (byte == '\n');
    if ((relay->chunk_len == UART_STREAM_CHUNK_MAX || line_complete) &&
            !relay_deliver(relay, sink, sink_ctx)) {
        return UART_STREAM_RELAY_SINK_ERROR;
    }

    if (line_complete) {
        if (relay->line_has_sentinel) {
            return UART_STREAM_RELAY_COMPLETE;
        }
        relay->tail_len = 0;
        relay->line_has_sentinel = false;
    }
    return UART_STREAM_RELAY_CONTINUE;
}

bool uart_stream_relay_flush(uart_stream_relay_t *relay,
                             uart_stream_relay_sink_fn sink, void *sink_ctx)
{
    if (relay == NULL || sink == NULL) return false;
    return relay_deliver(relay, sink, sink_ctx);
}

bool uart_stream_hex_encode(const uint8_t *data, size_t len,
                            char *out, size_t out_cap)
{
    if ((data == NULL && len != 0) || out == NULL ||
            len > UART_STREAM_CHUNK_MAX || out_cap < len * 2U + 1U) {
        return false;
    }
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        const uint8_t b = data[i];
        out[i * 2U] = hex[b >> 4];
        out[i * 2U + 1U] = hex[b & 0x0fU];
    }
    out[len * 2U] = '\0';
    return true;
}

static uint64_t ceil_div_u64(uint64_t value, uint64_t divisor)
{
    const uint64_t quotient = value / divisor;
    return quotient + ((value % divisor) != 0U ? 1U : 0U);
}

uint32_t uart_stream_deadline_ticks(int64_t now_us, int64_t deadline_us,
                                    uint32_t tick_period_ms)
{
    if (tick_period_ms == 0 || deadline_us <= now_us) return 0;

    /* Ordering is established in the signed domain first. Converting each
     * operand before subtraction then yields the full mathematical positive
     * delta modulo 2^64, including INT64_MIN -> INT64_MAX, without signed UB. */
    const uint64_t remain_us = (uint64_t)deadline_us - (uint64_t)now_us;
    const uint64_t tick_us = (uint64_t)tick_period_ms * 1000U;
    const uint64_t ticks = ceil_div_u64(remain_us, tick_us);
    return (ticks > UINT32_MAX) ? UINT32_MAX : (uint32_t)ticks;
}

uint32_t uart_stream_capped_deadline_ticks(int64_t now_us,
                                           int64_t deadline_us,
                                           uint32_t cap_ms,
                                           uint32_t tick_period_ms)
{
    if (cap_ms == 0 || tick_period_ms == 0) return 0;

    const uint32_t deadline_ticks =
        uart_stream_deadline_ticks(now_us, deadline_us, tick_period_ms);
    if (deadline_ticks == 0) return 0;

    /* ceil(min(remaining, cap) / tick) is the minimum of the separately
     * ceiling-rounded values. Computing the cap independently also avoids an
     * overflow-prone `now_us + cap_us` intermediate. */
    uint64_t cap_ticks = ceil_div_u64(cap_ms, tick_period_ms);
    if (cap_ticks > UINT32_MAX) cap_ticks = UINT32_MAX;
    return (deadline_ticks < cap_ticks) ? deadline_ticks : (uint32_t)cap_ticks;
}

bool uart_stream_acquire_until(void *first_lock, void *second_lock,
                               int64_t deadline_us, uint32_t tick_period_ms,
                               uart_stream_now_us_fn now_fn,
                               uart_stream_lock_take_fn take_fn,
                               uart_stream_lock_give_fn give_fn,
                               void *ctx)
{
    if (first_lock == NULL || now_fn == NULL || take_fn == NULL ||
            give_fn == NULL || tick_period_ms == 0) {
        return false;
    }

    uint32_t wait = uart_stream_deadline_ticks(now_fn(ctx), deadline_us,
                                               tick_period_ms);
    if (wait == 0 || !take_fn(first_lock, wait, ctx)) return false;
    if (second_lock == NULL) return true;

    wait = uart_stream_deadline_ticks(now_fn(ctx), deadline_us, tick_period_ms);
    if (wait == 0 || !take_fn(second_lock, wait, ctx)) {
        give_fn(first_lock, ctx);
        return false;
    }
    return true;
}
