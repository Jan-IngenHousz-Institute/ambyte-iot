#define _POSIX_C_SOURCE 200809L
#include "svc_store.h"
#include "svc_wire.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef SVC_HOST
void svc_store_fault(const char *point);
#define CUT(point) svc_store_fault(point)
#else
#define CUT(point) ((void)0)
#endif

static const char *io_reason(void) { return errno == ENOSPC ? "no_space" : "storage_error"; }
static void put32(unsigned char *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (i * 8));
}
static uint32_t get32(const unsigned char *p)
{
    uint32_t n = 0;
    for (unsigned i = 0; i < 4; ++i) n |= (uint32_t)p[i] << (i * 8);
    return n;
}
static void put64(unsigned char *p, uint64_t v)
{
    put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32));
}
static uint64_t get64(const unsigned char *p)
{
    return get32(p) | ((uint64_t)get32(p + 4) << 32);
}

static bool path_for(const svc_store_t *s, const char *name, char *out, size_t capacity)
{
    int n = snprintf(out, capacity, "%s/%s", s->root, name);
    return n >= 0 && (size_t)n < capacity;
}

bool svc_store_data_path(const svc_store_t *s, char *out, size_t capacity)
{
    return path_for(s, "stage.bin", out, capacity);
}

static bool record_path(const svc_store_t *s, uint64_t id, char *out, size_t capacity)
{
    char name[21];
    snprintf(name, sizeof name, "%016llx.rec", (unsigned long long)id);
    return path_for(s, name, out, capacity);
}

static const char *atomic_file(const svc_store_t *s, const char *destination,
                                const unsigned char *data, size_t length)
{
    char temporary[192];
    if (!path_for(s, "write.tmp", temporary, sizeof temporary)) return "storage_error";
    CUT("before_open");
    FILE *f = fopen(temporary, "wb");
    if (!f) return io_reason();
    CUT("after_open");
    const char *error = NULL;
    if (fwrite(data, 1, length, f) != length) error = io_reason();
    CUT("after_write");
    if (fflush(f) != 0 && !error) error = io_reason();
    CUT("after_flush");
    if (fsync(fileno(f)) != 0 && !error) error = io_reason();
    CUT("after_fsync");
    if (fclose(f) != 0 && !error) error = io_reason();
    CUT("after_close");
    if (error) return error;
    if (rename(temporary, destination) != 0) return io_reason();
    CUT("after_rename");
    return NULL;
}

static const char *header_write(svc_store_t *s, uint64_t highest)
{
    unsigned char bytes[32] = {0};
    memcpy(bytes, "SVCS0001", 8);
    put64(bytes + 8, s->instance);
    put64(bytes + 16, highest);
    put32(bytes + 28, svc_crc32(0, bytes, 28));
    char path[192];
    if (!path_for(s, "store", path, sizeof path)) return "storage_error";
    const char *error = atomic_file(s, path, bytes, sizeof bytes);
    if (!error) s->highest = highest;
    return error;
}

bool svc_terminal(svc_state_t state) { return state >= SVC_SUCCEEDED && state <= SVC_UNCERTAIN; }

const char *svc_state_name(svc_state_t state)
{
    static const char *const names[] = {"invalid", "staging", "connecting", "committing", "persisting",
                                         "succeeded", "failed", "aborted", "uncertain"};
    return (unsigned)state < sizeof names / sizeof names[0] ? names[state] : "invalid";
}

static void encode_record(const svc_record_t *r, unsigned char out[SVC_DISK_RECORD])
{
    memset(out, 0, SVC_DISK_RECORD);
    memcpy(out, "SVCR0001", 8);
    put64(out + 8, r->id);
    put32(out + 16, r->size); put32(out + 20, r->offset);
    out[24] = (unsigned char)r->kind; out[25] = (unsigned char)r->state;
    out[26] = (unsigned char)r->saved; out[27] = (unsigned char)r->connected;
    out[28] = (unsigned char)r->applied; out[29] = r->retrying; out[30] = r->timed_out;
    out[31] = r->restored;
    size_t at = 32;
#define ENCODE(field) memcpy(out + at, r->field, sizeof r->field); at += sizeof r->field
    ENCODE(reason); ENCODE(sha256); ENCODE(workbook); ENCODE(version); ENCODE(built_fw);
    ENCODE(ssid); ENCODE(fingerprint); ENCODE(previous_fingerprint);
#undef ENCODE
    put32(out + SVC_DISK_RECORD - 4, svc_crc32(0, out, SVC_DISK_RECORD - 4));
}

