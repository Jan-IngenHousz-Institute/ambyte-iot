#include <assert.h>
#include <stdio.h>
#include "command_router_pong.h"
#include "ota_identity.h"

int main(void)
{
    char json[448], small[16];
    assert(!ota_running_identity_json(small, sizeof small));
    assert(!command_router_pong_json(small, sizeof small, "ping-test", "device", "2.8.0", 42));
    assert(command_router_pong_json(json, sizeof json, "ping-test", "device", "2.8.0", 42));
    puts(json);
    return 0;
}
