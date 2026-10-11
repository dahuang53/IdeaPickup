#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "memo_sync_cache.h"
#if defined(MEMO_HOST_TEST) || defined(CAT_ANIM_TEST)
typedef int esp_err_t;
#else
#include "esp_err.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define MEMO_CARDLESS_ID_FLAG UINT32_C(0x80000000)
#define MEMO_CARDLESS_DISPLAY_ID(id) ((id) & UINT32_C(0x7fffffff))

typedef struct {
    uint32_t id;
    uint32_t duration_ms;
    uint32_t encoded_bytes;
    uint64_t received_samples;
    memo_job_state_t state;
    bool can_retry;
    bool interrupted;
    bool may_have_remote_audio; /* committed before the first PUT, internal only */
    char error[80];
    memo_sync_target_t target;
} memo_cardless_record_t;

/* Uses records1 in its own memo_meta NVS partition. Never erases a damaged partition or
 * evicts old records when full. Audio is only held in bounded PSRAM. Call before
 * board audio/Wi-Fi initialization to reserve the persistent 32 KiB internal
 * encoder stack; its worker blocks on notifications while idle. */
esp_err_t memo_cardless_init(void);
bool memo_cardless_ready(void);
/* Observe an SD/network display floor in RAM. Shared device allocation commits
 * max(persistent highwater, observed floor) before a cardless begin; there is no
 * second persistent counter here. Does not change records or schedule work. */
esp_err_t memo_cardless_observe_id(uint32_t floor);
/* Call after board audio and network initialization; only an interrupted active
 * marker starts the separate network worker. That worker remains lazy on an
 * idle SD-only boot; the encoder task was already reserved by init(). */
esp_err_t memo_cardless_start_pending(void);
/* Before begin, the caller must hold its existing audio/network power guard.
 * floor is the highest visible SD/network ID; the persistent cardless counter
 * and the shared device allocator prevent collisions. Wi-Fi and pairing are required. */
esp_err_t memo_cardless_begin(uint32_t floor, uint32_t *id);
/* Single producer, 16 kHz mono PCM; nonblocking, all-or-nothing enqueue.
 * On false stop acquisition then call finish(true,...). No oldest data drop. */
bool memo_cardless_append(const int16_t *pcm, size_t samples);
void memo_cardless_finish(bool interrupted, const char *reason);
bool memo_cardless_recording(void);
/* True while unique unsent audio or an unfinished stream recovery exists.
 * Cloud ASR after confirmed finish does not occupy this transport slot. */
bool memo_cardless_busy(void);
bool memo_cardless_should_stop(char *error, size_t capacity);
uint32_t memo_cardless_elapsed_ms(void);
void memo_cardless_error(char *error, size_t capacity);

/* Snapshots only: no network or flash IO in UI getters. List newest first. */
size_t memo_cardless_count(void);
size_t memo_cardless_list(size_t offset, memo_cardless_record_t *out, size_t capacity);
bool memo_cardless_get(uint32_t id, memo_cardless_record_t *out);
uint32_t memo_cardless_revision(void);
bool memo_cardless_request_status(uint32_t id);
bool memo_cardless_retry(uint32_t id);

/* Serial-only diagnostics. Control flags are RAM-only and cleared by reboot;
 * no SD records are targeted. Synthetic responses still pass through normal
 * state handling and can persist changes to the new test record's metadata.
 * Maintainers must perform a real status query afterward to restore server facts.
 * Kinds 1..3 require an active non-recovery cardless recording:
 * 1: block its HTTP requests for value seconds (1..90).
 * 2: discard its next successful PUT response after the server accepts it (value=0).
 * 3: discard its next successful finish response, then block this ID's GET/finish
 *    for value seconds (1..90), allowing a reset to test lost-ACK recovery.
 * Kinds 4..6 require idle, with no begin in progress:
 * 4: reject next begin before metadata writes, latch metadata fault (value=0).
 * 5: reject next begin at metadata capacity preflight (value=0).
 * 6: next successful begin uses a value-frame ring (250..1500); normal capacity
 *    resumes only once that recording releases its transport slot.
 * Kinds 7..9 require idle and value equal to the full ID (including high bit) of
 * the latest cardless record, which must have been newly created this boot:
 * 7/8/9: next GET returns 401/409/410 once, with no network request or reply body.
 *    These test only client handling; a later real GET can restore server facts.
 * 10: active non-recovery recording's next PUT returns 401 before network IO
 *    (value=0); retained audio can then use the normal explicit retry path. */
bool memo_cardless_test_fault(unsigned kind, uint32_t value);

#ifdef __cplusplus
}
#endif
