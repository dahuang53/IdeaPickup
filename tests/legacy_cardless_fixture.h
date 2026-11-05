#pragma once
#include "memo_cardless_record.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
 uint32_t id,duration_ms,encoded_bytes;uint64_t received_samples;
 memo_job_state_t state;bool can_retry,interrupted,may_have_remote_audio;
 char error[80];
 struct {struct {uint8_t bytes[16];} instance;uint8_t url_hash[32];char device_id[40];} target;
} legacy_cardless_fixture_t;
bool memo_cardless_record_encode(const legacy_cardless_fixture_t *,uint8_t [MEMO_CARDLESS_RECORD_SIZE]);
#ifdef __cplusplus
}
#endif
