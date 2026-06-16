#include "i2c_bus.h"

#include <stdbool.h>

#include "esp_log.h"
#include "freertos/semphr.h"

static const char *TAG = "I2C_BUS";

static StaticSemaphore_t s_i2c_bus_mutex_storage;
static SemaphoreHandle_t s_i2c_bus_mutex = NULL;
static i2c_bus_config_t s_i2c_bus_config = {0};
static bool s_i2c_bus_initialized = false;

static SemaphoreHandle_t i2c_bus_ensure_mutex(void)
{
    if (s_i2c_bus_mutex == NULL) {
        s_i2c_bus_mutex = xSemaphoreCreateMutexStatic(&s_i2c_bus_mutex_storage);
    }

    return s_i2c_bus_mutex;
}

static bool i2c_bus_config_matches(const i2c_bus_config_t *lhs, const i2c_bus_config_t *rhs)
{
    return (lhs->port == rhs->port) && (lhs->sda_gpio == rhs->sda_gpio) &&
           (lhs->scl_gpio == rhs->scl_gpio) && (lhs->clock_speed_hz == rhs->clock_speed_hz);
}

esp_err_t i2c_bus_init(const i2c_bus_config_t *config)
{
    if ((config == NULL) || (config->clock_speed_hz == 0) ||
        (config->sda_gpio == GPIO_NUM_NC) || (config->scl_gpio == GPIO_NUM_NC)) {
        return ESP_ERR_INVALID_ARG;
    }

    SemaphoreHandle_t mutex = i2c_bus_ensure_mutex();
    if (mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_i2c_bus_initialized) {
        const esp_err_t status = i2c_bus_config_matches(config, &s_i2c_bus_config)
                                     ? ESP_OK
                                     : ESP_ERR_INVALID_STATE;
        xSemaphoreGive(mutex);
        return status;
    }

    const i2c_config_t idf_config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = config->sda_gpio,
        .scl_io_num = config->scl_gpio,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = config->clock_speed_hz,
        .clk_flags = I2C_SCLK_SRC_FLAG_AWARE_DFS,  /* DFS-aware source so SCL timing
                                                    * survives APB scaling (Phase 2) */
    };

    esp_err_t err = i2c_param_config(config->port, &idf_config);
    if (err == ESP_OK) {
        err = i2c_driver_install(config->port, I2C_MODE_MASTER, 0, 0, 0);
    }

    if (err == ESP_OK) {
        s_i2c_bus_config = *config;
        s_i2c_bus_initialized = true;
    }

    xSemaphoreGive(mutex);
    return err;
}

esp_err_t i2c_bus_lock(TickType_t timeout_ticks)
{
    if (!s_i2c_bus_initialized || (s_i2c_bus_mutex == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

    return (xSemaphoreTake(s_i2c_bus_mutex, timeout_ticks) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t i2c_bus_unlock(void)
{
    if (!s_i2c_bus_initialized || (s_i2c_bus_mutex == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

    return (xSemaphoreGive(s_i2c_bus_mutex) == pdTRUE) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t i2c_bus_get_config(i2c_bus_config_t *out_config)
{
    if (out_config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_i2c_bus_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    *out_config = s_i2c_bus_config;
    return ESP_OK;
}

esp_err_t i2c_bus_get_port(i2c_port_t *out_port)
{
    if (out_port == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_i2c_bus_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    *out_port = s_i2c_bus_config.port;
    return ESP_OK;
}

esp_err_t i2c_bus_check_and_recover(i2c_port_t port)
{
    if (!s_i2c_bus_initialized || (s_i2c_bus_mutex == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_i2c_bus_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    const gpio_num_t sda = s_i2c_bus_config.sda_gpio;
    const gpio_num_t scl = s_i2c_bus_config.scl_gpio;

    int sda_level = gpio_get_level(sda);
    int scl_level = gpio_get_level(scl);

    if (sda_level == 1 && scl_level == 1) {
        xSemaphoreGive(s_i2c_bus_mutex);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "I2C bus stuck (SDA=%d SCL=%d), attempting recovery", sda_level, scl_level);

    /* Toggle SCL 9 times to release a stuck slave (per I2C spec) */
    i2c_driver_delete(port);

    gpio_config_t scl_cfg = {
        .pin_bit_mask = (1ULL << scl),
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&scl_cfg);

    for (int i = 0; i < 9; i++) {
        gpio_set_level(scl, 0);
        esp_rom_delay_us(5);
        gpio_set_level(scl, 1);
        esp_rom_delay_us(5);
    }

    /* Check if SDA is released */
    sda_level = gpio_get_level(sda);
    if (sda_level == 0) {
        ESP_LOGE(TAG, "I2C SDA still stuck low after 9 SCL toggles, reinitializing driver");
    }

    /* Reinitialize I2C driver */
    const i2c_config_t idf_config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = s_i2c_bus_config.clock_speed_hz,
        .clk_flags = I2C_SCLK_SRC_FLAG_AWARE_DFS,  /* DFS-aware source so SCL timing
                                                    * survives APB scaling (Phase 2) */
    };

    esp_err_t err = i2c_param_config(port, &idf_config);
    if (err == ESP_OK) {
        err = i2c_driver_install(port, I2C_MODE_MASTER, 0, 0, 0);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C driver reinit failed: %s", esp_err_to_name(err));
        xSemaphoreGive(s_i2c_bus_mutex);
        return ESP_FAIL;
    }

    sda_level = gpio_get_level(sda);
    scl_level = gpio_get_level(scl);

    if (sda_level == 0) {
        ESP_LOGE(TAG, "I2C bus unrecoverable (SDA still low after reinit)");
        xSemaphoreGive(s_i2c_bus_mutex);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "I2C bus recovered successfully");
    xSemaphoreGive(s_i2c_bus_mutex);
    return ESP_OK;
}

esp_err_t i2c_bus_probe_addr(i2c_port_t port, uint8_t addr)
{
    if (!s_i2c_bus_initialized || (s_i2c_bus_mutex == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_i2c_bus_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);

    esp_err_t err = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    xSemaphoreGive(s_i2c_bus_mutex);

    if (err == ESP_OK) {
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}
