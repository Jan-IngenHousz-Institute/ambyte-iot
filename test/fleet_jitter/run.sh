#!/bin/sh
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$test_dir/../.." && pwd)
test_build_dir=$(mktemp -d)
trap 'rm -rf "$test_build_dir"' EXIT HUP INT TERM

"${FLEET_JITTER_TEST_CC:-cc}" \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -pedantic \
    -I"$test_dir/stubs" \
    -I"$repo_dir/components/fleet_jitter/include" \
    "$repo_dir/components/fleet_jitter/fleet_jitter.c" \
    "$test_dir/test_fleet_jitter.c" \
    -o "$test_build_dir/test_fleet_jitter"

"$test_build_dir/test_fleet_jitter"
