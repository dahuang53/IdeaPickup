#pragma once
#include <stdbool.h>
#include <stdint.h>

#define MEMO_USB_BUS_TIMEOUT_US 500000ULL

typedef struct {
    uint16_t frame;
    uint64_t changed_us;
    bool observed;
    bool live;
} memo_usb_bus_watch_t;

/* A configured self-powered controller can retain its state after unplug.
 * Only advancing received-SOF numbers establish an operational host bus. */
static inline bool memo_usb_bus_watch_update(memo_usb_bus_watch_t *w,
                                             bool configured, uint16_t frame,
                                             uint64_t now_us)
{
    if (!configured) {
        *w = (memo_usb_bus_watch_t){0};
        return false;
    }
    if (!w->observed) {
        w->observed = true;
        w->frame = frame;
        w->changed_us = now_us;
        w->live = false;
    } else if (frame != w->frame) {
        w->frame = frame;
        w->changed_us = now_us;
        w->live = true;
    } else if (now_us - w->changed_us >= MEMO_USB_BUS_TIMEOUT_US) {
        w->live = false;
    }
    return w->live;
}
