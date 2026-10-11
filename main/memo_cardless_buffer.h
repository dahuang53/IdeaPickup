#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Caller synchronizes access. Packets stay until their exact chunk is ACKed.
 * No overwrite, eviction, allocation, or dependence on ESP/FreeRTOS. */
typedef struct {
    uint8_t *storage;
    size_t capacity;
    size_t head;
    size_t used;
    size_t packets;
} memo_cardless_buffer_t;
void memo_cardless_buffer_init(memo_cardless_buffer_t *b, void *storage, size_t capacity);
bool memo_cardless_buffer_push(memo_cardless_buffer_t *b, const void *packet, size_t length);
size_t memo_cardless_buffer_peek(const memo_cardless_buffer_t *b, void *out, size_t capacity,
                                size_t max_packets, size_t *packets);
bool memo_cardless_buffer_ack(memo_cardless_buffer_t *b, size_t bytes, size_t packets);
bool memo_cardless_next_id(uint32_t counter, uint32_t floor, uint32_t *id);
#ifdef __cplusplus
}
#endif
