#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "command_router_pong.h"
#include "ota_identity.h"

int main(void)
{
    char json[448], small[16];
    assert(!ota_running_identity_json(small, sizeof small));
    assert(!command_router_pong_json(small, sizeof small, "ping-test", "device", "2.8.0", 42, "panic", 7));
    /* The production reply buffer (command_router.c handle_ping) is 448 B:
     * the additive reset_reason/boot_count must still fit with the longest
     * reason name and a u32-max counter. */
    char probe[448];
    assert(command_router_pong_json(probe, sizeof probe, "ping-test", "device", "2.8.0", 42,
                                    "pwr_glitch", 4294967295u));
    assert(command_router_pong_json(json, sizeof json, "ping-test", "device", "2.8.0", 42, "panic", 7));
    assert(strstr(json, "\"reset_reason\":\"panic\",\"boot_count\":7,"));
    puts(json);
    return 0;
}
