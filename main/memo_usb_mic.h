#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool available;
    bool connected;
    bool suspended;
    bool streaming;
    bool enabled;
    bool local_muted;
    bool host_muted;
    uint32_t dropped_samples;
    uint32_t underrun_samples;
    uint32_t cdc_tx_dropped_bytes;
    uint32_t bus_frame;
    uint32_t bus_age_ms;
    uint32_t bus_watchdog_losses;
} memo_usb_status_t;

esp_err_t memo_usb_init(void);
void memo_usb_get_status(memo_usb_status_t *out);
bool memo_usb_open(void);
void memo_usb_close(void);
void memo_usb_set_muted(bool muted);
void memo_usb_submit(const int16_t *samples, size_t count);
/* Narrowly enable CDC backpressure while exporting a WAV from idle mode. */
void memo_usb_set_bulk_output(bool enabled);
/* Explicit maintenance command only. On success this restarts into ROM download mode. */
esp_err_t memo_usb_enter_bootloader(void);

#ifdef __cplusplus
}
#endif
