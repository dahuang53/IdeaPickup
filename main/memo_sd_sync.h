#pragma once
#include "memo_store.h"
#include "memo_sync_cache.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
 void *context;
 bool (*begin_io)(void *);
 void (*end_io)(void *);
 bool (*hash_file)(void *,const char *,memo_audio_identity_t *);
 int (*status)(void *,uint32_t,char *,size_t);
 int (*upload)(void *,const memo_record_t *,const memo_audio_identity_t *,char *,size_t);
 int (*retry)(void *,uint32_t,char *,size_t);
} memo_sd_sync_io_t;
typedef struct { memo_job_state_t state; bool can_retry, pending; char error[80]; } memo_sd_sync_result_t;
/* Reads only first-public pending sidecars; never treats arbitrary WAVs as new work.
 * complete distinguishes an empty scan from unavailable directory I/O. */
uint32_t memo_sd_sync_pending(const char *root,const memo_sync_target_t *,uint32_t *after,bool *manual,bool *complete);
/* mode 0: durable new-record intent; 1: explicit retry; 2: inspect only.
 * Requests run only after exact current-file hash and durable current binding. */
void memo_sd_sync_attempt(const char *wav,const char *cache,uint32_t id,const memo_sync_target_t *,int mode,const memo_sd_sync_io_t *,memo_sd_sync_result_t *);
#ifdef __cplusplus
}
#endif
