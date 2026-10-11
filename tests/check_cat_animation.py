#!/usr/bin/env python3
"""Verify the pinned slow-blink AAF supports the shortened cardless loop."""

from pathlib import Path
import hashlib
import struct


source = Path(__file__).resolve().parents[1] / "main/assets/cat/emotion_blink_slow_284_126.aaf"
data = source.read_bytes()
assert hashlib.sha256(data).hexdigest() == (
    "7ea7b322ee1dc859dbbc90e40eeb90dbfde036c5acbf6d794d90b7bcc546713a"
)
count, checksum, payload_size = struct.unpack_from("<III", data)
assert count == 119 and payload_size == len(data) - 12
assert checksum == sum(data[12:]) & 0xFFFFFFFF
table = [struct.unpack_from("<II", data, 12 + i * 8) for i in range(count)]
body = data[12 + count * 8 :]
for length, offset in table:
    assert 2 < length <= len(body) - offset
    assert body[offset : offset + 2] == b"ZZ"
assert len(set(table[:20])) > 10  # Actual eye motion before the held pose.
assert all(entry == table[19] for entry in table[20:])

derived_path = source.with_name("emotion_blink_cardless_284_126.aaf")
derived = derived_path.read_bytes()
derived_count, derived_checksum, derived_payload_size = struct.unpack_from("<III", derived)
assert derived_count == 15 and derived_payload_size == len(derived) - 12
assert derived_checksum == sum(derived[12:]) & 0xFFFFFFFF
derived_table = [struct.unpack_from("<II", derived, 12 + i * 8) for i in range(derived_count)]
expected = [table[0]]
for entry in table[1:20]:
    if entry != expected[-1]:
        expected.append(entry)
assert derived_table == expected
assert derived[12 + derived_count * 8 :] == body
print("CAT_CARDLESS_ASSET PASS: same eye art, 15 moving frames, 99-frame tail removed")
