#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    MEMO_POWER_ACTIVE = 0,
    MEMO_POWER_VOICE_SLEEP,
    MEMO_POWER_BUTTON_SLEEP,
    MEMO_POWER_SHUTDOWN,
} memo_power_stage_t;

/* Portable policy core. Runtime code owns the idle clock and passes elapsed
 * true-idle time; busy work and external power always win over saving. */
memo_power_stage_t memo_power_stage(bool supported, bool external_power,
                                    bool busy, uint64_t idle_ms);

/* Active-mode brightness follows the power source on boards that support the
 * policy. Battery operation stays readable at low brightness from the start. */
unsigned memo_power_active_brightness(bool supported, bool external_power);

/* The Spotpear board exposes only cell voltage, not a coulomb counter.  Report
 * a deliberately coarse, load-tolerant estimate instead of false precision. */
unsigned memo_battery_percent(uint16_t raw);
bool memo_battery_low(uint16_t raw);
bool memo_battery_critical(uint16_t raw);
bool memo_battery_emergency(uint16_t raw);
bool memo_battery_recovery_ready(bool supported, bool external, bool valid, uint16_t guard);

typedef struct {
    bool tracking;
    uint64_t since_ms;
} memo_battery_guard_t;

/* Debounce load sag and give charger detection time to observe a rising cell.
 * Returns true only after a sustained critical voltage. */
bool memo_battery_shutdown_due(memo_battery_guard_t *guard, bool external,
                               bool rising, bool valid, uint16_t raw,
                               uint64_t now_ms);
