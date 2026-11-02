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
#define MEMO_CARDLESS_DISPLAY_ID(id) ((id)&UINT32_C(0x7fffffff))
typedef struct {
 uint32_t id,duration_ms,encoded_bytes;
 uint64_t received_samples;
 memo_job_state_t state;
 bool can_retry,interrupted,may_have_remote_audio;
 char error[80];
 memo_sync_target_t target; /* Old records1 decoder only, never a live target. */
 bool direct;
 uint32_t qwen_generation,siyuan_generation,delivery_attempts;
} memo_cardless_record_t;
/* Bounded 1024x16B index + four detail slots in the original memo_meta partition.
 * records1 is read-only; new direct1 records <=128B. No erase or eviction. */
esp_err_t memo_cardless_init(void);
bool memo_cardless_ready(void);
esp_err_t memo_cardless_observe_id(uint32_t);
/* Existing single network owner; no Opus task or reconnect/history poller. */
void memo_cardless_network_step(void);
/* Refresh saved readiness; interrupt only after an actual committed change. */
void memo_cardless_configure(bool ready,bool changed);
esp_err_t memo_cardless_begin(uint32_t floor,uint32_t *id);
/* Single producer 16kHz PCM16; nonblocking all-or-nothing enqueue. */
bool memo_cardless_append(const int16_t *,size_t samples);
void memo_cardless_finish(bool interrupted,const char *reason);
bool memo_cardless_recording(void);
bool memo_cardless_busy(void);
bool memo_cardless_should_stop(char *,size_t);
uint32_t memo_cardless_elapsed_ms(void);
void memo_cardless_error(char *,size_t);
size_t memo_cardless_count(void);
size_t memo_cardless_list(size_t,memo_cardless_record_t *,size_t);
bool memo_cardless_get(uint32_t,memo_cardless_record_t *);
void memo_cardless_select(uint32_t);
bool memo_cardless_peek(uint32_t,memo_cardless_record_t *);
uint32_t memo_cardless_revision(void);
/* Only this active task can be cancelled. Finished records have no audio/retry. */
bool memo_cardless_can_cancel(uint32_t);
bool memo_cardless_cancel(uint32_t);
#ifdef __cplusplus
}
#endif
