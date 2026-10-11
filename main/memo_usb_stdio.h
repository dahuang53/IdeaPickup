#pragma once
#include <stdbool.h>

/* A single VFS range may cover exactly the three real stdio descriptors.
 * Validate before closing anything; never fill a gap belonging to another fd. */
static inline bool memo_usb_stdio_range(const int fds[3], int fd_limit,
                                       int *first, int *end)
{
    if (!fds || !first || !end || fd_limit <= 0) return false;
    int low = fd_limit, high = -1;
    for (unsigned i = 0; i < 3; ++i) {
        if (fds[i] < 0 || fds[i] >= fd_limit) return false;
        for (unsigned j = 0; j < i; ++j) if (fds[i] == fds[j]) return false;
        if (fds[i] < low) low = fds[i];
        if (fds[i] > high) high = fds[i];
    }
    if (high - low != 2) return false;
    *first = low;
    *end = high + 1;
    return true;
}
