#include "app.h"
bool memo_can_command(memo_state_t state, memo_cmd_t command)
{
    bool available = state == MEMO_IDLE || state == MEMO_SAVED || state == MEMO_ERROR;
    if (command == CMD_BOOTLOADER || command == CMD_SYNC_MODE) return available;
    if (command == CMD_VOICE_WAKE || command == CMD_VOICE_CANCEL)
        return available || state == MEMO_RECORDING || state == MEMO_USB_MIC;
    if (command == CMD_USB_OPEN) return available;
    if (command == CMD_USB_CLOSE || command == CMD_USB_MUTE || command == CMD_USB_UNMUTE)
        return state == MEMO_USB_MIC;
    if (state == MEMO_USB_MIC) return false;
    if (command == CMD_IDLE) return state == MEMO_SAVED;
    if (command == CMD_PAUSE) return state == MEMO_PLAYING;
    if (command == CMD_RESUME) return state == MEMO_PAUSED;
    if (command == CMD_PROVISION) return state != MEMO_RECORDING && state != MEMO_SAVING &&
                                         state != MEMO_PLAYING && state != MEMO_PAUSED;
    if (command == CMD_STOP) return state == MEMO_RECORDING || state == MEMO_PLAYING ||
                                    state == MEMO_PAUSED;
    if (state == MEMO_RECORDING || state == MEMO_SAVING || state == MEMO_PLAYING ||
        state == MEMO_PAUSED) return false;
    return command >= CMD_START && command <= CMD_REBOOT;
}
