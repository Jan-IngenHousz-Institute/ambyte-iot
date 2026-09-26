/* PAY-1 host driver: print evq_hil_payload() for (run, k, pad) triples read
 * from stdin, one per line, so the Python twin can be compared byte for byte. */
#include <stdio.h>
#include <stdlib.h>

#include "evq_hil_payload.h"

int main(void)
{
    char run[64];
    unsigned k, pad;
    static char out[70000];
    while (scanf("%63s %u %u", run, &k, &pad) == 3) {
        size_t n = evq_hil_payload(run, k, pad, out, sizeof out);
        if (n == 0) { printf("ERR\n"); continue; }
        fwrite(out, 1, n, stdout);
        putchar('\n');
    }
    return 0;
}
