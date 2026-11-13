/* SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Host harness for the production AWS Device Location codec
 * (components/command_router/location_aws_codec.c) with pinned-IDF cJSON.
 * Usage: location_aws_host <op> [args...]; prints one line.
 *   locid <thing>            -> <locId> or "!"
 *   publicip <text>          -> <ip> or "!"
 *   reply <accepted|rejected> <json> -> {"accepted":..,"lat":..,"lon":..,"accuracy_m":..,"error_code":..,"message_id":".."} or "!"
 *   wifi                     -> request body for a fixed scan
 *   ip <ip>                  -> request body
 */
#include "location_aws.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    const char *op = argv[1];
    char out[LOCATION_AWS_REQUEST_MAX];
    if (!strcmp(op, "locid") && argc == 3) {
        puts(location_aws_loc_id(argv[2], out, sizeof out) ? out : "!");
    } else if (!strcmp(op, "locids")) {
        /* One thing name per stdin line (hex-encoded, so any byte fits). */
        char line[1024];
        while (fgets(line, sizeof line, stdin)) {
            char name[512];
            size_t n = strcspn(line, "\n") / 2;
            for (size_t i = 0; i < n; i++) {
                unsigned v;
                if (sscanf(line + 2 * i, "%2x", &v) != 1) return 5;
                name[i] = (char)v;
            }
            name[n] = '\0';
            puts(location_aws_loc_id(name, out, sizeof out) ? out : "!");
        }
    } else if (!strcmp(op, "publicip") && argc == 3) {
        puts(location_aws_public_ip(argv[2], out, sizeof out) ? out : "!");
    } else if (!strcmp(op, "reply") && argc == 4) {
        location_aws_reply_t r;
        if (!location_aws_parse_reply(!strcmp(argv[2], "rejected"), argv[3], strlen(argv[3]), &r)) {
            puts("!");
        } else {
            printf("{\"accepted\":%s,\"lat\":%.6f,\"lon\":%.6f,\"accuracy_m\":%s,\"error_code\":%d,\"message_id\":\"%s\"}\n",
                   r.accepted ? "true" : "false", r.lat, r.lon,
                   isfinite(r.accuracy_m) ? "1" : "null", r.error_code, r.message_id);
            if (isfinite(r.accuracy_m)) fprintf(stderr, "%.1f\n", r.accuracy_m);
        }
    } else if (!strcmp(op, "wifi")) {
        location_aws_ap_t scan[] = {
            {{0x00, 0x00, 0x5E, 0x00, 0x53, 0x01}, -61},
            {{0x02, 0x11, 0x22, 0x33, 0x44, 0x55}, -40},   /* locally administered: dropped */
            {{0x00, 0x00, 0x5E, 0x00, 0x53, 0x02}, -75},
            {{0x00, 0x00, 0x5E, 0x00, 0x53, 0x01}, -61},   /* duplicate: dropped */
            {{0x01, 0x00, 0x5E, 0x00, 0x00, 0x01}, -30},   /* group address: dropped */
            {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, -20},   /* zero: dropped */
            {{0x00, 0x00, 0x5E, 0x00, 0x53, 0x03}, -50},
        };
        location_aws_ap_t aps[LOCATION_AWS_MAX_APS];
        int n = location_aws_select_aps(scan, 7, aps, LOCATION_AWS_MAX_APS);
        size_t len = location_aws_wifi_request("tz-1-w", 1794559977, aps, n, out, sizeof out);
        puts(len ? out : "!");
        /* One usable AP is not enough for the Wi-Fi solver. */
        if (location_aws_wifi_request("x", 0, aps, 1, out, sizeof out) != 0) return 3;
        /* Bounded: a full set fits; too small a buffer refuses. */
        if (location_aws_wifi_request("x", 0, aps, n, out, 40) != 0) return 4;
    } else if (!strcmp(op, "ip") && argc == 3) {
        size_t len = location_aws_ip_request("tz-2-i", 1794559977, argv[2], out, sizeof out);
        puts(len ? out : "!");
    } else {
        return 2;
    }
    return 0;
}
