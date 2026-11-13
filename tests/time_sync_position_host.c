/* SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * time_sync's position + generation under concurrent writer/reader tasks
 * (Opus review of #100): a reader that sees generation N must see position N
 * or newer, and a snapshot must never pair one move's latitude with another's
 * longitude. Built with ThreadSanitizer: an unlocked implementation is a
 * reported data race even when the bad interleaving does not happen. */
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include "time_sync.h"

#define MOVES 20000
static double lat_of(uint32_t k) { return k * 1e-3; }   /* < 90 for every k */
static atomic_int stop;

static void *writer(void *arg)
{
    (void)arg;
    for (uint32_t k = 1; k <= MOVES; k++) time_sync_set_position(lat_of(k), -lat_of(k));
    atomic_store(&stop, 1);
    return NULL;
}

int main(void)
{
    time_sync_set_position(lat_of(0), -lat_of(0));
    uint32_t base = time_sync_position_generation();
    pthread_t t;
    if (pthread_create(&t, NULL, writer, NULL) != 0) return 2;
    long checks = 0;
    while (!atomic_load(&stop)) {
        uint32_t g = time_sync_position_generation();
        double lat, lon;
        time_sync_get_location(&lat, &lon, NULL);
        if (lat + 1e-9 < lat_of(g - base)) {
            fprintf(stderr, "generation %u with an older position %.4f\n", g - base, lat);
            return 1;
        }
        uint32_t sg;
        time_sync_get_position(&lat, &lon, &sg);
        if (fabs(lat - lat_of(sg - base)) > 1e-9 || lat != -lon) {
            fprintf(stderr, "torn snapshot gen %u lat %.4f lon %.4f\n", sg - base, lat, lon);
            return 1;
        }
        checks++;
    }
    pthread_join(t, NULL);
    if (time_sync_position_generation() - base != MOVES) return 1;
    printf("TIME_SYNC_POSITION_OK %ld\n", checks);
    return 0;
}