static bool decode_record(const unsigned char in[SVC_DISK_RECORD], svc_record_t *r)
{
    if (memcmp(in, "SVCR0001", 8) || get32(in + SVC_DISK_RECORD - 4) !=
        svc_crc32(0, in, SVC_DISK_RECORD - 4)) return false;
    memset(r, 0, sizeof *r);
    r->id = get64(in + 8); r->size = get32(in + 16); r->offset = get32(in + 20);
    r->kind = in[24]; r->state = in[25]; r->saved = in[26]; r->connected = in[27];
    r->applied = in[28]; r->retrying = in[29]; r->timed_out = in[30];
    r->restored = in[31];
    size_t at = 32;
#define DECODE(field) memcpy(r->field, in + at, sizeof r->field); at += sizeof r->field
    DECODE(reason); DECODE(sha256); DECODE(workbook); DECODE(version); DECODE(built_fw);
    DECODE(ssid); DECODE(fingerprint); DECODE(previous_fingerprint);
#undef DECODE
    if (!r->id || r->kind < SVC_SCHEDULE || r->kind > SVC_WIFI ||
        r->state < SVC_STAGING || r->state > SVC_UNCERTAIN || r->offset > r->size ||
        r->size > SVC_DATA_MAX || r->saved > SVC_UNKNOWN || r->connected > SVC_UNKNOWN ||
        r->applied > SVC_UNKNOWN || in[29] > 1 || in[30] > 1 || in[31] > 1) return false;
#define TERMINATED(field) if (!memchr(r->field, 0, sizeof r->field)) return false
    TERMINATED(reason); TERMINATED(sha256); TERMINATED(workbook); TERMINATED(version);
    TERMINATED(built_fw); TERMINATED(ssid);
#undef TERMINATED
    return true;
}

const char *svc_store_get(svc_store_t *s, uint64_t id, svc_record_t *r)
{
    if (s->corrupt) return "corrupt_store";
    char path[192];
    if (!record_path(s, id, path, sizeof path)) return "storage_error";
    FILE *f = fopen(path, "rb");
    if (!f) return errno == ENOENT ? (id <= s->highest ? "expired" : "not_found") : io_reason();
    unsigned char bytes[SVC_DISK_RECORD];
    bool ok = fread(bytes, 1, sizeof bytes, f) == sizeof bytes && fgetc(f) == EOF && !ferror(f);
    if (fclose(f) != 0) ok = false;
    if (!ok || !decode_record(bytes, r) || r->id != id || id > s->highest) {
        s->corrupt = true;
        return "corrupt_store";
    }
    return NULL;
}

/* At most eight completed + one active + one just-completed GC predecessor.
 * Unknown files are evidence of an unsupported/damaged store, not permission
 * to unlink anything. write.tmp is always uncommitted workspace. */
static const char *list_records(svc_store_t *s, uint64_t ids[10], unsigned *count,
                                size_t *logical)
{
    DIR *directory = opendir(s->root);
    if (!directory) return io_reason();
    *count = 0; *logical = 0;
    const char *error = NULL;
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char path[192]; struct stat st;
        if (!path_for(s, entry->d_name, path, sizeof path) || stat(path, &st) != 0) {
            error = "storage_error"; break;
        }
        if (!S_ISREG(st.st_mode) || st.st_size < 0) { error = "corrupt_store"; break; }
        *logical += (size_t)st.st_size;
        if (!strcmp(entry->d_name, "store") || !strcmp(entry->d_name, "stage.bin") ||
            !strcmp(entry->d_name, "write.tmp")) continue;
        if (strlen(entry->d_name) != 20 || strcmp(entry->d_name + 16, ".rec") || *count == 10) {
            error = "corrupt_store"; break;
        }
        char id[17]; memcpy(id, entry->d_name, 16); id[16] = 0;
        if (!svc_hex(id, 16, &ids[*count]) || !ids[*count]) { error = "corrupt_store"; break; }
        ++*count;
    }
    if (closedir(directory) != 0 && !error) error = io_reason();
    if (*logical > 32768) error = "corrupt_store";
    if (error && !strcmp(error, "corrupt_store")) s->corrupt = true;
    return error;
}

const char *svc_store_account(svc_store_t *s, size_t *logical, unsigned *count)
{
    uint64_t ids[10];
    return list_records(s, ids, count, logical);
}

