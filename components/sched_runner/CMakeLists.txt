# sched_runner — declarative schedule runner: task, lifecycle, action run
# functions, embedded default. Firmware-only (FreeRTOS + device_commands);
# the pure due/cron/window math lives in sched_spec and is host-tested there.

# ── Bundle default.yaml as a C byte array ────────────────────────────────────
# Generate the schedule header only in a real component build: EMBED_TXTFILES mis-resolves
# the generated .S path under PlatformIO, so the array is generated at
# configure time into the BUILD dir (never the source tree). Reconfigures
# automatically when default.yaml changes.
# NOTE: ${CMAKE_CURRENT_SOURCE_DIR} and ${CMAKE_CURRENT_BINARY_DIR} both point
# at the project root during ESP-IDF's early requirements scan, which is how
# generated headers can otherwise leak into the source tree. Address the input from the
# project root, but write the header under ${CMAKE_BINARY_DIR}: during the
# scan that is the scan's own throwaway dir, during the real build it is the
# real build dir — either way never the source tree.
set(SCHED_DEFAULT_SRC "${CMAKE_SOURCE_DIR}/components/sched_runner/default.yaml")
set(SCHED_EMBED_DIR "${CMAKE_BINARY_DIR}/sched_runner_embed")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SCHED_DEFAULT_SRC}")
file(READ "${SCHED_DEFAULT_SRC}" SCHED_DEFAULT_HEX HEX)
string(LENGTH "${SCHED_DEFAULT_HEX}" SCHED_DEFAULT_HEXLEN)
math(EXPR SCHED_DEFAULT_LEN "${SCHED_DEFAULT_HEXLEN} / 2")
string(REGEX REPLACE "(..)" "0x\\1," SCHED_DEFAULT_ARR "${SCHED_DEFAULT_HEX}")
file(MAKE_DIRECTORY "${SCHED_EMBED_DIR}")
file(WRITE "${SCHED_EMBED_DIR}/default_yaml_embed.h"
"/* Generated from default.yaml at configure time — do not edit. */\n"
"static const char default_yaml_start[] = { ${SCHED_DEFAULT_ARR} };\n"
"static const unsigned default_yaml_size = ${SCHED_DEFAULT_LEN};\n")

idf_component_register(
    SRCS "sched_runner.c" "sched_runner_actions.c" "sched_runner_json.c"
    INCLUDE_DIRS "include"
    REQUIRES sched_spec ambit_trace device_commands event_log time_sync timezone device_config
    PRIV_REQUIRES esp_timer mbedtls
)
target_include_directories(${COMPONENT_LIB} PRIVATE "${SCHED_EMBED_DIR}")
