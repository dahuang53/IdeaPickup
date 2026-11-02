#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Single current configuration; USB input and writes belong to step 5. */
typedef struct {
 bool enabled;
 uint32_t qwen_generation,siyuan_generation;
 char workspace[64],qwen_key[257],siyuan_token[257];
} memo_cloud_config_t;
bool memo_cloud_config_ready(const memo_cloud_config_t *);
void memo_cloud_config_clear(memo_cloud_config_t *);
/* Missing namespace succeeds unconfigured. Invalid data never falls back to legacy. */
bool memo_cloud_config_read(memo_cloud_config_t *);
bool memo_cloud_config_decode(const void *,size_t,memo_cloud_config_t *);
#ifdef __cplusplus
}
#endif
