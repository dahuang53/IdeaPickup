#include "memo_cardless_buffer.h"
#include <string.h>

static void ring_copy_out(const memo_cardless_buffer_t *b, size_t offset, void *dst, size_t n)
{
    size_t pos = (b->head + offset) % b->capacity;
    size_t first = b->capacity - pos;
    if (first > n) first = n;
    memcpy(dst, b->storage + pos, first);
    memcpy((uint8_t *)dst + first, b->storage, n - first);
}
static void ring_copy_in(memo_cardless_buffer_t *b, const void *src, size_t n)
{
    size_t pos = (b->head + b->used) % b->capacity;
    size_t first = b->capacity - pos;
    if (first > n) first = n;
    memcpy(b->storage + pos, src, first);
    memcpy(b->storage, (const uint8_t *)src + first, n - first);
    b->used += n;
}
void memo_cardless_buffer_init(memo_cardless_buffer_t *b, void *storage, size_t capacity)
{
    *b = (memo_cardless_buffer_t){ .storage = storage, .capacity = capacity };
}
bool memo_cardless_buffer_push(memo_cardless_buffer_t *b, const void *packet, size_t length)
{
    if (!b || !b->storage || !packet || !length || length > UINT16_MAX ||
        b->capacity - b->used < length + 2) return false;
    uint8_t prefix[2] = { (uint8_t)length, (uint8_t)(length >> 8) };
    ring_copy_in(b, prefix, 2);
    ring_copy_in(b, packet, length);
    ++b->packets;
    return true;
}
size_t memo_cardless_buffer_peek(const memo_cardless_buffer_t *b, void *out, size_t capacity,
                                size_t max_packets, size_t *packets)
{
    size_t bytes = 0, count = 0;
    if (packets) *packets = 0;
    if (!b || !b->storage || !out || !b->capacity) return 0;
    while (count < b->packets && count < max_packets && bytes + 2 <= b->used) {
        uint8_t prefix[2];
        ring_copy_out(b, bytes, prefix, 2);
        size_t n = 2 + prefix[0] + ((size_t)prefix[1] << 8);
        if (n <= 2 || n > b->used - bytes || n > capacity - bytes) break;
        bytes += n;
        ++count;
    }
    if (bytes) ring_copy_out(b, 0, out, bytes);
    if (packets) *packets = count;
    return bytes;
}
bool memo_cardless_buffer_ack(memo_cardless_buffer_t *b, size_t bytes, size_t packets)
{
    if (!b || !bytes || !packets || bytes > b->used || packets > b->packets) return false;
    size_t validated = 0;
    for (size_t i = 0; i < packets; ++i) {
        if (validated + 2 > bytes) return false;
        uint8_t prefix[2];
        ring_copy_out(b, validated, prefix, 2);
        size_t n = 2 + prefix[0] + ((size_t)prefix[1] << 8);
        if (n <= 2 || n > bytes - validated) return false;
        validated += n;
    }
    if (validated != bytes) return false;
    b->head = (b->head + bytes) % b->capacity;
    b->used -= bytes;
    b->packets -= packets;
    return true;
}
bool memo_cardless_next_id(uint32_t counter, uint32_t floor, uint32_t *id)
{
    counter &= UINT32_C(0x7fffffff);
    floor &= UINT32_C(0x7fffffff);
    if (floor > counter) counter = floor;
    if (!id || counter == UINT32_C(0x7fffffff)) return false;
    *id = UINT32_C(0x80000000) | (counter + 1);
    return true;
}
