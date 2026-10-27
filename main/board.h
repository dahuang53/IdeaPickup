#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOARD_AUDIO_RATE 16000
#define BOARD_AUDIO_MAX_SAMPLES 1024
#define BOARD_SD_PATH "/sdcard"

/* All audio calls belong to the one application audio worker. The display and
 * SD share a power rail; this module owns that rail and the single I2C bus. */
const char *board_name(void);
/* Stable prefix for cloud device identity; changing it would require pairing
 * again, so each supported product family owns its own value. */
const char *board_device_id_prefix(void);
esp_err_t board_init_display(void);
esp_err_t board_sd_mount(void);
/* True only after an initialization attempt got no card responses and timed
 * out. A filesystem/CRC/IO error with a responding card never enables fallback.
 * Boards without a mechanical detect pin cannot distinguish an electrically
 * dead card from an empty slot. */
bool board_sd_absent(void);
/* One real CMD13 before opening an SD recording; free-space may be cached. */
esp_err_t board_sd_check(void);
esp_err_t board_sd_verify_session(void);
/* Call only while no recording/playback/file operation is active. */
esp_err_t board_sd_unmount(void);
esp_err_t board_audio_init(void);
esp_err_t board_record_open(void);
/* Discard input queued before a newly visible recording starts. */
esp_err_t board_record_resync(void);
esp_err_t board_record_read(int16_t *mono, size_t samples);
esp_err_t board_record_close(void);
uint32_t board_record_overflows(void);
/* Lightweight RX DMA ISR counters for diagnosing a stalled audio read. Times
 * are the low 32 bits of esp_timer_get_time(), suitable for short deltas. */
typedef struct {
    uint32_t eof_count;
    uint32_t overflow_count;
    uint32_t last_eof_us;
    uint32_t last_overflow_us;
    int32_t irq_core;
} board_rx_diagnostics_t;
void board_record_rx_diagnostics(board_rx_diagnostics_t *out);
esp_err_t board_play_open(void);
esp_err_t board_play_write(const int16_t *mono, size_t samples);
esp_err_t board_play_close(void);

typedef enum {
    BOARD_SLEEP_WAKE_BUTTON,
    BOARD_SLEEP_WAKE_EXTERNAL_POWER,
    BOARD_SLEEP_WAKE_TIMER,
    BOARD_SLEEP_WAKE_OTHER,
} board_sleep_wake_t;

/* Power policy capabilities. The original 32 MB VoCat keeps its accepted
 * behavior; the 16 MB VoCat uses its BQ27220 gauge and a conservative policy. */
bool board_power_policy_supported(void);
esp_err_t board_power_monitor_init(void);
bool board_external_power_present(void);
/* Board-specific battery raw value: Spotpear ADC counts or VoCat BQ27220 SOC.
 * Poll interval affects sensor transactions, never SD or audio sampling. */
bool board_battery_raw(uint16_t *raw);
/* Protection input on the same board-specific scale. VoCat additionally uses
 * measured low voltage as a reserve, without changing the displayed SOC. */
bool board_battery_protection_raw(uint16_t *raw);
/* Read-only standard fuel-gauge registers for field diagnosis. No profile,
 * calibration, OTP, or battery configuration writes are performed. */
typedef struct {
    uint16_t soc_percent, voltage_mv, remaining_mah, full_mah, design_mah;
    uint16_t time_to_empty_min, cycle_count, flags;
    int16_t current_ma, average_ma;
    uint16_t operation_status, previous_full_mah, previous_design_mah;
    uint16_t previous_ram_full_mah, previous_ram_design_mah;
    bool profile_ready, profile_changed, profile_reinitialized, rollback_attempted, rollback_verified;
} board_battery_diagnostics_t;
esp_err_t board_battery_diagnostics(board_battery_diagnostics_t *out);
bool board_battery_rising(void);
void board_battery_set_poll_ms(uint32_t interval_ms);
esp_err_t board_display_set_brightness(unsigned percent);
esp_err_t board_display_set_enabled(bool enabled);
board_sleep_wake_t board_light_sleep(uint64_t timeout_us);
void board_power_off(void);

#ifdef __cplusplus
}
#endif
