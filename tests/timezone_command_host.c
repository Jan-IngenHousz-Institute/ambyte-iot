/* Production MQTT timezone worker, actual cJSON + IANA engine; only SDK/NVS
 * and resident-worker scheduling boundaries are replaced. */
#define _XOPEN_SOURCE 700 /* putenv, for the libc boundary below */
#include "timezone_command.h"
#include "timezone.h"
#include "device_config.h"
#include "time_sync.h"
#include "tz_geo.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static char stored[48] = "Europe/Berlin";
static char last[2048];
static int writes, reads, fail_write, fail_readback, acquired, deny, queued;
static void (*job)(void *);
static void *argument;
/* libc boundary: this file and timezone.c are compiled with -Dsetenv=host_setenv
 * so a failing (ENOMEM) or misbehaving TZ installation can be injected into the
 * production apply path. 0 = real, 1 = setenv fails, 2 = libc ends up on a
 * different rule for the next call only. */
static int setenv_mode;
int host_setenv(const char *name, const char *value, int overwrite)
{
    static char env[2][160];
    static int slot;
    (void)overwrite;
    if (setenv_mode == 1) return -1;
    if (setenv_mode == 2) { setenv_mode = 0; value = "UTC0"; }
    slot ^= 1;
    snprintf(env[slot], sizeof env[slot], "%s=%s", name, value);
    return putenv(env[slot]);
}
esp_err_t device_config_get_timezone(char *out, size_t size)
{
    reads++;
    if (fail_readback && writes) return ESP_FAIL;
    if (!stored[0]) return ESP_ERR_NVS_NOT_FOUND;
    if (!strcmp(stored,"Mars/Olympus")) return ESP_ERR_INVALID_ARG;
    assert(size > strlen(stored)); strcpy(out, stored); return ESP_OK;
}
esp_err_t device_config_set_timezone(const char *value)
{
    writes++; if (fail_write) return ESP_FAIL;
    strcpy(stored, value); return ESP_OK;
}
/* Coordinate resolver boundaries: no persisted location in these #98 cases,
 * so the source flag is the only new state (tests/timezone_auto_host.c covers
 * the resolver policy against the real device_config and map). */
