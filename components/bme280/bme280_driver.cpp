#include "bme280.h"

#include "bme280_adafruit_private.hpp"
#include "i2c_bus.h"

static Adafruit_BME280 s_bme280_device;
static bool s_bme280_ready = false;

extern "C" esp_err_t bme280_init(uint8_t i2c_addr)
{
  if ((i2c_addr != BME280_I2C_ADDR_PRIMARY) &&
      (i2c_addr != BME280_I2C_ADDR_SECONDARY)) {
    s_bme280_ready = false;
    return ESP_ERR_INVALID_ARG;
  }

  i2c_port_t bus_port = I2C_NUM_MAX;
  const esp_err_t err = i2c_bus_get_port(&bus_port);
  if (err != ESP_OK) {
    s_bme280_ready = false;
    return err;
  }

  if (!s_bme280_device.begin(bus_port, i2c_addr)) {
    s_bme280_ready = false;
    return ESP_FAIL;
  }

  // Forced (one-shot) mode: the part sleeps between reads instead of converting
  // continuously. begin() leaves it in MODE_NORMAL @ X16 (draws ~hundreds of µA
  // 24/7); switch to MODE_FORCED, keeping X16 oversampling so reported precision
  // is unchanged. bme280_read() triggers + waits for each sample.
  s_bme280_device.setSampling(Adafruit_BME280::MODE_FORCED,
                              Adafruit_BME280::SAMPLING_X16,
                              Adafruit_BME280::SAMPLING_X16,
                              Adafruit_BME280::SAMPLING_X16,
                              Adafruit_BME280::FILTER_OFF);

  s_bme280_ready = true;
  return ESP_OK;
}

extern "C" bool bme280_is_ready(void)
{
  return s_bme280_ready;
}

extern "C" esp_err_t bme280_read(bme280_reading_t *out_reading)
{
  if (!s_bme280_ready) {
    return ESP_ERR_INVALID_STATE;
  }
  if (out_reading == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }

  // Forced mode: trigger a one-shot conversion and wait for completion, else the
  // read*() calls return the previous (stale) sample. Returns false on a 2 s
  // timeout (sensor wedged) — surface as a read failure rather than stale data.
  if (!s_bme280_device.takeForcedMeasurement()) {
    return ESP_FAIL;
  }

  out_reading->temperature_c = s_bme280_device.readTemperature();
  out_reading->humidity_percent = s_bme280_device.readHumidity();
  out_reading->pressure_pa = s_bme280_device.readPressure();

  if (std::isnan(out_reading->temperature_c) || std::isnan(out_reading->humidity_percent) ||
      std::isnan(out_reading->pressure_pa)) {
    return ESP_FAIL;
  }

  return ESP_OK;
}

static esp_err_t bme280_sensor_read_adapter(measurement_t *out)
{
  bme280_reading_t reading;
  esp_err_t err = bme280_read(&reading);
  if (err != ESP_OK) {
    return err;
  }
  out->temperature_c = reading.temperature_c;
  out->humidity_percent = reading.humidity_percent;
  out->pressure_pa = reading.pressure_pa;
  return ESP_OK;
}

extern "C" sensor_read_fn bme280_get_sensor_read_fn(void)
{
  return bme280_sensor_read_adapter;
}
