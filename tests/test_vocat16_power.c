#include "power_state.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    assert(memo_battery_percent(100) == 100);
    assert(memo_battery_percent(37) == 37);
    assert(memo_battery_percent(101) == 100);
    assert(memo_battery_low(15) && !memo_battery_low(16));
    assert(memo_battery_critical(5) && !memo_battery_critical(6));
    assert(!memo_battery_recovery_ready(true, false, false, 0));
    assert(!memo_battery_recovery_ready(true, true, false, 0));
    assert(!memo_battery_recovery_ready(true, false, true, 5));
    assert(memo_battery_recovery_ready(true, false, true, 6));
    assert(memo_battery_recovery_ready(true, true, true, 1));
    assert(memo_battery_recovery_ready(false, false, false, 0));

    memo_battery_guard_t guard = {0};
    assert(!memo_battery_shutdown_due(&guard, false, false, true, 5, 1000));
    assert(!memo_battery_shutdown_due(&guard, false, false, true, 5, 10999));
    assert(memo_battery_shutdown_due(&guard, false, false, true, 5, 11000));
    assert(!memo_battery_shutdown_due(&guard, true, false, true, 5, 11001));
    assert(!guard.tracking);
    assert(!memo_battery_shutdown_due(&guard, false, false, true, 2, 20000));
    assert(memo_battery_shutdown_due(&guard, false, false, true, 2, 27000));
    assert(!memo_battery_shutdown_due(&guard, false, true, true, 2, 28000));
    assert(!memo_battery_shutdown_due(&guard, false, false, false, 0, 29000));
    puts("PASS: VoCat BQ27220 SOC thresholds, sustained low battery, charger cancellation");
}
