#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SVC_DATA_MAX 16384u
#define SVC_RESULTS 8u
#define SVC_DISK_RECORD 512u

typedef enum { SVC_SCHEDULE = 1, SVC_WIFI } svc_kind_t;
typedef enum {
    SVC_STAGING = 1, SVC_CONNECTING, SVC_COMMITTING, SVC_PERSISTING,
    SVC_SUCCEEDED, SVC_FAILED, SVC_ABORTED, SVC_UNCERTAIN
} svc_state_t;
typedef enum { SVC_FALSE, SVC_TRUE, SVC_UNKNOWN } svc_truth_t;

typedef struct {
    uint64_t id;
    uint32_t size, offset;
    svc_kind_t kind;
    svc_state_t state;
    svc_truth_t saved, connected, applied;
    bool retrying, timed_out, restored, restoration_connected;
    char reason[32];
    char sha256[65], workbook[37], version[32], built_fw[32], ssid[33];
    unsigned char fingerprint[32], previous_fingerprint[32];
} svc_record_t;

typedef struct {
    char root[160];
    uint64_t instance, highest;
    bool corrupt;
    size_t (*free_bytes)(void *context);
    void *context;
} svc_store_t;

const char *svc_store_open(svc_store_t *store, const char *root, uint64_t new_instance,
                           size_t (*free_bytes)(void *), void *context);
const char *svc_store_get(svc_store_t *store, uint64_t id, svc_record_t *record);
const char *svc_store_active(svc_store_t *store, svc_record_t *record);
const char *svc_store_admit(svc_store_t *store, svc_record_t *record, bool *reused);
const char *svc_store_save(svc_store_t *store, const svc_record_t *record);
const char *svc_store_put(svc_store_t *store, svc_record_t *record, uint32_t offset,
                          uint32_t crc, const unsigned char *bytes, size_t length);
const char *svc_store_abort(svc_store_t *store, svc_record_t *record);
const char *svc_store_account(svc_store_t *store, size_t *logical, unsigned *records);
bool svc_store_data_path(const svc_store_t *store, char *path, size_t capacity);
bool svc_terminal(svc_state_t state);
const char *svc_state_name(svc_state_t state);
