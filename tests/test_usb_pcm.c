#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "memo_usb_gate.h"
#include "memo_usb_pcm.h"
#include "memo_usb_bus_watch.h"

int main(void)
{
    memo_usb_bus_watch_t bus = {0};
    assert(!memo_usb_bus_watch_update(&bus, true, 16380, 1000));
    assert(!memo_usb_bus_watch_update(&bus, true, 16380, 21000));
    assert(memo_usb_bus_watch_update(&bus, true, 16, 41000)); // SOF wrap
    assert(memo_usb_bus_watch_update(&bus, true, 16, 540999));
    assert(!memo_usb_bus_watch_update(&bus, true, 16, 541000)); // stale mount
    assert(!memo_usb_bus_watch_update(&bus, true, 16, 6041000));
    assert(memo_usb_bus_watch_update(&bus, true, 17, 6061000));
    assert(!memo_usb_bus_watch_update(&bus, false, 18, 6081000));
    assert(!memo_usb_bus_watch_update(&bus, true, 18, 6101000));
    assert(memo_usb_bus_watch_update(&bus, true, 19, 6121000));
    memo_usb_pcm_t fifo = {0};
    int16_t src[MEMO_USB_PCM_CAPACITY + 5];
    for (size_t i = 0; i < MEMO_USB_PCM_CAPACITY + 5; ++i) src[i] = (int16_t)(i + 1);

    // Wait for host alternate setting while keeping the user session open.
    assert(!memo_usb_gate_should_stream(true, false, true, false, false));
    assert(memo_usb_gate_should_stream(true, true, true, false, false));
    assert(!memo_usb_gate_should_stream(true, true, true, true, false));
    assert(!memo_usb_gate_should_stream(true, true, true, false, true));
    assert(!memo_usb_gate_should_stream(false, true, true, false, false));
    assert(!memo_usb_gate_should_stream(true, true, false, false, false));

    memo_usb_pcm_push(&fifo, src, MEMO_USB_PCM_CAPACITY + 5);
    assert(fifo.count == MEMO_USB_PCM_CAPACITY);
    assert(fifo.dropped_samples == 5);
    int16_t out[MEMO_USB_PCM_CAPACITY + 8];
    memo_usb_pcm_pop(&fifo, out, 512, true);
    for (int i = 0; i < 512; ++i) assert(out[i] == i + 1);
    memo_usb_pcm_push(&fifo, src, 512);
    memo_usb_pcm_pop(&fifo, out, MEMO_USB_PCM_CAPACITY, true);
    for (size_t i = 0; i < MEMO_USB_PCM_CAPACITY - 512; ++i)
        assert(out[i] == (int16_t)(i + 513));
    for (size_t i = MEMO_USB_PCM_CAPACITY - 512; i < MEMO_USB_PCM_CAPACITY; ++i)
        assert(out[i] == (int16_t)(i - (MEMO_USB_PCM_CAPACITY - 512) + 1));
    assert(fifo.count == 0);

    memo_usb_pcm_push(&fifo, src, 512);
    memo_usb_pcm_clear(&fifo);  // mute/disconnect must erase old speech
    assert(fifo.count == 0);
    memo_usb_pcm_pop(&fifo, out, 16, false);
    for (int i = 0; i < 16; ++i) assert(out[i] == 0);
    memo_usb_pcm_pop(&fifo, out, 16, true);
    for (int i = 0; i < 16; ++i) assert(out[i] == 0);
    assert(fifo.underrun_samples == 16);
    puts("usb pcm/gate PASS");
}
