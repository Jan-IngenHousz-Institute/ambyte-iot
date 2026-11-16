#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef int i2c_port_t;
#define I2C_NUM_MAX 2
esp_err_t i2c_master_write_read_device(int port, unsigned addr, const uint8_t *w, size_t wn, uint8_t *r, size_t rn, unsigned timeout);
esp_err_t i2c_master_write_to_device(int port, unsigned addr, const uint8_t *w, size_t wn, unsigned timeout);
