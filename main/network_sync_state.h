#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "memo_sync_cache.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MEMO_SYNC_UPLOAD,
    MEMO_SYNC_RECOVER_SENT,
    MEMO_SYNC_RECOVER_REJECTED,
    MEMO_SYNC_WAIT_REMOTE,
    MEMO_SYNC_RETRY_LATER,
} memo_sync_remote_action_t;

/* Decide what to do before uploading audio from the relay's status response. */
memo_sync_remote_action_t memo_sync_remote_action(int http_status,
                                                  const char *remote_status);

/* Once every upload byte has been written, a missing HTTP response does not
 * prove that the relay missed the memo.  Espressif may report that condition
 * as either zero or a negative status code, depending on where the response
 * timeout occurred. */
bool memo_sync_upload_response_pending(int http_status);

/* One active transfer and one replaceable pending record. Historical IDs are
 * never inferred from a high-water mark. A manual request replaces an
 * automatic pending record but never interrupts the active transfer. */
typedef struct {
    uint32_t active_id;
    uint32_t pending_id;
    bool active_manual;
    bool pending_manual;
    bool pending_persisted;
} memo_sync_queue_t;

bool memo_sync_queue_offer_auto(memo_sync_queue_t *queue, uint32_t id);
bool memo_sync_queue_offer_manual(memo_sync_queue_t *queue, uint32_t id,
                                  bool persisted);
void memo_sync_queue_manual_persisted(memo_sync_queue_t *queue, uint32_t id,
                                      bool success);
bool memo_sync_queue_start(memo_sync_queue_t *queue, bool auto_settled);
void memo_sync_queue_finish(memo_sync_queue_t *queue, uint32_t id);
bool memo_sync_queue_contains(const memo_sync_queue_t *queue, uint32_t id);
memo_job_state_t memo_sync_local_state(memo_job_state_t cached, bool scheduled);
/* Relay acceptance finishes device-side sending. Background reconciliation
 * tracks the same task; opening details only reads its saved local state. */
bool memo_sync_device_attempt_done(memo_job_state_t state);

#ifdef __cplusplus
}
#endif
