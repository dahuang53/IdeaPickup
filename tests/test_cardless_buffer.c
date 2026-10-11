#include "memo_cardless_buffer.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void test_full_retains_oldest_and_retry(void)
{
    uint8_t memory[23], out[23], retry[23];
    const uint8_t first[] = {1, 2, 3, 4, 5};
    const uint8_t second[] = {6, 7, 8, 9};
    const uint8_t third[] = {10, 11, 12, 13, 14, 15, 16};
    memo_cardless_buffer_t b;
    memo_cardless_buffer_init(&b, memory, sizeof(memory));
    assert(memo_cardless_buffer_push(&b, first, sizeof(first)));
    assert(memo_cardless_buffer_push(&b, second, sizeof(second)));
    assert(memo_cardless_buffer_push(&b, third, sizeof(third)));
    assert(b.used == 22 && b.packets == 3);
    assert(!memo_cardless_buffer_push(&b, first, sizeof(first)));
    size_t count;
    size_t length = memo_cardless_buffer_peek(&b, out, sizeof(out), 2, &count);
    assert(length == 13 && count == 2);
    assert(out[0] == 5 && out[1] == 0 && !memcmp(out + 2, first, sizeof(first)));
    assert(out[7] == 4 && out[8] == 0 && !memcmp(out + 9, second, sizeof(second)));
    assert(memo_cardless_buffer_peek(&b, retry, sizeof(retry), 2, &count) == length);
    assert(!memcmp(out, retry, length)); // Lost HTTP ACK retries exactly the same bytes.
    assert(!memo_cardless_buffer_ack(&b, 12, 2)); // Partial packet ACK forbidden.
    assert(!memo_cardless_buffer_ack(&b, 13, 1)); // Packet count must also match.
    assert(!memo_cardless_buffer_ack(&b, 24, 3));
    assert(b.used == 22 && b.packets == 3);
    assert(memo_cardless_buffer_ack(&b, length, 2));
    assert(b.used == 9 && b.packets == 1);
    assert(memo_cardless_buffer_push(&b, first, sizeof(first))); // Crosses physical ring end.
    length = memo_cardless_buffer_peek(&b, out, sizeof(out), 20, &count);
    assert(length == 16 && count == 2);
    assert(out[0] == 7 && !memcmp(out + 2, third, sizeof(third)));
    assert(out[9] == 5 && !memcmp(out + 11, first, sizeof(first)));
    assert(memo_cardless_buffer_ack(&b, length, count));
    assert(b.used == 0 && b.packets == 0);
}

static void test_mixed_size_wraparound_against_fifo(void)
{
    uint8_t storage[1024], chunk[1024], audio[100];
    uint8_t oracle[1024];
    size_t oracle_used = 0, oracle_packets = 0;
    uint32_t rng = 1729;
    memo_cardless_buffer_t b;
    memo_cardless_buffer_init(&b, storage, sizeof(storage));
    for (unsigned step = 0; step < 20000; ++step) {
        rng = rng * 1664525U + 1013904223U;
        size_t n = 1 + rng % sizeof(audio);
        memset(audio, (uint8_t)step, n);
        bool expected = n + 2 <= sizeof(oracle) - oracle_used;
        assert(memo_cardless_buffer_push(&b, audio, n) == expected);
        if (expected) {
            oracle[oracle_used++] = (uint8_t)n;
            oracle[oracle_used++] = (uint8_t)(n >> 8);
            memcpy(oracle + oracle_used, audio, n);
            oracle_used += n;
            ++oracle_packets;
        }
        if ((rng & 3) == 0 || !expected) {
            size_t wanted = 1 + rng % 7;
            size_t count = 0;
            size_t got = memo_cardless_buffer_peek(&b, chunk, sizeof(chunk), wanted, &count);
            assert(count <= wanted && got <= oracle_used && count <= oracle_packets);
            assert(!memcmp(chunk, oracle, got));
            if (got) {
                assert(memo_cardless_buffer_ack(&b, got, count));
                memmove(oracle, oracle + got, oracle_used - got);
                oracle_used -= got;
                oracle_packets -= count;
            }
        }
        assert(b.used == oracle_used && b.packets == oracle_packets);
    }
}

static void test_chunk_boundary_and_id_namespace(void)
{
    uint8_t storage[20000], packet[60], chunk[16000];
    memset(packet, 0x55, sizeof(packet));
    memo_cardless_buffer_t b;
    memo_cardless_buffer_init(&b, storage, sizeof(storage));
    for (unsigned i = 0; i < 251; ++i) assert(memo_cardless_buffer_push(&b, packet, sizeof(packet)));
    size_t count = 0;
    size_t bytes = memo_cardless_buffer_peek(&b, chunk, sizeof(chunk), 250, &count);
    assert(count == 250 && bytes == 15500);
    assert(memo_cardless_buffer_ack(&b, bytes, count));
    assert(b.packets == 1 && b.used == 62);
    assert(!memo_cardless_buffer_peek(&b, chunk, 61, 250, &count) && !count);
    assert(memo_cardless_buffer_peek(&b, chunk, sizeof(chunk), 250, &count) == 62 && count == 1);
    uint32_t id = 0;
    assert(memo_cardless_next_id(0, 380, &id) && id == 0x8000017dU);
    assert(memo_cardless_next_id(400, 380, &id) && id == 0x80000191U);
    assert(memo_cardless_next_id(400, 500, &id) && id == 0x800001f5U);
    assert(memo_cardless_next_id(400, 0x80000200U, &id) && id == 0x80000201U);
    assert(!memo_cardless_next_id(0x7fffffff, 0, &id));
    assert(!memo_cardless_next_id(0, 0x7fffffff, &id));
    assert(!memo_cardless_next_id(0, 0, NULL));
    assert(!memo_cardless_buffer_push(&b, packet, 0));
    assert(!memo_cardless_buffer_push(&b, NULL, 1));
}
int main(void)
{
    test_full_retains_oldest_and_retry();
    test_mixed_size_wraparound_against_fifo();
    test_chunk_boundary_and_id_namespace();
    puts("cardless buffer: PASS (no eviction, retry identity, ACK bounds, 20k wraparound, IDs)");
    return 0;
}
