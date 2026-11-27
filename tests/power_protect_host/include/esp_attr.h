/* Host stand-in: RTC_NOINIT_ATTR is ordinary static storage here. The harness
 * (power_protect_host.c reboot()) models it: kept across resets without power
 * loss, garbage after power-on / brownout. */
#pragma once
#define RTC_NOINIT_ATTR
