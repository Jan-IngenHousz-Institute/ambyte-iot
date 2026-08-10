#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "uart_stream_cli_support.h"
#include "uart_stream_support.h"

#define TOKEN "7A1E3AA1"

typedef struct {
    uint8_t data[1024];
    size_t len;
    size_t calls;
    size_t fail_call;
} capture_t;

static int capture_sink(const uint8_t *data, size_t len, void *opaque)
{
    capture_t *capture = (capture_t *)opaque;
    capture->calls++;
    if (capture->fail_call != 0 && capture->calls == capture->fail_call) {
        return -1;
    }
    assert(capture->len + len <= sizeof(capture->data));
    memcpy(capture->data + capture->len, data, len);
    capture->len += len;
    return 0;
}

static uart_stream_relay_result_t feed(uart_stream_relay_t *relay,
                                       capture_t *capture,
                                       const uint8_t *data, size_t len)
{
    uart_stream_relay_result_t result = UART_STREAM_RELAY_CONTINUE;
    for (size_t i = 0; i < len; i++) {
        result = uart_stream_relay_push(relay, data[i], capture_sink, capture);
        if (result != UART_STREAM_RELAY_CONTINUE) return result;
    }
    return result;
}

static void test_sentinel_at_boundary(size_t prefix_len)
{
    uint8_t input[256];
    assert(prefix_len + strlen(TOKEN) + 1U <= sizeof(input));
    memset(input, 'x', prefix_len);
    memcpy(input + prefix_len, TOKEN, strlen(TOKEN));
    input[prefix_len + strlen(TOKEN)] = '\n';
    const size_t input_len = prefix_len + strlen(TOKEN) + 1U;

    uart_stream_relay_t relay;
    capture_t capture = {0};
    assert(uart_stream_relay_init(&relay, TOKEN));
    assert(feed(&relay, &capture, input, input_len - 1U) ==
           UART_STREAM_RELAY_CONTINUE);
    assert(feed(&relay, &capture, input + input_len - 1U, 1U) ==
           UART_STREAM_RELAY_COMPLETE);
    assert(relay.accepted_bytes == input_len);
    assert(capture.len == input_len);
    assert(memcmp(capture.data, input, input_len) == 0);
}

static void test_scanner_and_sink(void)
{
    /* Token wholly before, straddling, and wholly after the 128-byte flush. */
    test_sentinel_at_boundary(10U);
    test_sentinel_at_boundary(124U);
    test_sentinel_at_boundary(140U);

    uart_stream_relay_t relay;
    capture_t capture = {0};
    assert(uart_stream_relay_init(&relay, TOKEN));
    assert(feed(&relay, &capture, (const uint8_t *)TOKEN, strlen(TOKEN)) ==
           UART_STREAM_RELAY_CONTINUE);
    assert(uart_stream_relay_flush(&relay, capture_sink, &capture));
    assert(relay.accepted_bytes == strlen(TOKEN));
    assert(capture.len == strlen(TOKEN));

    memset(&capture, 0, sizeof(capture));
    assert(uart_stream_relay_init(&relay, TOKEN));
    const char crlf[] = TOKEN "\r\n";
    assert(feed(&relay, &capture, (const uint8_t *)crlf, strlen(crlf)) ==
           UART_STREAM_RELAY_COMPLETE);
    assert(capture.len == strlen(crlf));

    memset(&capture, 0, sizeof(capture));
    assert(uart_stream_relay_init(&relay, TOKEN));
    const char lines[] = "unrelated\n7A1E3AA0\nstill unrelated\n" TOKEN "\n";
    assert(feed(&relay, &capture, (const uint8_t *)lines, strlen(lines)) ==
           UART_STREAM_RELAY_COMPLETE);
    assert(capture.len == strlen(lines));
    assert(memcmp(capture.data, lines, strlen(lines)) == 0);

    /* Timeout flushes an unterminated final chunk exactly once. */
    memset(&capture, 0, sizeof(capture));
    assert(uart_stream_relay_init(&relay, TOKEN));
    const char partial[] = "partial final chunk";
    assert(feed(&relay, &capture, (const uint8_t *)partial, strlen(partial)) ==
           UART_STREAM_RELAY_CONTINUE);
    assert(relay.accepted_bytes == 0);
    assert(uart_stream_relay_flush(&relay, capture_sink, &capture));
    assert(relay.accepted_bytes == strlen(partial));
    assert(capture.len == strlen(partial));

    /* A failed second callback is not counted; the first 128 bytes remain. */
    memset(&capture, 0, sizeof(capture));
    capture.fail_call = 2U;
    assert(uart_stream_relay_init(&relay, TOKEN));
    uint8_t failure_input[UART_STREAM_CHUNK_MAX + 1U];
    memset(failure_input, 'z', sizeof(failure_input));
    failure_input[sizeof(failure_input) - 1U] = '\n';
    assert(feed(&relay, &capture, failure_input, sizeof(failure_input)) ==
           UART_STREAM_RELAY_SINK_ERROR);
    assert(capture.calls == 2U);
    assert(capture.len == UART_STREAM_CHUNK_MAX);
    assert(relay.accepted_bytes == UART_STREAM_CHUNK_MAX);

    char encoded[16];
    const uint8_t exact[] = {'{', '}', '\n'};
    assert(uart_stream_hex_encode(exact, sizeof(exact), encoded, sizeof(encoded)));
    assert(strcmp(encoded, "7b7d0a") == 0);
}