static char source[16] = "";
esp_err_t device_config_set_timezone_source(const char *value) { strcpy(source, value); return ESP_OK; }
device_tz_source_t device_config_timezone_source(void)
{
    if (!source[0]) return DEVICE_TZ_SOURCE_PROVISIONED;
    return strcmp(source, "manual") ? DEVICE_TZ_SOURCE_AUTO : DEVICE_TZ_SOURCE_MANUAL;
}
const char *device_config_timezone_source_label(void) { return source[0] ? source : "provisioned"; }
esp_err_t device_config_get_location(double *lat, double *lon, char *d, size_t n)
{ (void)lat; (void)lon; (void)d; (void)n; return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t device_config_get_live_location_mode(char *buf, size_t len)
{ (void)buf; (void)len; return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t device_config_get_live_fix(device_live_fix_t *f) { (void)f; return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t device_config_set_live_fix(const device_live_fix_t *f) { (void)f; return ESP_FAIL; }
bool tz_geo_location_usable(double lat, double lon) { (void)lat; (void)lon; return false; }
const char *tz_geo_zone_for(double lat, double lon) { (void)lat; (void)lon; return NULL; }
static void reply(const char *json) { assert(strlen(json) < sizeof last); strcpy(last, json); }
static bool submit(void (*run)(void *), void *arg) { queued++; job=run; argument=arg; return true; }
static bool begin(void) { if (deny) return false; acquired++; return true; }
static void end(void) { acquired--; }
static void request(const char *json)
{
    cJSON *root=cJSON_Parse(json); assert(root);
    assert(timezone_command_handle(root)); cJSON_Delete(root);
}
static void execute(void) { assert(job); void (*run)(void *)=job; job=NULL; run(argument); assert(!acquired); }
static void *offset_reader(void *arg)
{
    (void)arg;
    for (int i=0; i<10000; i++) {
        int32_t off;
        const char *name;
        timezone_snapshot(1768478400, &name, &off);
        assert((!strcmp(name,"Europe/Berlin") && off==3600) ||
               (!strcmp(name,"Australia/Darwin") && off==34200));
    }
    return NULL;
}
int main(int argc, char **argv)
{
    assert(argc==2);
    timezone_apply("Europe/Berlin");
    timezone_command_config_t cfg={.reply=reply,.submit=submit,.maintenance_begin=begin,.maintenance_end=end};
    timezone_command_init(&cfg);
    const char *set="{\"type\":\"set_timezone\",\"id\":\"test-\\\"quoted\\\"\",\"timezone\":\"Australia/Darwin\"}";
    if (!strcmp(argv[1],"invalid")) {
        const char *values[]={"null","123","\"Mars/Olympus\"","\"\"","[]"};
        for (size_t i=0;i<sizeof values/sizeof *values;i++) {
            char json[256]; snprintf(json,sizeof json,"{\"type\":\"set_timezone\",\"id\":\"bad\",\"timezone\":%s}",values[i]);
            request(json); assert(!queued && !writes); assert(strstr(last,"invalid_timezone"));
        }
    } else if (!strcmp(argv[1],"busy")) {
        request(set); request(set); assert(strstr(last,"busy") && queued==1 && !writes);
        deny=1; execute(); assert(!writes && strstr(last,"busy"));
    } else if (!strcmp(argv[1],"persist_fail")) {
        fail_write=1; request(set); assert(!writes); execute();
        assert(strstr(last,"persist_failed") && !strcmp(timezone_current_name(),"Europe/Berlin"));
    } else if (!strcmp(argv[1],"readback_fail")) {
        fail_readback=1; request(set); execute();
        assert(strstr(last,"readback_failed") && !strcmp(timezone_current_name(),"Europe/Berlin"));
    } else if (!strcmp(argv[1],"repair")) {
        strcpy(stored,"Mars/Olympus"); request(set); execute();
        assert(writes==1 && strstr(last,"\"applied\":true"));
    } else if (!strcmp(argv[1],"unset")) {
        stored[0]='\0'; timezone_apply(NULL);
        request("{\"type\":\"get_timezone\",\"id\":\"before\"}"); execute();
        assert(!writes && strstr(last,"\"applied\":false"));
        request(set); execute(); assert(writes==1 && strstr(last,"\"applied\":true"));
    } else if (!strcmp(argv[1],"setenv_fail")) {
        /* setenv ENOMEM leaves libc on Berlin. Never report Darwin applied at
         * +3600, and the retry must re-apply rather than skip on the name. */
        setenv_mode=1; request(set); execute();
        assert(strstr(last,"\"ok\":false") && strstr(last,"\"applied\":false"));
        assert(strstr(last,"\"active_timezone\":\"Europe/Berlin\"") && strstr(last,"apply_failed"));
        assert(strstr(last,"\"utc_offset_seconds\":3600") && writes==1);
        setenv_mode=0; request(set); execute();
        assert(writes==1 && strstr(last,"\"applied\":true") && strstr(last,"\"utc_offset_seconds\":34200"));
    } else if (!strcmp(argv[1],"libc_reject")) {
        /* libc runs some other rule after setenv: restore the verified Berlin
         * rule, report not applied, and re-apply on retry. */
        setenv_mode=2; request(set); execute();
        assert(strstr(last,"\"applied\":false") && strstr(last,"\"active_timezone\":\"Europe/Berlin\""));
        assert(timezone_utc_offset_seconds(1768478400)==3600 && timezone_utc_offset_seconds(1784116800)==7200);
        request(set); execute();
        assert(writes==1 && strstr(last,"\"applied\":true") && strstr(last,"\"utc_offset_seconds\":34200"));
    } else if (!strcmp(argv[1],"position")) {
        timezone_apply("Australia/Darwin");
        assert(time_sync_get_utc_offset_seconds()==34200);
        time_sync_set_position(-25.3444,131.0369);
        double lat,lon; time_sync_get_location(&lat,&lon,NULL);
        assert(lat==-25.3444 && lon==131.0369);
        assert(time_sync_get_utc_offset_seconds()==34200);
        puts("position preserved half-hour offset: OK"); return 0;
    } else if (!strcmp(argv[1],"threads")) {
        pthread_t threads[4]; for(int i=0;i<4;i++) assert(!pthread_create(&threads[i],NULL,offset_reader,NULL));
        for(int i=0;i<1000;i++) timezone_apply(i%2 ? "Australia/Darwin" : "Europe/Berlin");
        for(int i=0;i<4;i++) assert(!pthread_join(threads[i],NULL));
        puts("threaded timezone readers: OK"); return 0;
    } else {
        assert(!strcmp(argv[1],"apply"));
        request(set); assert(!writes && !strcmp(timezone_current_name(),"Europe/Berlin"));
        execute(); assert(writes==1 && !strcmp(stored,"Australia/Darwin") && !source[0]);
        /* No location: the configured (fallback) zone is the zone in effect. */
        assert(strstr(last,"\"timezone_source\":\"configured\"") && strstr(last,"\"location_set\":false"));
        assert(strstr(last,"\"pinned\":false") && strstr(last,"\"requested_timezone\":\"Australia/Darwin\""));
        assert(timezone_utc_offset_seconds(1768478400)==34200);
        assert(timezone_utc_offset_seconds(1784116800)==34200);
        assert(strstr(last,"\"applied\":true"));
        request(set); execute(); assert(writes==1);
        int previous=reads;
        request("{\"type\":\"get_timezone\",\"id\":\"read\"}"); execute();
        assert(writes==1 && reads>previous && strstr(last,"read_only"));
    }
    puts(last); return 0;
}
