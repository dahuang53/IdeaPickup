#include "memo_cardless_state.h"

memo_cardless_query_action_t memo_cardless_query_action(int http_status,
                                                       bool valid_snapshot,
                                                       memo_job_state_t cached_state)
{
    if (http_status == 401 || http_status == 403) return MEMO_CARDLESS_QUERY_AUTH_NOTICE;
    if (http_status == 410) return MEMO_CARDLESS_QUERY_EXPIRED;
    if (http_status == 409) return MEMO_CARDLESS_QUERY_REFRESH;
    if ((http_status == 200 || http_status == 202) && valid_snapshot) return MEMO_CARDLESS_QUERY_APPLY;
    if (http_status == 404 && cached_state != MEMO_JOB_SENT) return MEMO_CARDLESS_QUERY_MISSING;
    return MEMO_CARDLESS_QUERY_WAIT;
}

bool memo_cardless_interruption(bool cached, bool reported, bool authoritative)
{
    return authoritative ? reported : cached || reported;
}
