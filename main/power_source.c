#include "power_source.h"

#include <string.h>

/* Values use the same 12-bit ADC scale as the maintained Xiaozhi definition
 * for this exact board (2480 ~= full).  Eight-sample averaging happens in the
 * board layer before values reach this estimator. */
#define FULL_CELL_RAW 2420U
#define FULL_CONFIRM_MS 15000ULL
#define TREND_WINDOW_MS 45000ULL
#define CHARGE_RISE_RAW 8
#define UNPLUG_DROP_RAW 12U
#define LOAD_SETTLE_MS 60000ULL

void memo_power_source_init(memo_power_source_t *source)
{
    if (source) memset(source, 0, sizeof(*source));
}

static void reset_window(memo_power_source_t *source, uint64_t now_ms,
                         uint16_t raw)
{
    source->baseline_ms = now_ms;
    source->baseline_raw = raw;
    source->peak_raw = raw;
    source->last_raw = raw;
    source->window_delta = 0;
}

void memo_power_source_load_changed(memo_power_source_t *source, uint64_t now_ms)
{
    if (!source) return;
    source->settle_until_ms = now_ms + LOAD_SETTLE_MS;
    if (source->initialized) reset_window(source, now_ms, source->last_raw);
}

bool memo_power_source_update(memo_power_source_t *source, uint64_t now_ms,
                              bool usb_host, bool charge_signal_valid,
                              bool charge_signal_high, bool adc_valid,
                              uint16_t adc_raw)
{
    if (!source) return usb_host || (charge_signal_valid && charge_signal_high);
    bool hard_external = usb_host || (charge_signal_valid && charge_signal_high);

    if (source->hard_external && !hard_external) {
        /* A disconnected data cable or real charge signal is stronger evidence
         * than a high cell voltage.  Do not let a full battery immediately
         * reclassify itself as externally powered. */
        source->adc_external = false;
        source->high_suppressed = true;
        if (source->initialized) reset_window(source, now_ms, source->last_raw);
    }
    source->hard_external = hard_external;

    if (adc_valid) {
        if (!source->initialized) {
            source->initialized = true;
            reset_window(source, now_ms, adc_raw);
        } else {
            source->last_raw = adc_raw;
        }
    }

    if (hard_external) {
        if (adc_valid) reset_window(source, now_ms, adc_raw);
        return true;
    }
    if (!adc_valid || !source->initialized) return source->adc_external;

    if (source->adc_external) {
        if (adc_raw > source->peak_raw) source->peak_raw = adc_raw;
        if ((uint32_t)adc_raw + UNPLUG_DROP_RAW <= source->peak_raw) {
            source->adc_external = false;
            source->high_suppressed = true;
            reset_window(source, now_ms, adc_raw);
        }
        return source->adc_external;
    }

    if (now_ms < source->settle_until_ms) {
        reset_window(source, now_ms, adc_raw);
        return false;
    }

    if (source->high_suppressed && adc_raw + UNPLUG_DROP_RAW < FULL_CELL_RAW)
        source->high_suppressed = false;

    uint64_t elapsed_ms = now_ms - source->baseline_ms;
    source->window_delta = (int16_t)((int32_t)adc_raw - source->baseline_raw);
    if (!source->high_suppressed && adc_raw >= FULL_CELL_RAW &&
        elapsed_ms >= FULL_CONFIRM_MS) {
        source->adc_external = true;
        source->peak_raw = adc_raw;
        return true;
    }
    if (elapsed_ms >= TREND_WINDOW_MS) {
        if (source->window_delta >= CHARGE_RISE_RAW) {
            source->adc_external = true;
            source->high_suppressed = false;
            source->peak_raw = adc_raw;
            return true;
        }
        reset_window(source, now_ms, adc_raw);
    }
    return false;
}
