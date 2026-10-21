/* Real pinned editor + service read filter. Peripheral-only ports are shared
 * with the USB boundary harness; editor code and control handling are real. */
#define SVC_REAL_EDITOR
#include "usb_driver.c"

static void framed_notice(const char *reason)
{
    assert(!strcmp(reason, "mode_required"));
    char frame[200];
    const char *json = "{\"ok\":false,\"store\":\"0123456789abcdef\",\"reason\":\"mode_required\"}";
    size_t n = svc_frame(frame, sizeof frame, 0, json); assert(n);
    assert(svc_usb_notice_write(frame, n));
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    bool dumb = !strcmp(argv[2], "dumb");
    int descriptors[2]; assert(pipe(descriptors) == 0);
    const char probes[] = "\x1b[1;1R\x1b[1;80R";
    if (!dumb) assert(write(descriptors[1], probes, sizeof probes - 1) == sizeof probes - 1);
    char frame[248];
    size_t n = svc_frame(frame, sizeof frame, 1,
        "svc.wifi.join 0000000000000001 QTNCZW5jaA== d3JvbmdwYXNz");
    assert(n && write(descriptors[1], "stray", 5) == 5);
    assert(write(descriptors[1], frame, n) == (ssize_t)n);
    if (!dumb) assert(write(descriptors[1], probes, sizeof probes - 1) == sizeof probes - 1);
    assert(write(descriptors[1], "identity\n", 9) == 9);
    close(descriptors[1]); assert(dup2(descriptors[0], STDIN_FILENO) >= 0); close(descriptors[0]);
    linenoiseSetDumbMode(dumb);
    linenoiseSetReadCharacteristics();
    svc_usb_notice_handler(framed_notice);
    char *line = linenoise("ambyte> ");
    assert(!line);  /* entire preceding stray edit was cancelled, never run */
    line = linenoise("ambyte> ");
    assert(line && !strcmp(line, "identity"));
    assert(linenoiseHistoryAdd(line)); linenoiseFree(line);
    assert(linenoiseHistorySave(argv[1]) == 0);
    assert(!s_notice_active && !s_active);
    puts("REAL_EDITOR_IDENTITY_OK");
    return 0;
}
