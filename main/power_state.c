#include "power_state.h"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#define VOICE_SLEEP_MS (2ULL * 60ULL * 1000ULL)
#define BUTTON_SLEEP_MS (5ULL * 60ULL * 1000ULL)
#define SHUTDOWN_MS (30ULL * 60ULL * 1000ULL)
#define EXTERNAL_BRIGHTNESS_PERCENT 70U
#define BATTERY_BRIGHTNESS_PERCENT 15U

memo_power_stage_t memo_power_stage(bool supported, bool external_power,
                                    bool busy, uint64_t idle_ms)
{
    if (!supported || external_power || busy) return MEMO_POWER_ACTIVE;
    if (idle_ms < VOICE_SLEEP_MS) return MEMO_POWER_ACTIVE;
    if (idle_ms < BUTTON_SLEEP_MS) return MEMO_POWER_VOICE_SLEEP;
    if (idle_ms < SHUTDOWN_MS) return MEMO_POWER_BUTTON_SLEEP;
    return MEMO_POWER_SHUTDOWN;
}

unsigned memo_power_active_brightness(bool supported, bool external_power)
{
    if (supported && !external_power) return BATTERY_BRIGHTNESS_PERCENT;
    return EXTERNAL_BRIGHTNESS_PERCENT;
}

unsigned memo_battery_percent(uint16_t raw)
{
#if defined(CONFIG_MEMO_VOCAT16) && CONFIG_MEMO_VOCAT16
    /* This V1.2 unit has a real coulomb-counting BQ27220 gauge. */
    return raw > 100U ? 100U : raw;
#else
    /* Approximate Li-ion discharge anchors on this board's ADC scale.  The UI
     * rounds this result to 10%, so voltage sag never masquerades as a precise
     * fuel-gauge reading. */
    static const struct { uint16_t raw; uint8_t percent; } anchors[] = {
        {1850, 0}, {1950, 10}, {2050, 25}, {2150, 50},
        {2250, 75}, {2350, 90}, {2480, 100},
    };
    if (raw <= anchors[0].raw) return 0;
    for (unsigned i = 1; i < sizeof(anchors) / sizeof(anchors[0]); ++i) {
        if (raw <= anchors[i].raw) {
            unsigned span = anchors[i].raw - anchors[i - 1].raw;
            unsigned gain = anchors[i].percent - anchors[i - 1].percent;
            return anchors[i - 1].percent +
                   (unsigned)(raw - anchors[i - 1].raw) * gain / span;
        }
    }
    return 100;
#endif
}

#if defined(CONFIG_MEMO_VOCAT16) && CONFIG_MEMO_VOCAT16
bool memo_battery_low(uint16_t raw) { return raw <= 15U; }
bool memo_battery_critical(uint16_t raw) { return raw <= 5U; }
bool memo_battery_emergency(uint16_t raw) { return raw <= 2U; }
#else
bool memo_battery_low(uint16_t raw) { return raw <= 2050U; }
bool memo_battery_critical(uint16_t raw) { return raw <= 1950U; }
bool memo_battery_emergency(uint16_t raw) { return raw <= 1850U; }
#endif

bool memo_battery_recovery_ready(bool supported, bool external, bool valid, uint16_t guard)
{
    return !supported || (valid && (external || !memo_battery_critical(guard)));
}

bool memo_battery_shutdown_due(memo_battery_guard_t *guard, bool external,
                               bool rising, bool valid, uint16_t raw,
                               uint64_t now_ms)
{
    if (!guard) return false;
    if (external || rising || !valid || !memo_battery_critical(raw)) {
        guard->tracking = false;
        guard->since_ms = 0;
        return false;
    }
    if (!guard->tracking) {
        guard->tracking = true;
        guard->since_ms = now_ms;
        return false;
    }
    uint64_t confirm_ms = memo_battery_emergency(raw) ? 7000ULL : 10000ULL;
    return now_ms - guard->since_ms >= confirm_ms;
}
