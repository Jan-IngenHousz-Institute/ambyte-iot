#pragma once
#include "driver/i2c.h"
esp_err_t i2c_bus_get_port(i2c_port_t *port);
esp_err_t i2c_bus_lock(unsigned timeout);
esp_err_t i2c_bus_unlock(void);
esp_err_t i2c_bus_probe_addr(i2c_port_t port, unsigned addr);
