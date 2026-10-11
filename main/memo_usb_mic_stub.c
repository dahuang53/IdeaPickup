#include "memo_usb_mic.h"
#include <string.h>

esp_err_t memo_usb_init(void) { return ESP_ERR_NOT_SUPPORTED; }
void memo_usb_get_status(memo_usb_status_t *out) { if (out) memset(out, 0, sizeof(*out)); }
bool memo_usb_open(void) { return false; }
void memo_usb_close(void) {}
void memo_usb_set_muted(bool muted) { (void)muted; }
void memo_usb_submit(const int16_t *samples, size_t count) { (void)samples; (void)count; }
void memo_usb_set_bulk_output(bool enabled) { (void)enabled; }
esp_err_t memo_usb_enter_bootloader(void) { return ESP_ERR_NOT_SUPPORTED; }
