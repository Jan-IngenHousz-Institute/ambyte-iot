#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "messaging_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Live location from AWS IoT Core Device Location over the existing MQTT
 * connection (docs/timezone-from-location.md). The Ambyte has no GPS and the
 * fleet (~500 units) has no other location hardware; this needs no key on
 * the device and no second TLS session.
 *
 * Contract (verified on DEV eu-central-1 by the openJII policy work, 2026-11-13):
 *   publish   $aws/device_location/<locId>/get_position_estimate
 *   subscribe .../get_position_estimate/accepted and /rejected
 *   <locId>   = thing name (== MQTT client id) with every byte outside
 *               [A-Za-z0-9] -- '-' included -- replaced by '-' and two
 *               UPPERCASE hex digits: ambyte_28:37:2F:FF:E7:04 ->
 *               ambyte-5F28-3A37-3A2F-3AFF-3AE7-3A04. Injective, so edge_box,
 *               edge-box and edge:box never share an id (and its grant). AWS
 *               allows only [A-Za-z0-9-] and <= 128 chars there and silently
 *               drops other ids; a longer result keeps the client inert (never
 *               truncated). The policy binds the thing attribute
 *               deviceLocationId, which the platform sets with the same rule
 *               (open-jii #2430).
 *   request   strict JSON, only Timestamp (STRING of unix s), MessageId,
 *             WiFiAccessPoints [{MacAddress, Rss}] OR Ip {IpAddress}. Never
 *             both: unknown APs + IP is a 404, not an IP fallback. AWS does
 *             not use the connection's source IP, so the device sends its
 *             public IP, learned over PLAIN HTTP from checkip.amazonaws.com
 *             (this board cannot hold a second TLS session; a spoofed IP only
 *             shifts a coarse estimate, which the country rule bounds).
 *   accepted  GeoJSON Point: coordinates [lon, lat], properties
 *             {horizontalAccuracy m, messageId, ...}
 *   rejected  {errorCode 400|403|404|500, errorMessage, messageId}
 *
 * Inert unless BOTH reply subscriptions are granted on the current
 * connection (as iot_jobs): certificates without the device-location policy
 * see only refused SUBACKs (MQTT 5 reason 0x87, no disconnect) and nothing is
 * ever published on the request topic -- no refused PUBACKs.
 *
 * Cadence: first attempt 1-10 min (jittered) after the first granted
 * connection of a boot, then every 6 h (+-10 % jitter: AWS's default quota is
 * 10 TPS per account, shared by the fleet); after a failed attempt the
 * interval doubles up to 24 h. One attempt = Wi-Fi scan (>= 2 globally
 * administered BSSIDs) -> on 404 or too few APs, the IP form. A reply must
 * arrive within 5 s. `cfg set loc_live off` disables it. */

#define LOCATION_AWS_ID_MAX     129     /* AWS thing names are <= 128 */
#define LOCATION_AWS_MAX_APS    8
#define LOCATION_AWS_REQUEST_MAX 640

typedef struct {
    uint8_t bssid[6];
    int8_t rssi;
} location_aws_ap_t;

typedef struct {
    const char                  *thing_name;
    message_publish_fn           publish;
    message_add_subscription_fn  add_subscription;
    message_connection_stats_fn  connection_stats;
    bool                       (*submit)(void (*run)(void *), void *arg);
    bool                       (*maintenance_begin)(void);
    void                       (*maintenance_end)(void);
    /* Blocking Wi-Fi scan while connected; returns the number of records. */
    int                        (*wifi_scan)(location_aws_ap_t *aps, int max);
    /* Public IP as text into out (plain HTTP GET, short timeout). */
    bool                       (*public_ip)(char *out, size_t len);
} location_aws_config_t;

/* Register the reply subscriptions. Call before the MQTT client starts. */
esp_err_t location_aws_init(const location_aws_config_t *cfg);

/* One attempt now (USB `locate [ip]`, for maintenance and bench evidence),
 * on the worker like a scheduled one; `ip_only` skips the Wi-Fi scan. Still
 * refused unless the policy grant holds on this connection. Returns
 * "queued" | "not_authorized" | "not_started". */
const char *location_aws_request_now(bool ip_only);

/* ── pure helpers (host-tested) ─────────────────────────────────────────── */

/* Thing name -> <locId>. False if empty or the id would exceed 128 chars. */
bool location_aws_loc_id(const char *thing_name, char *out, size_t len);
/* Keep globally administered unicast BSSIDs (randomised / phone-hotspot MACs
 * are never in a location database), strongest first, at most `max_out`. */
int location_aws_select_aps(const location_aws_ap_t *in, int n, location_aws_ap_t *out, int max_out);
/* Request bodies. Return the length, or 0 if it does not fit. */
size_t location_aws_wifi_request(const char *message_id, int64_t now_utc,
                                 const location_aws_ap_t *aps, int n, char *out, size_t cap);
size_t location_aws_ip_request(const char *message_id, int64_t now_utc, const char *ip,
                               char *out, size_t cap);
/* A public IPv4/IPv6 literal (trims one trailing newline). False for
 * private, loopback, link-local, CGNAT-shared, multicast, or junk. */
bool location_aws_public_ip(const char *text, char *out, size_t len);

typedef struct {
    bool accepted;
    double lat, lon, accuracy_m;   /* accepted */
    int error_code;                /* rejected */
    char message_id[48];
} location_aws_reply_t;
/* Parse an accepted/rejected payload. False if malformed. */
bool location_aws_parse_reply(bool rejected_topic, const char *payload, size_t len,
                              location_aws_reply_t *out);

#ifdef __cplusplus
}
#endif
