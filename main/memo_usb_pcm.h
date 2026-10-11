#pragma once
#include <stddef.h>
#include <stdint.h>

#define MEMO_USB_PCM_CAPACITY 4096U

typedef struct {
    int16_t samples[MEMO_USB_PCM_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    uint32_t dropped_samples;
    uint32_t underrun_samples;
} memo_usb_pcm_t;

void memo_usb_pcm_clear(memo_usb_pcm_t *fifo);
void memo_usb_pcm_push(memo_usb_pcm_t *fifo, const int16_t *src, size_t count);
void memo_usb_pcm_pop(memo_usb_pcm_t *fifo, int16_t *dst, size_t count, int streaming);
