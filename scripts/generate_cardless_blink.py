#!/usr/bin/env python3
"""Derive a quicker, synchronized blink from the pinned Espressif AAF."""

from pathlib import Path
import hashlib
import struct


root = Path(__file__).resolve().parents[1]
source = root / "main/assets/cat/emotion_blink_slow_284_126.aaf"
target = root / "main/assets/cat/emotion_blink_cardless_284_126.aaf"
data = source.read_bytes()
assert hashlib.sha256(data).hexdigest() == (
    "7ea7b322ee1dc859dbbc90e40eeb90dbfde036c5acbf6d794d90b7bcc546713a"
)
count, checksum, payload_size = struct.unpack_from("<III", data)
assert count == 119 and payload_size == len(data) - 12
assert checksum == sum(data[12:]) & 0xFFFFFFFF
table = [struct.unpack_from("<II", data, 12 + i * 8) for i in range(count)]
assert all(entry == table[19] for entry in table[20:])

# The first 20 frames show both eyes blinking; five entries hold the exact
# previous compressed frame. Drop only those duplicates and the 99-frame tail.
moving = [table[0]]
for entry in table[1:20]:
    if entry != moving[-1]:
        moving.append(entry)
assert len(moving) == 15 and moving[-1] == table[19]
payload = b"".join(struct.pack("<II", *entry) for entry in moving) + data[12 + count * 8 :]
derived = struct.pack("<III", len(moving), sum(payload) & 0xFFFFFFFF, len(payload)) + payload
target.write_bytes(derived)
print(f"CAT_CARDLESS_ASSET generated {target.name}: {len(moving)} frames, {len(derived)} bytes")
