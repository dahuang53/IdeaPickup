#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ui_layout.h"
#define MEMO_PAGE_SIZE (MEMO_UI_COMPACT ? 3 : 6)
/* Keep existing numeric states stable for serial diagnostics. */
typedef enum { MEMO_IDLE, MEMO_RECORDING, MEMO_SAVING, MEMO_SAVED, MEMO_PLAYING, MEMO_ERROR,
               MEMO_PAUSED, MEMO_USB_MIC } memo_state_t;
typedef enum {
    CMD_START, CMD_STOP, CMD_PLAY, CMD_NEXT, CMD_PREV, CMD_REFRESH,
    CMD_FAULT_WRITE, CMD_FAULT_SPACE, CMD_DUMP, CMD_PARTIAL, CMD_RECOVER,
    CMD_DISCARD, CMD_REBOOT, CMD_IDLE, CMD_PROVISION,
    CMD_RECOVER_PENDING, CMD_POWER_SHUTDOWN, CMD_PAUSE, CMD_RESUME,
    CMD_USB_OPEN, CMD_USB_CLOSE, CMD_USB_MUTE, CMD_USB_UNMUTE,
    CMD_VOICE_WAKE, CMD_VOICE_CANCEL, CMD_BOOTLOADER
} memo_cmd_t;
typedef struct { uint32_t id; uint32_t duration_ms; uint32_t file_bytes; bool cardless; } memo_item_t;
typedef struct {
    memo_state_t state;
    bool sd_ready;
    bool cardless;
    bool cardless_ready;
    bool recovering;
    bool voice_ready, voice_window;
    bool usb_available, usb_connected, usb_suspended, usb_streaming;
    bool usb_muted, usb_local_muted;
    uint32_t voice_wake_count;
    uint32_t voice_drops;
    uint32_t elapsed_ms;
    unsigned level;
    uint32_t active_id;
    size_t count, page, page_count;
    unsigned revision;
    char error[96];
    char notice[96];
    memo_item_t items[MEMO_PAGE_SIZE];
    size_t item_count;
} memo_snapshot_t;
// Thread-safe; touch and serial use the same worker/business logic.
bool app_command(memo_cmd_t command, uint32_t arg, const char *source);
void app_snapshot(memo_snapshot_t *out);
bool memo_can_command(memo_state_t state, memo_cmd_t command);
bool app_power_is_busy(void);
bool app_power_set_listening(bool enabled, uint32_t timeout_ms);
/* Close every SD/audio owner for critical-battery shutdown. The network lock
 * remains held until shutdown or app_power_cancel_shutdown(). */
bool app_power_prepare_shutdown(uint32_t timeout_ms);
void app_power_cancel_shutdown(void);
/* Retry a boot recovery that was deferred due to low battery. */
void app_power_recover_deferred(void);
void ui_init(void);
void ui_power_show_sleep(void);
void ui_power_show_awake(void);
