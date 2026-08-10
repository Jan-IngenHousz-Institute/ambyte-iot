#ifndef AMBYTE_UART_STREAM_CLI_SUPPORT_H
#define AMBYTE_UART_STREAM_CLI_SUPPORT_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Validate one bounded openJII request before the CLI prints BEGIN or acquires
 * any transaction/UART lock. Only object/array roots are accepted; cJSON must
 * consume the complete input except trailing JSON whitespace. */
bool uart_stream_json_request_valid(const char *request, size_t max_len);

#ifdef __cplusplus
}
#endif

#endif /* AMBYTE_UART_STREAM_CLI_SUPPORT_H */
