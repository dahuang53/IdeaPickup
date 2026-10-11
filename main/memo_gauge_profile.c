/* BQ27220 RAM protocol adapted from Espressif's CC0-1.0 driver at
 * esp-iot-solution 6958385313b0e4fc1f3de259b1d677fca7d7d236.
 * Addresses, endian order and commands checked against TI SLUUBD4A ch. 6.
 * Only the two capacity fields in volatile RAM are changed. */
#include "memo_gauge_profile.h"
#include <string.h>

static int word(const memo_gauge_bus_t *b, uint8_t reg, uint16_t *value)
{
    uint8_t v[2];
    if (b->read(b->context, reg, v, 2)) return -1;
    *value = (uint16_t)v[0] | ((uint16_t)v[1] << 8);
    return 0;
}
static int control(const memo_gauge_bus_t *b, uint16_t command)
{
    uint8_t v[] = {(uint8_t)command, (uint8_t)(command >> 8)};
    return b->write(b->context, 0x00, v, sizeof(v));
}
static int wait_cfg(const memo_gauge_bus_t *b, bool enabled)
{
    for (unsigned i = 0; i < 50; ++i) {
        uint16_t status;
        if (word(b, 0x3a, &status)) return -1;
        if (((status & 0x0400) != 0) == enabled) return 0;
        b->delay_ms(b->context, 20);
    }
    return -1;
}
static int read_capacity_ram(const memo_gauge_bus_t *b, uint8_t data[4])
{
    const uint8_t address[] = {0x9d, 0x92};
    if (b->write(b->context, 0x3e, address, sizeof(address))) return -1;
    for (unsigned i = 0; i < 20; ++i) {
        uint8_t frame[6];
        b->delay_ms(b->context, 1);
        if (b->read(b->context, 0x3e, frame, sizeof(frame))) return -1;
        if (frame[0] == address[0] && frame[1] == address[1]) {
            memcpy(data, frame + 2, 4);
            return 0;
        }
    }
    return -1;
}
static int write_capacity_ram(const memo_gauge_bus_t *b, const uint8_t data[4])
{
    uint8_t frame[6] = {0x9d, 0x92};
    memcpy(frame + 2, data, 4);
    uint8_t sum = 0;
    for (unsigned i = 0; i < sizeof(frame); ++i) sum += frame[i];
    uint8_t commit[] = {(uint8_t)(255U - sum), 8};
    if (b->write(b->context, 0x3e, frame, sizeof(frame))) return -1;
    b->delay_ms(b->context, 10);
    if (b->write(b->context, 0x60, commit, sizeof(commit))) return -1;
    b->delay_ms(b->context, 10);
    uint8_t actual[4];
    return read_capacity_ram(b, actual) || memcmp(actual, data, 4) ? -1 : 0;
}
static int seal(const memo_gauge_bus_t *b)
{
    if (control(b, 0x0030)) return -1;
    b->delay_ms(b->context, 1000);
    uint16_t status;
    return word(b, 0x3a, &status) || (status & 6U) != 6U ? -1 : 0;
}
static int full_access(const memo_gauge_bus_t *b)
{
    uint16_t status;
    if (word(b, 0x3a, &status)) return -1;
    if ((status & 6U) == 2U) return 0;
    if ((status & 6U) == 6U) {
        if (control(b, 0x0414)) return -1;
        b->delay_ms(b->context, 10);
        if (control(b, 0x3672)) return -1;
        b->delay_ms(b->context, 10);
        if (word(b, 0x3a, &status) || (status & 6U) != 4U) return -1;
    }
    if (control(b, 0xffff)) return -1;
    b->delay_ms(b->context, 10);
    if (control(b, 0xffff)) return -1;
    b->delay_ms(b->context, 1000);
    return word(b, 0x3a, &status) || (status & 6U) != 2U ? -1 : 0;
}
bool memo_gauge_capacity_valid(uint16_t design, uint16_t full)
{
    /* Keep a learned FCC on subsequent starts; do not reset it to 700 on
     * every MCU reboot. Reject the previous 3 Ah/default capacity scale. */
    return design == MEMO_GAUGE_CAPACITY_MAH && full >= 100 && full <= 1050;
}
int memo_gauge_configure(const memo_gauge_bus_t *b, memo_gauge_profile_result_t *r)
{
    if (!b || !b->read || !b->write || !b->delay_ms || !r) return -1;
    memset(r, 0, sizeof(*r));
    if (word(b, 0x3c, &r->previous_design_mah) || word(b, 0x12, &r->previous_full_mah)) return -1;
    uint16_t device, status;
    if (word(b, 0x3a, &status)) return -1;
    if (memo_gauge_capacity_valid(r->previous_design_mah, r->previous_full_mah) && !(status & 0x0400)) return 0;
    /* Require the actual BQ27220 device ID before any configuration writes. */
    if (control(b, 0x0001)) return -1;
    b->delay_ms(b->context, 20);
    if (word(b, 0x40, &device) || device != 0x0220 || word(b, 0x3a, &status)) return -1;
    bool access_touched = false, cfg_requested = false, write_attempted = false;
    uint8_t backup[4] = {0};
    access_touched = true;
    if (full_access(b)) goto fail;
    if (read_capacity_ram(b, backup)) goto fail;
    r->previous_ram_full_mah = (uint16_t)backup[0] * 256U + backup[1];
    r->previous_ram_design_mah = (uint16_t)backup[2] * 256U + backup[3];
    /* 0x929D/F are big-endian RAM fields; standard command words are LE. */
    cfg_requested = true;
    if (control(b, 0x0090)) goto fail;
    b->delay_ms(b->context, 2000);
    if (wait_cfg(b, true)) goto fail;
    const uint8_t capacity[] = {0x02, 0xbc, 0x02, 0xbc};
    write_attempted = true;
    if (write_capacity_ram(b, capacity)) goto fail;
    /* Use Espressif's single exit-with-reinit sequence. On the real V1.2,
     * NO_REINIT leaves the standard FCC stale and seals access. Only apply
     * this once for an invalid profile; ordinary MCU boots keep learned FCC. */
    if (control(b, 0x0091)) goto fail;
    r->reinitialized = true;
    b->delay_ms(b->context, 2000);
    uint16_t full, design;
    if (wait_cfg(b, false) || word(b, 0x12, &full) || word(b, 0x3c, &design)) goto fail;
    if (!memo_gauge_capacity_valid(design, full)) goto fail;
    if (seal(b)) goto fail;
    r->changed = true;
    return 0;
fail:
    if (write_attempted) {
        r->rollback_attempted = true;
        /* Backup was read before any mutation. Best effort rollback even if
         * a failed transaction might already have reached the chip. */
        if (!full_access(b) && !control(b, 0x0090)) {
            b->delay_ms(b->context, 2000);
            r->rollback_verified = !wait_cfg(b, true) && !write_capacity_ram(b, backup);
        }
    }
    if (cfg_requested) {
        (void)control(b, 0x0092);
        b->delay_ms(b->context, 2000);
        (void)wait_cfg(b, false);
    }
    if (access_touched) (void)seal(b);
    return -1;
}
uint16_t memo_gauge_protection_soc(uint16_t soc, bool calibrated, uint16_t mv)
{
    uint16_t guard = calibrated && soc <= 100 ? soc : 100;
    /* Software reserve for the 3.3 V system, not a measured discharge curve.
     * The caller still applies 10 s / 7 s debounce and external-power veto. */
    if (mv >= 2500 && mv <= 3300 && guard > 2) guard = 2;
    else if (mv >= 2500 && mv <= 3450 && guard > 5) guard = 5;
    return guard;
}
