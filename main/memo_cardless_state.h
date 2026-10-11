#pragma once
#include <stdbool.h>
#include "memo_sync_cache.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MEMO_CARDLESS_QUERY_APPLY,
    MEMO_CARDLESS_QUERY_EXPIRED,
    MEMO_CARDLESS_QUERY_MISSING,
    MEMO_CARDLESS_QUERY_AUTH_NOTICE,
    MEMO_CARDLESS_QUERY_REFRESH,
    MEMO_CARDLESS_QUERY_WAIT,
} memo_cardless_query_action_t;

/* Error responses need not contain a stream snapshot. A conflict is resolved
 * with one status GET; lack of authorization is not evidence of job failure. */
memo_cardless_query_action_t memo_cardless_query_action(int http_status,
                                                       bool valid_snapshot,
                                                       memo_job_state_t cached_state);
/* Only a completed server snapshot is authoritative about interruption. An
 * active NVS marker alone is NOT evidence that a recording was truncated. */
bool memo_cardless_interruption(bool cached, bool reported, bool authoritative);
#ifdef __cplusplus
}
#endif
