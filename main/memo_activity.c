#include "memo_activity.h"
bool memo_config_activity_acquire(memo_config_activity_gate_t *gate, memo_config_activity_t owner)
{
    if (!gate || (owner != MEMO_ACTIVITY_AUDIO && owner != MEMO_ACTIVITY_COMMIT && owner != MEMO_ACTIVITY_MEDIA)) return false;
    uint32_t expected = MEMO_ACTIVITY_IDLE;
    return __atomic_compare_exchange_n(&gate->owner, &expected, (uint32_t)owner, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
bool memo_config_activity_release(memo_config_activity_gate_t *gate, memo_config_activity_t owner)
{
    if (!gate || (owner != MEMO_ACTIVITY_AUDIO && owner != MEMO_ACTIVITY_COMMIT && owner != MEMO_ACTIVITY_MEDIA)) return false;
    uint32_t expected = (uint32_t)owner;
    return __atomic_compare_exchange_n(&gate->owner, &expected, MEMO_ACTIVITY_IDLE, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
memo_config_activity_t memo_config_activity(const memo_config_activity_gate_t *gate)
{
    return gate ? (memo_config_activity_t)__atomic_load_n(&gate->owner, __ATOMIC_SEQ_CST) : MEMO_ACTIVITY_IDLE;
}
