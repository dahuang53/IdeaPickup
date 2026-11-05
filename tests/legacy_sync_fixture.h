#pragma once
#include "memo_sync_cache.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {uint8_t bytes[16];} memo_server_uuid_t;
typedef struct {memo_server_uuid_t instance;uint8_t url_hash[32];char device_id[40];} memo_sync_target_t;
typedef struct {uint8_t sha256[32];uint64_t bytes;} memo_audio_identity_t;
typedef struct {
 uint32_t id;memo_job_state_t state;bool automatic,bound,manual;
 memo_sync_target_t target;memo_audio_identity_t audio;
} legacy_sync_fixture_t;
/* Test data producer only. Firmware retains no legacy sidecar writer. */
bool legacy_fixture_write(const char *,const legacy_sync_fixture_t *);
#ifdef __cplusplus
}
#endif
