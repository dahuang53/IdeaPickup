#include "voice_gate.h"

// The phrases are registered once with the Chinese MultiNet model. Keeping
// IDs and spellings here also lets the host test the complete intent map.
static const voice_phrase_t phrases[] = {
    {VOICE_CMD_RECORD_START, "kai shi lu yin"},
    {VOICE_CMD_RECORD_START, "kai shi ji lu"},
    {VOICE_CMD_RECORD_START, "lu yin"},
    {VOICE_CMD_RECORD_START, "ji lu yi xia"},
    {VOICE_CMD_RECORD_STOP, "bao cun lu yin"},
    {VOICE_CMD_RECORD_STOP, "bao cun"},
    {VOICE_CMD_RECORD_STOP, "jie shu lu yin"},
    {VOICE_CMD_RECORD_STOP, "ting zhi lu yin"},
    {VOICE_CMD_USB_OPEN, "da kai mai ke feng"},
    {VOICE_CMD_USB_OPEN, "kai qi mai ke feng"},
    {VOICE_CMD_USB_OPEN, "mai ke feng mo shi"},
    {VOICE_CMD_USB_CLOSE, "guan bi mai ke feng"},
    {VOICE_CMD_USB_CLOSE, "tui chu mai ke feng"},
    {VOICE_CMD_USB_CLOSE, "jie shu mai ke feng"},
    {VOICE_CMD_USB_MUTE, "jing yin"},
    {VOICE_CMD_USB_UNMUTE, "qu xiao jing yin"},
    {VOICE_CMD_USB_UNMUTE, "hui fu mai ke feng"},
    {VOICE_CMD_CANCEL, "qu xiao"},
};

const voice_phrase_t *voice_command_phrases(size_t *count) {
    if(count)*count=sizeof(phrases)/sizeof(phrases[0]);
    return phrases;
}

void voice_gate_reset(voice_gate_t *g,voice_mode_t mode,uint32_t generation) {
    *g=(voice_gate_t){.mode=mode,.generation=generation};
}

void voice_gate_ack(voice_gate_t *g,uint32_t generation) {
    if(generation==g->generation)g->pending=false;
}

voice_action_t voice_gate_step(voice_gate_t *g,uint32_t generation,int64_t now,
                              bool wake,int command_id,float probability) {
    if(generation!=g->generation || g->mode==VOICE_OFF || g->pending)return VG_NONE;
    if(g->deadline && now>=g->deadline) {
        g->deadline=0;
        return VG_TIMEOUT;
    }
    if(wake && !g->deadline) {
        g->deadline=now+VOICE_COMMAND_WINDOW_US;
        return VG_WINDOW;
    }
    if(!g->deadline || probability<VOICE_COMMAND_THRESHOLD ||
       !(probability>=0.0f && probability<=1.0f))return VG_NONE;

    voice_action_t action=VG_NONE;
    if(command_id==VOICE_CMD_CANCEL)action=VG_CANCEL;
    else if(g->mode==VOICE_IDLE) {
        if(command_id==VOICE_CMD_RECORD_START)action=VG_START;
        else if(command_id==VOICE_CMD_USB_OPEN)action=VG_USB_OPEN;
    } else if(g->mode==VOICE_RECORDING) {
        if(command_id==VOICE_CMD_RECORD_STOP)action=VG_STOP;
    } else if(g->mode==VOICE_USB_MIC) {
        if(command_id==VOICE_CMD_USB_CLOSE)action=VG_USB_CLOSE;
        else if(command_id==VOICE_CMD_USB_MUTE)action=VG_USB_MUTE;
        else if(command_id==VOICE_CMD_USB_UNMUTE)action=VG_USB_UNMUTE;
    }
    if(action!=VG_NONE) {
        g->deadline=0;
        if(action!=VG_CANCEL)g->pending=true;
    }
    return action;
}
