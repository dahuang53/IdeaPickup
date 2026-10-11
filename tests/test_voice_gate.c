#include "voice_gate.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct { const char *phrase; voice_command_id_t id; } expected_phrase_t;
static void test_phrases(void) {
    static const expected_phrase_t expected[] = {
        {"kai shi lu yin", VOICE_CMD_RECORD_START},
        {"kai shi ji lu", VOICE_CMD_RECORD_START},
        {"lu yin", VOICE_CMD_RECORD_START},
        {"ji lu yi xia", VOICE_CMD_RECORD_START},
        {"bao cun lu yin", VOICE_CMD_RECORD_STOP},
        {"bao cun", VOICE_CMD_RECORD_STOP},
        {"jie shu lu yin", VOICE_CMD_RECORD_STOP},
        {"ting zhi lu yin", VOICE_CMD_RECORD_STOP},
        {"da kai mai ke feng", VOICE_CMD_USB_OPEN},
        {"kai qi mai ke feng", VOICE_CMD_USB_OPEN},
        {"mai ke feng mo shi", VOICE_CMD_USB_OPEN},
        {"guan bi mai ke feng", VOICE_CMD_USB_CLOSE},
        {"tui chu mai ke feng", VOICE_CMD_USB_CLOSE},
        {"jie shu mai ke feng", VOICE_CMD_USB_CLOSE},
        {"jing yin", VOICE_CMD_USB_MUTE},
        {"qu xiao jing yin", VOICE_CMD_USB_UNMUTE},
        {"hui fu mai ke feng", VOICE_CMD_USB_UNMUTE},
        {"qu xiao", VOICE_CMD_CANCEL},
    };
    size_t count=0;
    const voice_phrase_t *actual=voice_command_phrases(&count);
    assert(count==sizeof(expected)/sizeof(expected[0]));
    for(size_t i=0;i<count;i++) {
        assert(actual[i].id==expected[i].id);
        assert(strcmp(actual[i].pinyin,expected[i].phrase)==0);
        assert(strstr(actual[i].pinyin,"shou jian xiang")==NULL);
    }
}

