#include "uart_stream_cli_support.h"

#include <string.h>

#include "cJSON.h"

bool uart_stream_json_request_valid(const char *request, size_t max_len)
{
    if (request == NULL || max_len == 0) return false;
    const size_t len = strlen(request);
    if (len < 2U || len > max_len || strpbrk(request, "\r\n") != NULL) {
        return false;
    }

    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithOpts(request, &parse_end, true);
    if (root == NULL) return false;
    const bool valid = parse_end != NULL && *parse_end == '\0' &&
                       (cJSON_IsObject(root) || cJSON_IsArray(root));
    cJSON_Delete(root);
    return valid;
}
