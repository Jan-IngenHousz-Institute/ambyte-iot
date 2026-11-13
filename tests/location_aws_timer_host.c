/* SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * The production AWS Device Location scheduler (location_aws.c, included
 * whole) with its RTOS/timer/MQTT boundaries replaced. Review of #100
 * (gpt-6-astra + Opus): a timer firing while unauthorized returned without
 * re-arming, and later grants never re-armed, so live location died for the
 * rest of the boot. Exit code 0 = all scenarios behave. */
#include <stdio.h>
#include <string.h>
#include "../components/command_router/location_aws.c"

static int armed, arms;
static int64_t age;
static uint32_t session = 1;
static message_suback_fn acks[2];
static void *contexts[2];
static int subs;

esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *h) { (void)a; *h = (void *)1; return ESP_OK; }
esp_err_t esp_timer_stop(esp_timer_handle_t h) { (void)h; armed = 0; return ESP_OK; }
esp_err_t esp_timer_start_once(esp_timer_handle_t h, uint64_t us) { (void)h; (void)us; armed = 1; arms++; return ESP_OK; }
bool esp_timer_is_active(esp_timer_handle_t h) { (void)h; return armed; }
esp_err_t device_config_get_live_location_mode(char *out, size_t n) { snprintf(out, n, "aws"); return ESP_OK; }
void device_location_set_live_status(const char *s) { (void)s; }
bool timezone_command_offer_live(double a, double b, double c, int64_t t, device_live_source_t s)
{ (void)a; (void)b; (void)c; (void)t; (void)s; return true; }
bool location_aws_loc_id(const char *n, char *o, size_t z) { (void)n; snprintf(o, z, "unit"); return true; }
int location_aws_select_aps(const location_aws_ap_t *a, int n, location_aws_ap_t *b, int m) { (void)a; (void)n; (void)b; (void)m; return 0; }
size_t location_aws_wifi_request(const char *a, int64_t b, const location_aws_ap_t *c, int d, char *e, size_t f) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; return 0; }
size_t location_aws_ip_request(const char *a, int64_t b, const char *c, char *d, size_t e) { (void)a; (void)b; (void)c; (void)d; (void)e; return 0; }
bool location_aws_public_ip(const char *a, char *b, size_t c) { (void)a; (void)b; (void)c; return false; }
bool location_aws_parse_reply(bool a, const char *b, size_t c, location_aws_reply_t *d) { (void)a; (void)b; (void)c; (void)d; return false; }
static esp_err_t pub(const char *a, const char *b, size_t c, int *d) { (void)a; (void)b; (void)c; (void)d; return ESP_OK; }
static esp_err_t sub(const char *f, message_received_fn r, message_suback_fn a, void *c) { (void)f; (void)r; acks[subs] = a; contexts[subs++] = c; return ESP_OK; }
static void stats(uint32_t *c, int64_t *a, char *r, size_t n) { (void)r; (void)n; *c = session; *a = age; }
static bool submit(void (*run)(void *), void *a) { run(a); return true; }
static void grant(void) { acks[0](true, session, contexts[0]); acks[1](true, session, contexts[1]); }
/* A one-shot esp_timer is inactive once its callback runs. */
static void fire(void) { armed = 0; timer_cb(NULL); }

#define EXPECT(c) do { if (!(c)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main(void)
{
    location_aws_config_t cfg = {.thing_name = "unit", .publish = pub, .add_subscription = sub,
                                 .connection_stats = stats, .submit = submit};
    EXPECT(location_aws_init(&cfg) == ESP_OK && subs == 2);
    grant();
    EXPECT(armed && arms == 1);                 /* first grant: one jittered attempt */

    age = -1;                                   /* the link drops ...           */
    fire();                                     /* ... and the attempt fires    */
    EXPECT(!armed);                             /* unauthorized: nothing sent, nothing armed */

    age = 0;
    session = 2;
    grant();                                    /* reconnect, both granted */
    EXPECT(armed && arms == 2);                 /* re-armed: live location continues */

    fire();                                     /* attempt runs (no IP here): backs off, arms */
    EXPECT(armed && arms == 3);
    session = 3;
    grant();                                    /* churn while a timer is pending */
    EXPECT(arms == 3);                          /* no extra request from reconnects */
    puts("LOCATION_AWS_TIMER_OK");
    return 0;
}
