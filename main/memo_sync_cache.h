#pragma once
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
 MEMO_JOB_UNKNOWN, MEMO_JOB_LOCAL, MEMO_JOB_QUEUED, MEMO_JOB_CHECKING,
 MEMO_JOB_UPLOADING, MEMO_JOB_WAITING, MEMO_JOB_RECEIVED, MEMO_JOB_TRANSCRIBING,
 MEMO_JOB_DELIVERING, MEMO_JOB_SENT, MEMO_JOB_FAILED, MEMO_JOB_REJECTED,
 MEMO_JOB_UNCERTAIN, MEMO_JOB_CANCELLED, MEMO_JOB_CANCELLING
} memo_job_state_t;
#ifdef __cplusplus
}
#endif

#include "memo_network_config.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { memo_server_uuid_t instance; uint8_t url_hash[32]; char device_id[40]; } memo_sync_target_t;
typedef struct { uint8_t sha256[32]; uint64_t bytes; } memo_audio_identity_t;
typedef struct {
 uint32_t id;
 memo_job_state_t state;
 bool automatic, bound, manual;
 memo_sync_target_t target;
 memo_audio_identity_t audio;
} memo_sync_entry_t;
typedef enum { MEMO_CACHE_MISSING, MEMO_CACHE_OK, MEMO_CACHE_INVALID, MEMO_CACHE_IO_ERROR } memo_cache_result_t;
/* Current metadata: destination/intent before HTTP; audio digest from server receipt.
 * Two independently fsynced/read-back slots avoid FAT overwrite-rename. */
memo_cache_result_t memo_sync_entry_read(const char *,uint32_t,memo_sync_entry_t *);
bool memo_sync_entry_write(const char *,const memo_sync_entry_t *);
bool memo_sync_target_matches(const memo_sync_target_t *,const memo_sync_target_t *,bool partial);
bool memo_sync_audio_matches(const memo_audio_identity_t *,const memo_audio_identity_t *);
bool memo_sync_entry_equal(const memo_sync_entry_t *,const memo_sync_entry_t *);
#ifdef __cplusplus
}
#endif
