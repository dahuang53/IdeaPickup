#pragma once
#include "voice.h"
// Keep the model's official 90% operating point. Natural-speech calibration
// found that weak positive and non-wake scores overlap below this threshold.
#define VOICE_WAKE_THRESHOLD 0.632f
#define VOICE_COMMAND_THRESHOLD 0.60f
#define VOICE_COMMAND_WINDOW_US 8000000LL
// One semantic ID can have several MultiNet phrases. Zero means no command.
typedef enum {
    VOICE_CMD_RECORD_STOP = 1, /* Preserve the existing diagnostic command ID. */
    VOICE_CMD_RECORD_START,
    VOICE_CMD_USB_OPEN,
    VOICE_CMD_USB_CLOSE,
    VOICE_CMD_USB_MUTE,
    VOICE_CMD_USB_UNMUTE,
    VOICE_CMD_CANCEL,
} voice_command_id_t;
typedef struct { voice_command_id_t id; const char *pinyin; } voice_phrase_t;
const voice_phrase_t *voice_command_phrases(size_t *count);
typedef enum {
    VG_NONE, VG_WINDOW, VG_START, VG_STOP, VG_USB_OPEN, VG_USB_CLOSE,
    VG_USB_MUTE, VG_USB_UNMUTE, VG_CANCEL, VG_TIMEOUT
} voice_action_t;
typedef struct {
    voice_mode_t mode;
    uint32_t generation;
    int64_t deadline;
    bool pending;
} voice_gate_t;
void voice_gate_reset(voice_gate_t *g, voice_mode_t mode, uint32_t generation);
void voice_gate_ack(voice_gate_t *g, uint32_t generation);
voice_action_t voice_gate_step(voice_gate_t *g, uint32_t generation, int64_t now,
                              bool wake, int command_id, float probability);
