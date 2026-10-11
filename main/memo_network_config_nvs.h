#pragma once
#include "memo_network_config.h"
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t memo_network_config_nvs_init(void);
memo_config_t *memo_network_config_nvs_lock(void);
void memo_network_config_nvs_unlock(void);
/* Caller owns the lock and audio commit gate; closes an uncertain handle before
 * reading durable state. No namespace erase or old configuration import. */
memo_config_result_t memo_network_config_nvs_reload(void);
#ifdef __cplusplus
}
#endif
