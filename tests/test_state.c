#include "app.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    const memo_state_t available[] = {MEMO_IDLE, MEMO_SAVED, MEMO_ERROR};
    for (unsigned i=0;i<3;i++) {
        assert(memo_can_command(available[i], CMD_START));
        assert(memo_can_command(available[i], CMD_PLAY));
        assert(!memo_can_command(available[i], CMD_STOP));
        assert(memo_can_command(available[i], CMD_USB_OPEN));
        assert(memo_can_command(available[i], CMD_BOOTLOADER));
        assert(memo_can_command(available[i], CMD_VOICE_WAKE));
        assert(!memo_can_command(available[i], CMD_USB_UNMUTE));
    }
    assert(memo_can_command(MEMO_RECORDING, CMD_STOP));
    assert(memo_can_command(MEMO_PLAYING, CMD_STOP));
    assert(memo_can_command(MEMO_PAUSED, CMD_STOP));
    assert(memo_can_command(MEMO_PLAYING, CMD_PAUSE));
    assert(!memo_can_command(MEMO_PAUSED, CMD_PAUSE));
    assert(memo_can_command(MEMO_PAUSED, CMD_RESUME));
    assert(!memo_can_command(MEMO_PLAYING, CMD_RESUME));
    assert(!memo_can_command(MEMO_IDLE, CMD_RESUME));
    assert(!memo_can_command(MEMO_PAUSED, CMD_START));
    assert(!memo_can_command(MEMO_PAUSED, CMD_PLAY));
    assert(!memo_can_command(MEMO_PAUSED, CMD_PROVISION));
    assert(memo_can_command(MEMO_SAVED, CMD_IDLE));
    assert(!memo_can_command(MEMO_IDLE, CMD_IDLE));
    for (int c=CMD_START;c<=CMD_IDLE;c++) {
        if (c!=CMD_STOP) {
            assert(!memo_can_command(MEMO_RECORDING,c));
            assert(!memo_can_command(MEMO_PLAYING,c));
        }
        assert(!memo_can_command(MEMO_SAVING,c));
    }
    assert(!memo_can_command(MEMO_IDLE,99));
    for (int c=CMD_START;c<=CMD_BOOTLOADER;c++) {
        bool allowed = c==CMD_USB_CLOSE || c==CMD_USB_MUTE || c==CMD_USB_UNMUTE ||
                       c==CMD_VOICE_WAKE || c==CMD_VOICE_CANCEL;
        assert(memo_can_command(MEMO_USB_MIC,c)==allowed);
    }
    assert(!memo_can_command(MEMO_RECORDING,CMD_USB_OPEN));
    assert(!memo_can_command(MEMO_PLAYING,CMD_USB_OPEN));
    assert(!memo_can_command(MEMO_PAUSED,CMD_USB_OPEN));
    assert(!memo_can_command(MEMO_SAVING,CMD_USB_OPEN));
    assert(!memo_can_command(MEMO_RECORDING,CMD_BOOTLOADER));
    assert(!memo_can_command(MEMO_PLAYING,CMD_BOOTLOADER));
    assert(!memo_can_command(MEMO_PAUSED,CMD_BOOTLOADER));
    assert(!memo_can_command(MEMO_SAVING,CMD_BOOTLOADER));
    puts("PASS: playback pause/resume/stop gates and busy-state command isolation");
}