const char *svc_store_active(svc_store_t *s, svc_record_t *out)
{
    if (s->corrupt) return "corrupt_store";
    uint64_t ids[10]; unsigned count; size_t logical;
    const char *error = list_records(s, ids, &count, &logical);
    if (error) return error;
    bool found = false;
    for (unsigned i = 0; i < count; ++i) {
        svc_record_t r;
        if ((error = svc_store_get(s, ids[i], &r))) return error;
        if (svc_terminal(r.state)) continue;
        if (found) { s->corrupt = true; return "corrupt_store"; }
        *out = r; found = true;
    }
    return found ? NULL : "not_found";
}

static const char *gc(svc_store_t *s)
{
    uint64_t ids[10]; unsigned count; size_t logical;
    const char *error = list_records(s, ids, &count, &logical);
    if (error) return error;
    unsigned terminal = 0;
    uint64_t completed[10];
    for (unsigned i = 0; i < count; ++i) {
        svc_record_t r;
        if ((error = svc_store_get(s, ids[i], &r))) return error;
        if (svc_terminal(r.state)) {
            unsigned at = terminal++;
            while (at && completed[at - 1] > r.id) {
                completed[at] = completed[at - 1]; --at;
            }
            completed[at] = r.id;
        }
    }
    for (unsigned i = 0; i + SVC_RESULTS < terminal; ++i) {
        char path[192];
        record_path(s, completed[i], path, sizeof path);
        CUT("before_gc");
        if (unlink(path) != 0) return io_reason();
        CUT("after_gc");
    }
    return NULL;
}

const char *svc_store_save(svc_store_t *s, const svc_record_t *r)
{
    if (s->corrupt) return "corrupt_store";
    unsigned char bytes[SVC_DISK_RECORD]; char path[192];
    encode_record(r, bytes);
    record_path(s, r->id, path, sizeof path);
    const char *error = atomic_file(s, path, bytes, sizeof bytes);
    if (!error && svc_terminal(r->state)) error = gc(s);
    return error;
}

const char *svc_store_open(svc_store_t *s, const char *root, uint64_t fresh,
                           size_t (*free_bytes)(void *), void *context)
{
    memset(s, 0, sizeof *s);
    if (strlen(root) >= sizeof s->root || !fresh) return "invalid_args";
    strcpy(s->root, root); s->free_bytes = free_bytes; s->context = context;
    if (mkdir(root, 0700) != 0 && errno != EEXIST) return io_reason();
    char path[192]; path_for(s, "store", path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (errno != ENOENT) return io_reason();
        uint64_t ids[10]; unsigned count; size_t logical;
        const char *error = list_records(s, ids, &count, &logical);
        /* An interrupted first header can leave only its uncommitted temp. */
        if (error || count) { s->corrupt = true; return "corrupt_store"; }
        char stage[192]; svc_store_data_path(s, stage, sizeof stage);
        struct stat st;
        if (stat(stage, &st) == 0) { s->corrupt = true; return "corrupt_store"; }
        s->instance = fresh;
        if ((error = header_write(s, 0))) return error;
    } else {
        unsigned char bytes[32];
        bool ok = fread(bytes, 1, sizeof bytes, f) == sizeof bytes && fgetc(f) == EOF && !ferror(f);
        if (fclose(f) != 0) ok = false;
        if (!ok || memcmp(bytes, "SVCS0001", 8) || get32(bytes + 28) != svc_crc32(0, bytes, 28)) {
            s->corrupt = true; return "corrupt_store";
        }
        s->instance = get64(bytes + 8); s->highest = get64(bytes + 16);
        if (!s->instance) { s->corrupt = true; return "corrupt_store"; }
    }
    const char *error = gc(s);
    if (error) return error;
    svc_record_t active;
    error = svc_store_active(s, &active);
    if (error && strcmp(error, "not_found")) return error;
    if (!error && active.kind == SVC_SCHEDULE && active.state == SVC_STAGING) {
        char stage[192]; svc_store_data_path(s, stage, sizeof stage);
        struct stat st;
        if (stat(stage, &st) != 0 || st.st_size < active.offset) {
            s->corrupt = true; return "corrupt_store";
        }
        if ((uint64_t)st.st_size > active.offset) {
            f = fopen(stage, "r+b");
            if (!f) return io_reason();
            bool ok = ftruncate(fileno(f), active.offset) == 0 && fsync(fileno(f)) == 0;
            if (fclose(f) != 0) ok = false;
            if (!ok) return io_reason();
        }
    }
    return NULL;
}

