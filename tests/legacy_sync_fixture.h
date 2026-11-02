#pragma once
#include "memo_sync_cache.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Test data producer only. Firmware retains no legacy sidecar writer. */
bool legacy_fixture_write(const char *,const memo_sync_entry_t *);
#ifdef __cplusplus
}
#endif
