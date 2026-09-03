# sched_spec — pure C11 schedule spec compiler (YAML subset parser, compiler,
# cron/window/due math). No ESP-IDF dependency beyond esp_err_t; the only real
# dependency is time_sync, itself pure C. Host-tested via tests/test_sched_spec.py.
idf_component_register(
    SRCS "sched_yaml.c" "sched_compile.c" "sched_actions_table.c"
         "sched_cron.c" "sched_window.c" "sched_due.c"
    INCLUDE_DIRS "include"
    REQUIRES time_sync esp_common
)
