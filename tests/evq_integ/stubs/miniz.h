/* Host stub of the ESP32-S3 ROM tdefl surface device_commands.c references.
 * The harness never enables publish_gzip, so both functions abort if reached. */
#pragma once
#include <stddef.h>
typedef enum { TDEFL_STATUS_BAD_PARAM = -2, TDEFL_STATUS_PUT_BUF_FAILED = -1,
               TDEFL_STATUS_OKAY = 0, TDEFL_STATUS_DONE = 1 } tdefl_status;
typedef enum { TDEFL_NO_FLUSH = 0, TDEFL_SYNC_FLUSH = 2, TDEFL_FULL_FLUSH = 3, TDEFL_FINISH = 4 } tdefl_flush;
typedef int (*tdefl_put_buf_func_ptr)(const void *pBuf, int len, void *pUser);
typedef struct { unsigned char opaque[64]; } tdefl_compressor;
tdefl_status tdefl_init(tdefl_compressor *d, tdefl_put_buf_func_ptr f, void *u, int flags);
tdefl_status tdefl_compress(tdefl_compressor *d, const void *in, size_t *in_sz, void *out, size_t *out_sz, tdefl_flush flush);