static voice_action_t step(voice_gate_t *g,uint32_t gen,int64_t now,bool wake,int cmd,float confidence) {
    return voice_gate_step(g,gen,now,wake,cmd,confidence);
}
static void test_idle(void) {
    voice_gate_t g;
    voice_gate_reset(&g,VOICE_OFF,1);
    assert(step(&g,1,100,true,VOICE_CMD_RECORD_START,1)==VG_NONE);
    voice_gate_reset(&g,VOICE_IDLE,2);
    assert(step(&g,2,100,false,VOICE_CMD_RECORD_START,1)==VG_NONE);
    assert(step(&g,2,200,true,0,0)==VG_WINDOW);
    assert(g.deadline==200+VOICE_COMMAND_WINDOW_US && !g.pending);
    assert(step(&g,2,300,true,0,0)==VG_NONE);
    assert(step(&g,2,400,false,VOICE_CMD_RECORD_STOP,1)==VG_NONE);
    assert(step(&g,2,500,false,VOICE_CMD_USB_CLOSE,1)==VG_NONE);
    assert(step(&g,2,600,false,VOICE_CMD_RECORD_START,NAN)==VG_NONE);
    assert(step(&g,2,700,false,VOICE_CMD_RECORD_START,.59f)==VG_NONE);
    assert(step(&g,2,800,false,VOICE_CMD_RECORD_START,.60f)==VG_START);
    assert(g.pending && !g.deadline);
    assert(step(&g,2,900,false,VOICE_CMD_USB_OPEN,1)==VG_NONE);
    // A rejected queued command releases pending and allows another wake.
    voice_gate_ack(&g,1);
    assert(g.pending);
    voice_gate_ack(&g,2);
    assert(!g.pending);
    assert(step(&g,2,1000,false,VOICE_CMD_USB_OPEN,1)==VG_NONE);
    assert(step(&g,2,1100,true,0,0)==VG_WINDOW);
    assert(step(&g,2,1200,false,VOICE_CMD_USB_OPEN,1)==VG_USB_OPEN);
    voice_gate_ack(&g,2);
    assert(step(&g,2,1300,true,0,0)==VG_WINDOW);
    assert(step(&g,2,1400,false,VOICE_CMD_CANCEL,1)==VG_CANCEL);
    assert(step(&g,2,1500,false,VOICE_CMD_RECORD_START,1)==VG_NONE);
    assert(step(&g,2,1600,true,0,0)==VG_WINDOW);
    assert(step(&g,2,1600+VOICE_COMMAND_WINDOW_US,false,VOICE_CMD_RECORD_START,1)==VG_TIMEOUT);
    assert(step(&g,2,1601+VOICE_COMMAND_WINDOW_US,false,VOICE_CMD_RECORD_START,1)==VG_NONE);
}
static void test_recording(void) {
    voice_gate_t g;
    voice_gate_reset(&g,VOICE_RECORDING,3);
    assert(step(&g,3,100,false,VOICE_CMD_RECORD_STOP,1)==VG_NONE);
    assert(step(&g,3,200,true,0,0)==VG_WINDOW);
    assert(step(&g,3,300,false,VOICE_CMD_RECORD_START,1)==VG_NONE);
    assert(step(&g,3,400,false,VOICE_CMD_USB_OPEN,1)==VG_NONE);
    assert(step(&g,3,500,false,VOICE_CMD_USB_MUTE,1)==VG_NONE);
    assert(step(&g,3,600,false,VOICE_CMD_CANCEL,1)==VG_CANCEL);
    assert(step(&g,3,700,true,0,0)==VG_WINDOW);
    assert(step(&g,3,800,false,VOICE_CMD_RECORD_STOP,1)==VG_STOP);
    assert(step(&g,3,900,true,0,0)==VG_NONE);
    voice_gate_ack(&g,3);
    assert(step(&g,3,1000,true,0,0)==VG_WINDOW);
    assert(step(&g,3,1100+VOICE_COMMAND_WINDOW_US,false,0,0)==VG_TIMEOUT);
}
static void test_usb(void) {
    voice_gate_t g;
    voice_gate_reset(&g,VOICE_USB_MIC,4);
    assert(step(&g,4,100,false,VOICE_CMD_USB_CLOSE,1)==VG_NONE);
    assert(step(&g,4,200,true,0,0)==VG_WINDOW);
    assert(step(&g,4,300,false,VOICE_CMD_RECORD_START,1)==VG_NONE);
    assert(step(&g,4,400,false,VOICE_CMD_RECORD_STOP,1)==VG_NONE);
    assert(step(&g,4,500,false,VOICE_CMD_USB_OPEN,1)==VG_NONE);
    assert(step(&g,4,600,false,VOICE_CMD_USB_MUTE,1)==VG_USB_MUTE);
    voice_gate_ack(&g,4);
    assert(step(&g,4,700,true,0,0)==VG_WINDOW);
    assert(step(&g,4,800,false,VOICE_CMD_USB_UNMUTE,1)==VG_USB_UNMUTE);
    voice_gate_ack(&g,4);
    assert(step(&g,4,900,true,0,0)==VG_WINDOW);
    assert(step(&g,4,1000,false,VOICE_CMD_USB_CLOSE,1)==VG_USB_CLOSE);
}
static void test_generation(void) {
    voice_gate_t g;
    voice_gate_reset(&g,VOICE_IDLE,9);
    assert(step(&g,9,100,true,0,0)==VG_WINDOW);
    assert(step(&g,9,200,false,VOICE_CMD_RECORD_START,1)==VG_START);
    voice_gate_reset(&g,VOICE_RECORDING,10);
    voice_gate_ack(&g,9);
    assert(!g.pending);
    assert(step(&g,9,300,true,VOICE_CMD_RECORD_STOP,1)==VG_NONE);
    assert(step(&g,10,400,false,VOICE_CMD_RECORD_STOP,1)==VG_NONE);
    assert(step(&g,10,500,true,0,0)==VG_WINDOW);
    assert(step(&g,9,600,false,VOICE_CMD_RECORD_STOP,1)==VG_NONE);
    assert(step(&g,10,700,false,VOICE_CMD_RECORD_STOP,1)==VG_STOP);
}
int main(void) {
    assert(VOICE_WAKE_THRESHOLD>=.4f && VOICE_WAKE_THRESHOLD<=.9999f);
    test_phrases();test_idle();test_recording();test_usb();test_generation();
    puts("VOICE_GATE PASS: phrases, intents, mutual exclusion, confidence, timeout, cancellation, acknowledgements, generations");
}
