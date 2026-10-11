#include "memo_usb_pcm.h"
#include <string.h>

void memo_usb_pcm_clear(memo_usb_pcm_t *fifo)
{
    fifo->head = fifo->tail = fifo->count = 0;
    memset(fifo->samples, 0, sizeof(fifo->samples));
}

void memo_usb_pcm_push(memo_usb_pcm_t *fifo, const int16_t *src, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (fifo->count == MEMO_USB_PCM_CAPACITY) {
            fifo->dropped_samples += (uint32_t)(count - i);
            return;
        }
        fifo->samples[fifo->head] = src[i];
        fifo->head = (fifo->head + 1) % MEMO_USB_PCM_CAPACITY;
        ++fifo->count;
    }
}

void memo_usb_pcm_pop(memo_usb_pcm_t *fifo, int16_t *dst, size_t count, int streaming)
{
    for (size_t i = 0; i < count; ++i) {
        if (streaming && fifo->count) {
            dst[i] = fifo->samples[fifo->tail];
            fifo->tail = (fifo->tail + 1) % MEMO_USB_PCM_CAPACITY;
            --fifo->count;
        } else {
            dst[i] = 0;
            if (streaming) ++fifo->underrun_samples;
        }
    }
}
