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
typedef struct {
 uint32_t id;
 memo_job_state_t state;
 bool bound;
} memo_sync_entry_t;
typedef enum { MEMO_CACHE_MISSING, MEMO_CACHE_OK, MEMO_CACHE_INVALID, MEMO_CACHE_IO_ERROR } memo_cache_result_t;
/* Read-only legacy metadata compatibility. Legacy target/hash fields are
 * validated in place, never retained as runtime state or used for requests. */
memo_cache_result_t memo_sync_entry_read(const char *,uint32_t,memo_sync_entry_t *);
#ifdef __cplusplus
}
#endif
