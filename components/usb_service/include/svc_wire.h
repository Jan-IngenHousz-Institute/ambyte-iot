#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SVC_LINE_MAX 248u
#define SVC_CHUNK_MAX 144u
#define SVC_JSON_MAX 1024u
#define SVC_FRAME_MAX (SVC_JSON_MAX + 19u)

uint32_t svc_crc32(uint32_t previous, const void *data, size_t length);
bool svc_hex(const char *text, size_t digits, uint64_t *value);
void svc_wipe(void *data, size_t length);
bool svc_utf8(const unsigned char *text, size_t length);
int svc_base64_decode(const char *text, unsigned char *out, size_t capacity);
size_t svc_base64_encode(const unsigned char *data, size_t length, char *out);
size_t svc_frame(char *out, size_t capacity, uint32_t correlation, const char *json);

typedef struct {
    char line[SVC_LINE_MAX + 1];
    size_t length;
    int64_t started_ms;
    bool draining;
} svc_rx_t;

/* A complete, verified request stays in rx->line until the caller wipes it.
 * The returned body starts after the correlation ID and has no trailer. */
const char *svc_rx_byte(svc_rx_t *rx, unsigned char byte, int64_t now_ms,
                        char **body, uint32_t *correlation);
const char *svc_rx_timeout(svc_rx_t *rx, int64_t now_ms);
void svc_rx_clear(svc_rx_t *rx);

#define SVC_EDITOR_OUTPUT_MAX (SVC_LINE_MAX + 1u)
typedef struct {
    unsigned char prefix[SVC_EDITOR_OUTPUT_MAX];
    size_t length;
    bool draining;
    bool passthrough;
    bool candidate;
} svc_editor_filter_t;

/* Recognized service headers are protected at EVERY input offset, independent
 * of the editor's control keys, cursor or history state. Output is at most
 * SVC_EDITOR_OUTPUT_MAX bytes. dropped is set at the drained line terminator. */
size_t svc_editor_byte(svc_editor_filter_t *filter, unsigned char byte,
                       unsigned char output[SVC_EDITOR_OUTPUT_MAX], bool *dropped);
