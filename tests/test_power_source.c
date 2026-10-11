#include "power_source.h"

#include <assert.h>
#include <stdio.h>

static bool sample(memo_power_source_t *source, uint64_t seconds, bool usb,
                   bool charge_valid, bool charge_high, unsigned raw)
{
    return memo_power_source_update(source, seconds * 1000ULL, usb,
                                    charge_valid, charge_high, true,
                                    (uint16_t)raw);
}

int main(void)
{
    memo_power_source_t source;

    memo_power_source_init(&source);
    assert(sample(&source, 0, true, false, false, 2460));
    assert(!sample(&source, 1, false, false, false, 2459));
    assert(source.high_suppressed);

    memo_power_source_init(&source);
    assert(!sample(&source, 0, false, false, false, 2200));
    assert(!sample(&source, 44, false, false, false, 2207));
    assert(sample(&source, 45, false, false, false, 2208));
    assert(source.adc_external);
    assert(sample(&source, 90, false, false, false, 2250));
    assert(!sample(&source, 91, false, false, false, 2238));

    memo_power_source_init(&source);
    assert(!sample(&source, 0, false, false, false, 2430));
    assert(!sample(&source, 14, false, false, false, 2430));
    assert(sample(&source, 15, false, false, false, 2430));
    assert(!sample(&source, 20, false, false, false, 2418));
    assert(source.high_suppressed);
    assert(!sample(&source, 80, false, false, false, 2416));

    memo_power_source_init(&source);
    assert(sample(&source, 0, false, true, true, 2100));
    assert(!sample(&source, 1, false, true, false, 2100));

    memo_power_source_init(&source);
    assert(!sample(&source, 0, false, false, false, 2300));
    assert(!sample(&source, 45, false, false, false, 2304));
    assert(!sample(&source, 90, false, false, false, 2298));

    memo_power_source_init(&source);
    assert(!sample(&source, 0, false, false, false, 2200));
    memo_power_source_load_changed(&source, 10 * 1000ULL);
    assert(!sample(&source, 20, false, false, false, 2220));
    assert(!sample(&source, 69, false, false, false, 2221));
    assert(!sample(&source, 70, false, false, false, 2221));
    assert(!sample(&source, 115, false, false, false, 2225));

    puts("PASS: USB/charge signals and battery-ADC power fallback");
}
