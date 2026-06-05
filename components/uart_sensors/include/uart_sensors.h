#ifndef AMBYTE_UART_SENSORS_H
#define AMBYTE_UART_SENSORS_H

#include "esp_err.h"
#include "uart_sensor_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialise all UART controllers and pin mappings for the 4 sensor channels.
 *
 * Channel layout (Option D — GPIO-remap for channels 2 & 3):
 *   CH0 (AMBIT1): UART1  — dedicated
 *   CH1 (AMBIT2): UART2  — dedicated
 *   CH2 (AMBIT3): UART0  — shared, GPIO-remapped per query
 *   CH3 (AMBIT4): UART0  — shared, GPIO-remapped per query
 *
 * Must be called once from app_main before any query/ping.
 */
esp_err_t uart_sensors_init(void);

/* Port-adapter getters — return function pointers wired into device_commands */
uart_sensor_query_fn       uart_sensors_get_query_fn(void);
uart_sensor_ping_fn        uart_sensors_get_ping_fn(void);
uart_sensor_status_fn      uart_sensors_get_status_fn(void);
uart_sensor_text_query_fn  uart_sensors_get_text_query_fn(void);
uart_sensor_stream_query_fn uart_sensors_get_stream_query_fn(void);

#ifdef __cplusplus
}
#endif

#endif /* AMBYTE_UART_SENSORS_H */
