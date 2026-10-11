#include "power_state.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    const uint64_t minute = 60ULL * 1000ULL;
    assert(memo_power_stage(false, false, false, 60 * minute) == MEMO_POWER_ACTIVE);
    assert(memo_power_stage(true, true, false, 60 * minute) == MEMO_POWER_ACTIVE);
    assert(memo_power_stage(true, false, true, 60 * minute) == MEMO_POWER_ACTIVE);
    assert(memo_power_stage(true, false, false, 2 * minute - 1) == MEMO_POWER_ACTIVE);
    assert(memo_power_stage(true, false, false, 2 * minute) == MEMO_POWER_VOICE_SLEEP);
    assert(memo_power_stage(true, false, false, 5 * minute - 1) == MEMO_POWER_VOICE_SLEEP);
    assert(memo_power_stage(true, false, false, 5 * minute) == MEMO_POWER_BUTTON_SLEEP);
    assert(memo_power_stage(true, false, false, 30 * minute - 1) == MEMO_POWER_BUTTON_SLEEP);
    assert(memo_power_stage(true, false, false, 30 * minute) == MEMO_POWER_SHUTDOWN);
    assert(memo_power_active_brightness(false, false) == 70);
    assert(memo_power_active_brightness(true, true) == 70);
    assert(memo_power_active_brightness(true, false) == 15);
    assert(memo_battery_percent(1800) == 0);
    assert(memo_battery_percent(1850) == 0);
    assert(memo_battery_percent(1950) == 10);
    assert(memo_battery_percent(2050) == 25);
    assert(memo_battery_percent(2150) == 50);
    assert(memo_battery_percent(2250) == 75);
    assert(memo_battery_percent(2350) == 90);
    assert(memo_battery_percent(2480) == 100);
    assert(memo_battery_percent(2600) == 100);
    assert(!memo_battery_low(2051) && memo_battery_low(2050));
    assert(!memo_battery_critical(1951) && memo_battery_critical(1950));
    assert(!memo_battery_emergency(1851) && memo_battery_emergency(1850));
    memo_battery_guard_t guard = {0};
    assert(!memo_battery_shutdown_due(&guard, false, false, true, 1940, 1000));
    assert(!memo_battery_shutdown_due(&guard, false, false, true, 1940, 10999));
    assert(memo_battery_shutdown_due(&guard, false, false, true, 1940, 11000));
    assert(!memo_battery_shutdown_due(&guard, true, false, true, 1800, 12000));
    assert(!guard.tracking);
    assert(!memo_battery_shutdown_due(&guard, false, false, true, 1800, 20000));
    assert(!memo_battery_shutdown_due(&guard, false, false, true, 1800, 26999));
    assert(memo_battery_shutdown_due(&guard, false, false, true, 1800, 27000));
    assert(!memo_battery_shutdown_due(&guard, false, true, true, 1800, 28000));
    assert(!guard.tracking);
    puts("PASS: Spotpear power boundaries, battery estimate, thresholds, priority, and brightness");
}