typedef struct {
    int64_t now_us;
    int64_t advance_after_first_take_us;
    uint32_t waits[4];
    size_t takes;
    size_t fail_take;
    size_t gives;
} lock_fixture_t;

static int64_t fake_now(void *opaque)
{
    return ((lock_fixture_t *)opaque)->now_us;
}

static bool fake_take(void *lock, uint32_t wait_ticks, void *opaque)
{
    (void)lock;
    lock_fixture_t *fixture = (lock_fixture_t *)opaque;
    fixture->takes++;
    fixture->waits[fixture->takes - 1U] = wait_ticks;
    if (fixture->takes == 1U && fixture->advance_after_first_take_us != 0) {
        fixture->now_us = fixture->advance_after_first_take_us;
    }
    return fixture->fail_take == 0 || fixture->takes != fixture->fail_take;
}

static void fake_give(void *lock, void *opaque)
{
    (void)lock;
    ((lock_fixture_t *)opaque)->gives++;
}

static void test_deadline_acquisition(void)
{
    int first_lock = 1;
    int second_lock = 2;

    lock_fixture_t fixture = {.advance_after_first_take_us = 90000};
    assert(uart_stream_acquire_until(&first_lock, &second_lock, 100000, 1,
                                     fake_now, fake_take, fake_give, &fixture));
    assert(fixture.takes == 2U);
    assert(fixture.waits[0] == 100U);
    assert(fixture.waits[1] == 10U);
    assert(fixture.gives == 0U);

    fixture = (lock_fixture_t){.advance_after_first_take_us = 100000};
    assert(!uart_stream_acquire_until(&first_lock, &second_lock, 100000, 1,
                                      fake_now, fake_take, fake_give, &fixture));
    assert(fixture.takes == 1U);
    assert(fixture.gives == 1U); /* first mutex rolled back at deadline */

    fixture = (lock_fixture_t){
        .advance_after_first_take_us = 50000,
        .fail_take = 2U,
    };
    assert(!uart_stream_acquire_until(&first_lock, &second_lock, 100000, 1,
                                      fake_now, fake_take, fake_give, &fixture));
    assert(fixture.waits[0] == 100U);
    assert(fixture.waits[1] == 50U);
    assert(fixture.gives == 1U);

    fixture = (lock_fixture_t){.now_us = 100000};
    assert(!uart_stream_acquire_until(&first_lock, &second_lock, 100000, 1,
                                      fake_now, fake_take, fake_give, &fixture));
    assert(fixture.takes == 0U);
    assert(fixture.gives == 0U);

    assert(uart_stream_deadline_ticks(0, 95000, 10) == 10U);
    assert(uart_stream_deadline_ticks(91000, 95000, 10) == 1U);

    /* The wake read uses this exact production calculation: a positive
     * sub-tick remainder gets one tick, a long remainder is capped at 20 ms,
     * and an expired deadline does not begin another read. */
    assert(uart_stream_capped_deadline_ticks(95001, 100000, 20, 10) == 1U);
    assert(uart_stream_capped_deadline_ticks(0, 100000, 20, 10) == 2U);
    assert(uart_stream_capped_deadline_ticks(100000, 100000, 20, 10) == 0U);
    assert(uart_stream_capped_deadline_ticks(INT64_MIN, INT64_MAX, 20, 10) ==
           2U);
}

static void test_json_validation(void)
{
    assert(uart_stream_json_request_valid("{}", 448));
    assert(uart_stream_json_request_valid("[{}]   \t", 448));
    assert(!uart_stream_json_request_valid("{not-json}", 448));
    assert(!uart_stream_json_request_valid("[1,]", 448));
    assert(!uart_stream_json_request_valid("{}garbage", 448));
    assert(!uart_stream_json_request_valid("[]  x", 448));
    assert(!uart_stream_json_request_valid("\"scalar\"", 448));
    assert(!uart_stream_json_request_valid("{}\n", 448));
}

int main(void)
{
    test_scanner_and_sink();
    test_deadline_acquisition();
    test_json_validation();
    puts("uart stream production helper tests: ok");
    return 0;
}
