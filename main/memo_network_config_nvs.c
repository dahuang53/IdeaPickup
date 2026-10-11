/* NVS callbacks/locking extracted from DONOR, with all import/media code removed. */
#include "memo_network_config_nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

static SemaphoreHandle_t config_mutex;
static nvs_handle_t handle;
static bool opened;
static memo_config_t config;

static memo_config_io_t read_blob(void *ctx, const char *key, uint8_t *out, size_t *length)
{
    (void)ctx; esp_err_t e = nvs_get_blob(handle, key, out, length);
    return e == ESP_OK ? MEMO_CONFIG_IO_OK : e == ESP_ERR_NVS_NOT_FOUND ? MEMO_CONFIG_IO_NOT_FOUND : MEMO_CONFIG_IO_ERROR;
}
static memo_config_io_t write_blob(void *ctx, const char *key, const uint8_t *bytes, size_t length)
{
    (void)ctx; return nvs_set_blob(handle, key, bytes, length) == ESP_OK ? MEMO_CONFIG_IO_OK : MEMO_CONFIG_IO_ERROR;
}
static memo_config_io_t commit_blob(void *ctx)
{
    (void)ctx; return nvs_commit(handle) == ESP_OK ? MEMO_CONFIG_IO_OK : MEMO_CONFIG_IO_ERROR;
}
memo_config_result_t memo_network_config_nvs_reload(void)
{
    if (opened) nvs_close(handle);
    opened = false;
    if (nvs_open("memo_cfg1", NVS_READWRITE, &handle) != ESP_OK) {
        config.loaded = false; config.needs_reload = true; config.result = MEMO_CONFIG_STORAGE_ERROR; return config.result;
    }
    opened = true;
    memo_config_storage_t io = {NULL, read_blob, write_blob, commit_blob};
    return memo_config_load(&config, &io);
}
esp_err_t memo_network_config_nvs_init(void)
{
    if (nvs_flash_init() != ESP_OK) return ESP_FAIL;
    if (!config_mutex) config_mutex = xSemaphoreCreateRecursiveMutex();
    if (!config_mutex) return ESP_ERR_NO_MEM;
    xSemaphoreTakeRecursive(config_mutex, portMAX_DELAY);
    memo_config_result_t result = memo_network_config_nvs_reload();
    xSemaphoreGiveRecursive(config_mutex);
    return result == MEMO_CONFIG_OK ? ESP_OK : ESP_FAIL;
}
memo_config_t *memo_network_config_nvs_lock(void)
{
    if (!config_mutex) return NULL;
    return xSemaphoreTakeRecursive(config_mutex, portMAX_DELAY) == pdTRUE ? &config : NULL;
}
void memo_network_config_nvs_unlock(void) { xSemaphoreGiveRecursive(config_mutex); }
