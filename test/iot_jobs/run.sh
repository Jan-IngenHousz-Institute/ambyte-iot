#!/bin/sh
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$test_dir/../.." && pwd)
test_build_dir=$(mktemp -d)
trap 'rm -rf "$test_build_dir"' EXIT HUP INT TERM

"${IOT_JOBS_TEST_CC:-cc}" \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -pedantic \
    -I"$repo_dir/components/iot_jobs/include" \
    "$repo_dir/components/iot_jobs/iot_jobs_plan.c" \
    "$test_dir/test_iot_jobs_plan.c" \
    -o "$test_build_dir/test_iot_jobs_plan"

"$test_build_dir/test_iot_jobs_plan"
