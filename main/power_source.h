#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Portable estimator for boards whose USB VBUS is not routed to a GPIO.
 * GPIO/USB-host signals are authoritative.  The battery ADC fallback detects
 * charging from a sustained rise, or a stable near-full cell, and clears the
 * inference after the cell falls from its observed peak. */
typedef struct {
    bool initialized;
    bool hard_external;
    bool adc_external;
    bool high_suppressed;
    uint16_t baseline_raw;
    uint16_t peak_raw;
    uint16_t last_raw;
    int16_t window_delta;
    uint64_t baseline_ms;
    uint64_t settle_until_ms;
} memo_power_source_t;

void memo_power_source_init(memo_power_source_t *source);
void memo_power_source_load_changed(memo_power_source_t *source, uint64_t now_ms);
bool memo_power_source_update(memo_power_source_t *source, uint64_t now_ms,
                              bool usb_host, bool charge_signal_valid,
                              bool charge_signal_high, bool adc_valid,
                              uint16_t adc_raw);
