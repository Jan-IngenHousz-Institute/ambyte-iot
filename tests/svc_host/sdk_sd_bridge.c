/* Actual SD producer tee. The writer/file path is outside this USB test. */
#include "../../components/sd_logger/sd_logger.c"
#include <assert.h>
void sdk_sd_log(const char *fmt, ...)
{
    s_prev_vprintf = vprintf;
    va_list ap; va_start(ap, fmt); (void)sd_log_vprintf(fmt, ap); va_end(ap);
}
void sdk_sd_check(void)
{
    uint8_t bytes[SD_LOGGER_CHUNK + 1];
    size_t n = ring_pop(bytes, SD_LOGGER_CHUNK); assert(n && n <= SD_LOGGER_CHUNK);
    bytes[n] = 0; assert(strstr((char *)bytes, "W (0) sync_runner: sentinel") && bytes[n-1] == '\n');
}
