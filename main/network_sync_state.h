#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "memo_sync_cache.h"

#ifdef __cplusplus
extern "C" {
#endif

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
/* ASR -> DELIVERY and retry waits keep the same task; only terminal states
 * finish the device attempt. Details always read saved local state. */
bool memo_sync_device_attempt_done(memo_job_state_t state);

#ifdef __cplusplus
}
#endif
