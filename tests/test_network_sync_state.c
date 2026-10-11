#include "network_sync_state.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    assert(memo_sync_remote_action(0, NULL) == MEMO_SYNC_RETRY_LATER);
    assert(memo_sync_remote_action(404, NULL) == MEMO_SYNC_UPLOAD);
    assert(memo_sync_remote_action(200, "sent") == MEMO_SYNC_RECOVER_SENT);
    assert(memo_sync_remote_action(200, "transcription_rejected") ==
           MEMO_SYNC_RECOVER_REJECTED);
    assert(memo_sync_remote_action(200, "received") == MEMO_SYNC_WAIT_REMOTE);
    assert(memo_sync_remote_action(200, "transcribing") == MEMO_SYNC_WAIT_REMOTE);
    assert(memo_sync_remote_action(200, "transcription_failed") == MEMO_SYNC_UPLOAD);
    assert(memo_sync_remote_action(503, "transcribing") == MEMO_SYNC_RETRY_LATER);
    assert(memo_sync_remote_action(200, "sending") == MEMO_SYNC_WAIT_REMOTE);
    assert(memo_sync_upload_response_pending(-1));
    assert(memo_sync_upload_response_pending(0));
    assert(!memo_sync_upload_response_pending(200));
    assert(!memo_sync_upload_response_pending(202));
    assert(!memo_sync_device_attempt_done(MEMO_JOB_UNKNOWN));
    assert(!memo_sync_device_attempt_done(MEMO_JOB_CHECKING));
    assert(!memo_sync_device_attempt_done(MEMO_JOB_UPLOADING));
    assert(memo_sync_device_attempt_done(MEMO_JOB_WAITING));
    assert(memo_sync_device_attempt_done(MEMO_JOB_SENT));
    assert(memo_sync_device_attempt_done(MEMO_JOB_FAILED));
    assert(memo_sync_local_state(MEMO_JOB_UNKNOWN, false)==MEMO_JOB_LOCAL);
    assert(memo_sync_local_state(MEMO_JOB_UNKNOWN, true)==MEMO_JOB_QUEUED);
    assert(memo_sync_local_state(MEMO_JOB_QUEUED, false)==MEMO_JOB_LOCAL);
    assert(memo_sync_local_state(MEMO_JOB_SENT, false)==MEMO_JOB_SENT);

    /* Booting with a high historical ID does not itself populate this queue. */
    memo_sync_queue_t queue = {0};
    assert(!memo_sync_queue_start(&queue, true));
    assert(memo_sync_queue_offer_auto(&queue, 379));
    assert(!memo_sync_queue_start(&queue, false));
    assert(memo_sync_queue_start(&queue, true));
    assert(queue.active_id == 379 && !queue.active_manual && !queue.pending_id);
    assert(memo_sync_queue_offer_auto(&queue, 380));
    assert(memo_sync_queue_offer_auto(&queue, 381));
    assert(queue.pending_id == 381); /* #380 remains local, never backfilled. */
    assert(!memo_sync_queue_offer_auto(&queue, 380));
    assert(!memo_sync_queue_offer_auto(&queue, 379));
    assert(!memo_sync_queue_start(&queue, true));
    memo_sync_queue_finish(&queue, 379);
    assert(memo_sync_queue_start(&queue, true));
    assert(queue.active_id == 381 && !queue.pending_id);
    memo_sync_queue_finish(&queue, 381);
    assert(!memo_sync_queue_start(&queue, true));

    assert(memo_sync_queue_offer_auto(&queue, 382));
    assert(memo_sync_queue_start(&queue, true));
    assert(memo_sync_queue_offer_auto(&queue, 383));
    assert(memo_sync_queue_offer_manual(&queue, 301, false));
    assert(queue.pending_id == 301 && queue.pending_manual);
    assert(!memo_sync_queue_offer_auto(&queue, 384));
    assert(!memo_sync_queue_start(&queue, true));
    memo_sync_queue_manual_persisted(&queue, 301, true);
    memo_sync_queue_finish(&queue, 382);
    assert(memo_sync_queue_start(&queue, true));
    assert(queue.active_id == 301 && queue.active_manual);
    assert(!memo_sync_queue_offer_manual(&queue, 302, false));
    assert(memo_sync_queue_offer_auto(&queue, 385));
    memo_sync_queue_finish(&queue, 301);
    assert(memo_sync_queue_start(&queue, true));
    assert(queue.active_id == 385);
    memo_sync_queue_finish(&queue, 385);
    assert(!memo_sync_queue_start(&queue, true));

    assert(memo_sync_queue_offer_manual(&queue, 300, true));
    assert(memo_sync_queue_start(&queue, false));
    assert(queue.active_id == 300 && queue.active_manual);
    memo_sync_queue_finish(&queue, 300);
    assert(!memo_sync_queue_start(&queue, true));
    assert(memo_sync_queue_offer_manual(&queue, 299, false));
    memo_sync_queue_manual_persisted(&queue, 299, false);
    assert(!memo_sync_queue_contains(&queue, 299));
    puts("PASS: relay states and bounded one-active/one-pending scheduler");
}
