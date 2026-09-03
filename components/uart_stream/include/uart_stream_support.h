#ifndef AMBYTE_UART_STREAM_SUPPORT_H
#define AMBYTE_UART_STREAM_SUPPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UART_STREAM_CHUNK_MAX          128U
#define UART_STREAM_SENTINEL_MAX_BYTES  32U

typedef int (*uart_stream_relay_sink_fn)(const uint8_t *data, size_t len,
                                         void *ctx);

typedef enum {
    UART_STREAM_RELAY_CONTINUE = 0,
    UART_STREAM_RELAY_COMPLETE,
    UART_STREAM_RELAY_SINK_ERROR,
} uart_stream_relay_result_t;

/* Response-size-independent scanner + chunker used by the firmware UART loop
 * and the host regression harness. A sink call is accepted only when it returns
 * zero; accepted_bytes therefore always matches whole successfully delivered
 * chunks. */
typedef struct {
    uint8_t chunk[UART_STREAM_CHUNK_MAX];
    size_t chunk_len;
    uint8_t sentinel[UART_STREAM_SENTINEL_MAX_BYTES];
    size_t sentinel_len;
    uint8_t tail[UART_STREAM_SENTINEL_MAX_BYTES];
    size_t tail_len;
    bool line_has_sentinel;
    size_t accepted_bytes;
} uart_stream_relay_t;

bool uart_stream_relay_init(uart_stream_relay_t *relay, const char *sentinel);
uart_stream_relay_result_t uart_stream_relay_push(
    uart_stream_relay_t *relay, uint8_t byte,
    uart_stream_relay_sink_fn sink, void *sink_ctx);
bool uart_stream_relay_flush(uart_stream_relay_t *relay,
                             uart_stream_relay_sink_fn sink, void *sink_ctx);

/* Encode one bounded callback as lowercase hex. The caller supplies at least
 * 2*len+1 bytes. Keeping this in the shared helper lets the host harness execute
 * the exact production codec used by CLI DATA records. */
bool uart_stream_hex_encode(const uint8_t *data, size_t len,
                            char *out, size_t out_cap);

/* Generic two-lock acquisition under ONE absolute deadline. Firmware adapters
 * supply FreeRTOS semaphore callbacks; host tests supply deterministic fakes.
 * If the optional second lock cannot be acquired, the first is rolled back. */
typedef int64_t (*uart_stream_now_us_fn)(void *ctx);
typedef bool (*uart_stream_lock_take_fn)(void *lock, uint32_t wait_ticks,
                                         void *ctx);
typedef void (*uart_stream_lock_give_fn)(void *lock, void *ctx);

uint32_t uart_stream_deadline_ticks(int64_t now_us, int64_t deadline_us,
                                    uint32_t tick_period_ms);
uint32_t uart_stream_capped_deadline_ticks(int64_t now_us,
                                           int64_t deadline_us,
                                           uint32_t cap_ms,
                                           uint32_t tick_period_ms);
bool uart_stream_acquire_until(void *first_lock, void *second_lock,
                               int64_t deadline_us, uint32_t tick_period_ms,
                               uart_stream_now_us_fn now_fn,
                               uart_stream_lock_take_fn take_fn,
                               uart_stream_lock_give_fn give_fn,
                               void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* AMBYTE_UART_STREAM_SUPPORT_H */
