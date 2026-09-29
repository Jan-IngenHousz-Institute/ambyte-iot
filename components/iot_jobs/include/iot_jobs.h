#pragma once

#include "esp_err.h"
#include "messaging_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * AWS IoT Jobs client: the device end of openJII's firmware rollout.
 *
 * openJII rolls a firmware release out as an IoT Job (its reviewer-gated
 * firmware-rollout workflow); AWS queues one execution per targeted Thing and
 * hands it to the device on the reserved `$aws/things/<thing>/jobs/...` topics.
 * This module listens there, decides with iot_jobs_plan(), drives ota_update's
 * job path, and reports IN_PROGRESS / SUCCEEDED / FAILED / REJECTED back.
 *
 * Identity: the openJII Jobs policy resolves ${iot:Connection.Thing.ThingName},
 * which AWS only sets when the certificate is attached to a Thing AND the MQTT
 * client id equals that Thing's name. Platform-provisioned units (flash GUI)
 * connect exactly so, so the thing name here IS the MQTT client id.
 *
 * Not every certificate carries the Jobs policy: AWS does not attach new
 * policies to existing certificates, so prod certs issued before Jobs support
 * (2026-08-28) lack it, as do units on the legacy shared certificate. A publish
 * outside the policy is refused (MQTT 5 PUBACK 0x87; under 3.1.1 it dropped the
 * connection, the 2026-06 status-topic incident) and would land in the
 * publisher's refusal telemetry. So the module publishes nothing until the
 * broker has granted both job subscriptions on the current connection: an
 * unauthorized SUBSCRIBE is refused in the SUBACK, which makes it a
 * side-effect-free probe. A refused unit logs once and stays silent.
 *
 * Lifecycle of one update:
 *   notify-next / $next/get -> plan INSTALL -> ota worker takes the maintenance
 *   lock -> IN_PROGRESS -> download, version + sha256 check -> reboot ->
 *   new image confirmed (MQTT + persistence) -> iot_jobs_kick -> $next/get ->
 *   version matches -> SUCCEEDED.
 * A rolled-back image boots the old version with this job's id still latched
 * -> FAILED, so AWS's abort criteria see it.
 */

typedef struct {
    const char                  *thing_name;       /* == MQTT client id */
    const char                  *family;           /* "ambyte" */
    const char                  *running_version;  /* compiled app version */
    message_publish_fn           publish;
    message_add_subscription_fn  add_subscription;
    message_connection_stats_fn  connection_stats; /* identifies the live session */
} iot_jobs_config_t;

/* Register the job subscriptions. Call before the MQTT client starts. */
esp_err_t iot_jobs_init(const iot_jobs_config_t *cfg);

/* Ask AWS for the next pending execution, if this connection is authorized.
 * Safe from any task and before init (no-op). Wired as ota_update's
 * `confirmed` hook. */
void iot_jobs_kick(void);

#ifdef __cplusplus
}
#endif