const char *svc_store_admit(svc_store_t *s, svc_record_t *r, bool *reused)
{
    *reused = false;
    svc_record_t old;
    const char *error = svc_store_get(s, r->id, &old);
    if (!error) {
        if (old.kind != r->kind || memcmp(old.fingerprint, r->fingerprint, 32)) return "request_conflict";
        *r = old; *reused = true; return NULL;
    }
    if (s->highest == UINT64_MAX &&
        (!strcmp(error, "expired") || !strcmp(error, "not_found"))) return "id_exhausted";
    if (strcmp(error, "not_found")) return error;
    error = svc_store_active(s, &old);
    if (!error) return "busy";
    if (strcmp(error, "not_found")) return error;
    if (s->highest == UINT64_MAX) return "id_exhausted";
    if (!r->id || r->id <= s->highest) return "expired";
    if (r->size > SVC_DATA_MAX) return "size_limit";
    if (s->free_bytes && s->free_bytes(s->context) < 65536u + r->size + 2048u) return "no_space";
    if ((error = header_write(s, r->id))) return error;
    if (r->kind == SVC_SCHEDULE) {
        char path[192]; svc_store_data_path(s, path, sizeof path);
        CUT("before_data_open");
        FILE *f = fopen(path, "wb");
        if (!f) return io_reason();
        bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
        if (fclose(f) != 0) ok = false;
        CUT("after_data_open");
        if (!ok) return io_reason();
    }
    return svc_store_save(s, r);
}

const char *svc_store_put(svc_store_t *s, svc_record_t *r, uint32_t offset,
                          uint32_t crc, const unsigned char *bytes, size_t length)
{
    if (s->corrupt) return "corrupt_store";
    if (svc_terminal(r->state)) return "operation_complete";
    if (r->state == SVC_COMMITTING) return "commit_started";
    if (r->kind != SVC_SCHEDULE || r->state != SVC_STAGING) return "invalid_sequence";
    if (!length || length > SVC_CHUNK_MAX || offset > r->size || length > r->size - offset) return "size_limit";
    if (svc_crc32(0, bytes, length) != crc) return "chunk_crc";
    if (offset > r->offset) return "offset_gap";
    if (offset % SVC_CHUNK_MAX || (length != SVC_CHUNK_MAX && offset + length != r->size)) return "offset_conflict";
    char path[192]; svc_store_data_path(s, path, sizeof path);
    FILE *f = fopen(path, "r+b");
    if (!f) { s->corrupt = true; return "corrupt_store"; }
    const char *error = NULL;
    if (fseek(f, (long)offset, SEEK_SET) != 0) error = io_reason();
    if (offset < r->offset && !error) {
        unsigned char old[SVC_CHUNK_MAX];
        if (length > r->offset - offset || fread(old, 1, length, f) != length || memcmp(old, bytes, length))
            error = "offset_conflict";
        svc_wipe(old, sizeof old);
    } else if (!error) {
        CUT("before_chunk_write");
        if (fwrite(bytes, 1, length, f) != length) error = io_reason();
        CUT("after_chunk_write");
        if (fflush(f) != 0 && !error) error = io_reason();
        CUT("after_chunk_flush");
        if (fsync(fileno(f)) != 0 && !error) error = io_reason();
        CUT("after_chunk_fsync");
    }
    if (fclose(f) != 0 && !error) error = io_reason();
    CUT("after_chunk_close");
    if (error || offset < r->offset) return error;
    svc_record_t next = *r;
    next.offset += (uint32_t)length;
    error = svc_store_save(s, &next);
    if (!error) *r = next;
    return error;
}

const char *svc_store_abort(svc_store_t *s, svc_record_t *r)
{
    if (r->state == SVC_ABORTED) return NULL;
    if (r->state == SVC_COMMITTING || r->state == SVC_PERSISTING || r->state == SVC_CONNECTING) return "commit_started";
    if (svc_terminal(r->state)) return "operation_complete";
    svc_record_t next = *r;
    next.state = SVC_ABORTED;
    strcpy(next.reason, "aborted");
    const char *error = svc_store_save(s, &next);
    if (error) return error;
    *r = next;
    char path[192]; svc_store_data_path(s, path, sizeof path);
    CUT("before_abort_cleanup");
    if (unlink(path) != 0 && errno != ENOENT) return io_reason();
    CUT("after_abort_cleanup");
    return NULL;
}
