#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Narrow, transport-independent BQ27220 RAM configuration. No OTP access. */
typedef struct {
    void *context;
    int (*read)(void *, uint8_t, uint8_t *, size_t);
    int (*write)(void *, uint8_t, const uint8_t *, size_t);
    void (*delay_ms)(void *, unsigned);
} memo_gauge_bus_t;
typedef struct {
    uint16_t previous_full_mah, previous_design_mah;
    uint16_t previous_ram_full_mah, previous_ram_design_mah;
    bool changed, reinitialized, rollback_attempted, rollback_verified;
} memo_gauge_profile_result_t;

enum { MEMO_GAUGE_CAPACITY_MAH = 700 };
bool memo_gauge_capacity_valid(uint16_t design, uint16_t full);
/* 0: verified profile; -1: IO/status/verification failure. On failure the
 * caller must not present an unverified SOC as an accurate percentage. */
int memo_gauge_configure(const memo_gauge_bus_t *, memo_gauge_profile_result_t *);
/* Protection only: voltage reserve does not replace the displayed SOC. */
uint16_t memo_gauge_protection_soc(uint16_t soc, bool calibrated, uint16_t mv);
