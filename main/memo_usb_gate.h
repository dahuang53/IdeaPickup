#pragma once
#include <stdbool.h>

static inline bool memo_usb_gate_should_stream(bool connected, bool streaming,
                                                bool enabled, bool local_muted,
                                                bool host_muted)
{
    return connected && streaming && enabled && !local_muted && !host_muted;
}
