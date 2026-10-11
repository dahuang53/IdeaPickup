#include "memo_cardless_state.h"
#include <assert.h>
#include <stdio.h>

static void test_error_body_is_not_a_snapshot(void)
{
    /* These response bodies contain only error/message, hence valid=false.
     * Expiration must end the Retry button even without recording_id/status. */
    assert(memo_cardless_query_action(410, false, MEMO_JOB_FAILED) == MEMO_CARDLESS_QUERY_EXPIRED);
    assert(memo_cardless_query_action(410, true, MEMO_JOB_FAILED) == MEMO_CARDLESS_QUERY_EXPIRED);
    assert(memo_cardless_query_action(409, false, MEMO_JOB_FAILED) == MEMO_CARDLESS_QUERY_REFRESH);
    /* GET following cannot_retry may reveal success, another active retry or
     * uncertain delivery. All valid states can replace the stale snapshot. */
    assert(memo_cardless_query_action(200, true, MEMO_JOB_FAILED) == MEMO_CARDLESS_QUERY_APPLY);
    assert(memo_cardless_query_action(202, true, MEMO_JOB_UNCERTAIN) == MEMO_CARDLESS_QUERY_APPLY);
    assert(memo_cardless_query_action(200, false, MEMO_JOB_FAILED) == MEMO_CARDLESS_QUERY_WAIT);
    assert(memo_cardless_query_action(0, false, MEMO_JOB_SENT) == MEMO_CARDLESS_QUERY_WAIT);
}

static void test_auth_does_not_rewrite_delivered_record(void)
{
    assert(memo_cardless_query_action(401, false, MEMO_JOB_SENT) == MEMO_CARDLESS_QUERY_AUTH_NOTICE);
    assert(memo_cardless_query_action(403, false, MEMO_JOB_SENT) == MEMO_CARDLESS_QUERY_AUTH_NOTICE);
    assert(memo_cardless_query_action(401, false, MEMO_JOB_FAILED) == MEMO_CARDLESS_QUERY_AUTH_NOTICE);
    assert(memo_cardless_query_action(404, false, MEMO_JOB_SENT) == MEMO_CARDLESS_QUERY_WAIT);
    assert(memo_cardless_query_action(404, false, MEMO_JOB_RECEIVED) == MEMO_CARDLESS_QUERY_MISSING);
}

static void test_lost_finish_ack_is_not_partial_audio(void)
{
    /* Boot sees an active marker, but has not yet learned whether finish was
     * accepted. CHECKING must not create an interrupted flag. */
    bool interrupted = memo_cardless_interruption(false, false, false);
    assert(!interrupted);
    /* Finish accepted before power loss: GET received/sent, interrupted=false. */
    interrupted = memo_cardless_interruption(interrupted, false, true);
    assert(!interrupted);
    /* Also repair flags produced by the old premature-interruption behavior. */
    assert(!memo_cardless_interruption(true, false, true));
}

static void test_only_confirmed_prefix_recovery_is_partial(void)
{
    bool interrupted = memo_cardless_interruption(false, false, false);
    /* A server still receiving has not finalized/truncated anything yet. */
    assert(!memo_cardless_interruption(interrupted, false, false));
    /* The explicit finish(interrupted=true) response establishes that fact. */
    interrupted = memo_cardless_interruption(interrupted, true, true);
    assert(interrupted);
    assert(memo_cardless_interruption(interrupted, false, false));
    assert(memo_cardless_interruption(false, true, true));
}

int main(void)
{
    test_error_body_is_not_a_snapshot();
    test_auth_does_not_rewrite_delivered_record();
    test_lost_finish_ack_is_not_partial_audio();
    test_only_confirmed_prefix_recovery_is_partial();
    puts("cardless state: PASS (410 retry expiry, 409 refresh, auth preservation, lost-finish-ACK recovery)");
    return 0;
}
