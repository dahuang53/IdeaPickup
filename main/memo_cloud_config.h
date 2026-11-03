#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Single current provider configuration; no legacy token fallback. */
typedef struct {
 bool enabled;
 uint32_t qwen_generation,siyuan_generation;
 char qwen_key[257],siyuan_token[257];
} memo_cloud_config_t;
bool memo_cloud_config_ready(const memo_cloud_config_t *);
void memo_cloud_config_clear(memo_cloud_config_t *);
/* Missing namespace succeeds unconfigured. Invalid data never falls back to legacy. */
bool memo_cloud_config_read(memo_cloud_config_t *);
bool memo_cloud_config_decode(const void *,size_t,memo_cloud_config_t *);
bool memo_cloud_config_encode(const memo_cloud_config_t *,uint8_t out[604]);
typedef enum {MEMO_CLOUD_OK,MEMO_CLOUD_INVALID,MEMO_CLOUD_BUSY,MEMO_CLOUD_STORAGE,MEMO_CLOUD_CONFLICT,MEMO_CLOUD_NO_SPACE,MEMO_CLOUD_NOT_CONFIGURED} memo_cloud_result_t;
typedef struct {
 const char *qwen_key,*siyuan_token; /* NULL/empty keep current. */
 bool clear_qwen,clear_siyuan,set_mode,enabled;
 bool check_generation;
 uint32_t expected_qwen,expected_siyuan;
} memo_cloud_patch_t;
typedef struct {
 memo_cloud_result_t result;
 bool enabled,configured,qwen_set,siyuan_set;
 uint32_t qwen_generation,siyuan_generation;
} memo_cloud_public_t;
/* Caller serializes commits using the existing network/activity gate. */
memo_cloud_result_t memo_cloud_settings_apply(const memo_cloud_patch_t *,bool *changed);
void memo_cloud_settings_public(memo_cloud_public_t *);
#ifdef __cplusplus
}
#endif
