#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define MEMO_RECORD_ID_MAX UINT32_C(0x7fffffff)
#define MEMO_RECORD_IDS_FACT_SIZE 16U
typedef enum { MEMO_IDS_OK, MEMO_IDS_MISSING, MEMO_IDS_IO_ERROR, MEMO_IDS_INVALID, MEMO_IDS_EXHAUSTED } memo_ids_result_t;
typedef struct {
 void *context;
 memo_ids_result_t (*read)(void *,uint32_t *);
 /* Success requires commit and exact fresh-handle readback. */
 memo_ids_result_t (*write)(void *,uint32_t);
} memo_ids_storage_t;
typedef struct { memo_ids_storage_t storage; uint32_t highwater; bool loaded; } memo_ids_t;
bool memo_record_ids_decode_fact(const uint8_t *,size_t,uint32_t *);
bool memo_record_ids_encode_fact(uint32_t,uint8_t *,size_t);
memo_ids_result_t memo_ids_load(memo_ids_t *,const memo_ids_storage_t *);
memo_ids_result_t memo_ids_reserve(memo_ids_t *,uint32_t floor,uint32_t *id);
bool memo_record_ids_observe(uint32_t floor);
/* One new namespace, no legacy mirror/import/history declarations. */
bool memo_record_ids_init(void);
bool memo_record_ids_reserve(uint32_t floor,uint32_t *id);
#ifdef __cplusplus
}
#endif
