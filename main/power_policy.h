#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "power_state.h"

typedef struct {
    bool supported;
    bool initialized;
    bool external_power;
    bool busy;
    bool battery_valid;
    bool battery_low;
    bool critical_pending;
    uint16_t battery_raw;
    uint16_t battery_guard_raw;
    unsigned battery_percent;
    memo_power_stage_t stage;
    uint32_t idle_seconds;
} memo_power_status_t;

void power_policy_init(void);
void power_policy_note_activity(const char *source);
void power_policy_get_status(memo_power_status_t *out);
/* Safe before the policy task starts: queries the initialized board monitor. */
bool power_policy_recovery_ready(void);
