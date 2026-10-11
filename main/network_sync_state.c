#include "network_sync_state.h"

#include <string.h>

memo_sync_remote_action_t memo_sync_remote_action(int http_status,
                                                  const char *remote_status)
{
    if (http_status == 404) return MEMO_SYNC_UPLOAD;
    if (http_status != 200) return MEMO_SYNC_RETRY_LATER;
    if (!remote_status) return MEMO_SYNC_UPLOAD;
    if (strcmp(remote_status, "sent") == 0) return MEMO_SYNC_RECOVER_SENT;
    if (strcmp(remote_status, "transcription_rejected") == 0)
        return MEMO_SYNC_RECOVER_REJECTED;
    if (strcmp(remote_status, "received") == 0 ||
        strcmp(remote_status, "transcribing") == 0 ||
        strcmp(remote_status, "sending") == 0)
        return MEMO_SYNC_WAIT_REMOTE;
    return MEMO_SYNC_UPLOAD;
}

bool memo_sync_upload_response_pending(int http_status)
{
    return http_status <= 0;
}

bool memo_sync_queue_offer_auto(memo_sync_queue_t *queue, uint32_t id)
{
    if (!queue || !id || queue->active_id == id || queue->pending_id == id ||
        queue->pending_manual || queue->pending_id > id) return false;
    queue->pending_id = id;
    queue->pending_manual = false;
    queue->pending_persisted = false;
    return true;
}

bool memo_sync_queue_offer_manual(memo_sync_queue_t *queue, uint32_t id,
                                  bool persisted)
{
    if (!queue || !id || queue->active_id == id || queue->active_manual ||
        queue->pending_manual) return false;
    queue->pending_id = id;
    queue->pending_manual = true;
    queue->pending_persisted = persisted;
    return true;
}

void memo_sync_queue_manual_persisted(memo_sync_queue_t *queue, uint32_t id,
                                      bool success)
{
    if (!queue || !id || queue->pending_id != id || !queue->pending_manual)return;
    if (success)queue->pending_persisted = true;
    else {
        queue->pending_id = 0;
        queue->pending_manual = false;
        queue->pending_persisted = false;
    }
}

bool memo_sync_queue_start(memo_sync_queue_t *queue, bool auto_settled)
{
    if (!queue || queue->active_id || !queue->pending_id ||
        (queue->pending_manual && !queue->pending_persisted) ||
        (!queue->pending_manual && !auto_settled)) return false;
    queue->active_id = queue->pending_id;
    queue->active_manual = queue->pending_manual;
    queue->pending_id = 0;
    queue->pending_manual = false;
    queue->pending_persisted = false;
    return true;
}

void memo_sync_queue_finish(memo_sync_queue_t *queue, uint32_t id)
{
    if (!queue || !id || queue->active_id != id)return;
    queue->active_id = 0;
    queue->active_manual = false;
}

bool memo_sync_queue_contains(const memo_sync_queue_t *queue, uint32_t id)
{
    return queue && id && (queue->active_id == id || queue->pending_id == id);
}

memo_job_state_t memo_sync_local_state(memo_job_state_t cached, bool scheduled)
{
    if (cached==MEMO_JOB_UNKNOWN)return scheduled?MEMO_JOB_QUEUED:MEMO_JOB_LOCAL;
    if (cached==MEMO_JOB_QUEUED && !scheduled)return MEMO_JOB_LOCAL;
    return cached;
}

bool memo_sync_device_attempt_done(memo_job_state_t state)
{
    return state==MEMO_JOB_WAITING || state==MEMO_JOB_RECEIVED ||
           state==MEMO_JOB_TRANSCRIBING || state==MEMO_JOB_DELIVERING ||
           state==MEMO_JOB_SENT || state==MEMO_JOB_FAILED ||
           state==MEMO_JOB_REJECTED || state==MEMO_JOB_UNCERTAIN;
}
