#pragma once

/* Composition-root predicate shared with the host integration harness. Define
 * app_maint_submit before including: service jobs use that raw queue port and
 * only legacy component configurations receive this filtered port. */
static bool app_maint_submit_legacy(void (*run)(void *arg), void *arg)
{
    return !usb_service_wifi_busy() && app_maint_submit(run, arg);
}
