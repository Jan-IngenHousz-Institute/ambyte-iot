#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Include the whole production translation unit to exercise its actual
 * private lifecycle/program state. Link GC discards unrelated hardware paths. */
#include "../../components/sched_runner/sched_runner.c"

static unsigned enters, exits;
static bool checking_snapshot;
static int depth;
void identity_host_enter(portMUX_TYPE *mux)
{
    assert(mux == &s_state_mux && depth++ == 0);
    enters++;
}
void identity_host_exit(portMUX_TYPE *mux)
{
    assert(mux == &s_state_mux && --depth == 0);
    exits++;
}

const char *__real_sched_pool_str(const sched_program_t *, uint16_t);
const char *__wrap_sched_pool_str(const sched_program_t *p, uint16_t off)
{
    if (checking_snapshot) assert(depth == 1);
    return __real_sched_pool_str(p, off);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    sched_identity_t out;
    assert(sched_runner_identity(NULL) == ESP_ERR_INVALID_ARG);
    assert(sched_runner_identity(&out) == ESP_OK);
    assert(out.kind == SCHED_SOURCE_NONE && out.generation == 0);
    assert(!out.has_workbook && !out.sha256[0]);

    /* The real start() has exactly this window: old source remains while
     * the old program is invalid and the new program is compiling. */
    s_source.kind = SCHED_SOURCE_INSTALLED;
    memset(s_source.sha256, 'a', 64);
    assert(sched_lifecycle_begin_start(&s_lifecycle, NULL));
    s_prog_valid = false;
    memset(&out, 0x55, sizeof out);
    assert(sched_runner_identity(&out) == ESP_ERR_INVALID_STATE);
    assert(!out.sha256[0] && !out.workbook[0]);

    FILE *fixture = fopen(argv[1], "rb");
    assert(fixture != NULL);
    char yaml[SCHED_YAML_MAX_FILE_BYTES + 1];
    size_t len = fread(yaml, 1, sizeof yaml, fixture);
    assert(feof(fixture) && !ferror(fixture) && len < sizeof yaml);
    fclose(fixture);
    char err[160];
    esp_err_t compiled = sched_compile_text(yaml, len, &s_prog, err, sizeof err);
    if (compiled != ESP_OK) fprintf(stderr, "fixture compile failed: %s\n", err);
    assert(compiled == ESP_OK);
    s_prog_valid = true;
    assert(sched_lifecycle_publish_start(&s_lifecycle, s_lifecycle.generation, NULL));
    unsigned before = enters;
    checking_snapshot = true;
    assert(sched_runner_identity(&out) == ESP_OK);
    checking_snapshot = false;
    assert(enters == before + 1 && exits == enters && depth == 0);
    assert(out.kind == SCHED_SOURCE_INSTALLED && out.generation == 1);
    assert(!strcmp(out.sha256, s_source.sha256));
    assert(out.has_workbook && !strcmp(out.workbook,
           "11111111-2222-4333-8444-555555555555"));

    sched_identity_t running = out;
    assert(sched_lifecycle_request_stop(&s_lifecycle) == 1);
    assert(sched_lifecycle_complete(&s_lifecycle, 1));
    assert(sched_runner_identity(&out) == ESP_OK);
    assert(memcmp(&out, &running, sizeof out) == 0);

    assert(sched_lifecycle_begin_start(&s_lifecycle, NULL));
    s_prog_valid = false;
    assert(sched_runner_identity(&out) == ESP_ERR_INVALID_STATE);
    assert(!out.sha256[0]);
    s_source.kind = SCHED_SOURCE_EMBEDDED_FALLBACK;
    memset(s_source.sha256, 'b', 64);
    s_prog_valid = true;
    assert(sched_runner_identity(&out) == ESP_OK);
    assert(out.generation == 2 && out.kind == SCHED_SOURCE_EMBEDDED_FALLBACK);
    assert(out.sha256[0] == 'b');
    puts("IDENTITY_RUNNER_OK");
    return 0;
}
