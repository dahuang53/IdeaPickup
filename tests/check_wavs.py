#!/usr/bin/env python3
"""Independently validate exported memo WAV files using stdlib struct and wave.

Examples:
  python3 tests/check_wavs.py private/exported-recordings --min-count 10 --min-duration 120
  python3 tests/check_wavs.py private/exported-recordings/memo_0000000001.wav

This proves file structure and duration, not actual microphone sound or playback.
"""

import argparse
import json
import re
import struct
import sys
import wave
from pathlib import Path

NAME = re.compile(r"memo_([0-9]{10})\.wav\Z")


def inspect(path: Path) -> dict:
    match = NAME.fullmatch(path.name)
    if not match or not 1 <= int(match[1]) <= 0xFFFFFFFF:
        raise ValueError("filename is not a canonical nonzero 32-bit memo ID")
    with path.open("rb") as stream:
        header = stream.read(44)
    if len(header) != 44:
        raise ValueError("header is shorter than 44 bytes")
    (riff, riff_length, wave_tag, fmt, fmt_length, encoding, channels,
     rate, byte_rate, alignment, bits, data_tag, data_length) = struct.unpack(
        "<4sI4s4sIHHIIHH4sI", header
    )
    if (riff, wave_tag, fmt, fmt_length, encoding, channels, rate, byte_rate,
        alignment, bits, data_tag) != (
        b"RIFF", b"WAVE", b"fmt ", 16, 1, 1, 16000, 32000, 2, 16, b"data"
    ):
        raise ValueError("expected canonical 16 kHz / 16 bit / mono PCM WAV")
    if not data_length or data_length % 2 or data_length > ((0x7FFFFFFF - 44) & ~1):
        raise ValueError("PCM byte length must be nonzero, even and fit the device's 2 GiB file limit")
    if riff_length != data_length + 36 or path.stat().st_size != data_length + 44:
        raise ValueError("RIFF/data lengths do not match the actual file length")
    with wave.open(str(path), "rb") as recording:
        if recording.getparams()[:3] != (1, 2, 16000):
            raise ValueError("independent wave decoder reported an unexpected format")
        frames = recording.getnframes()
        if frames * 2 != data_length:
            raise ValueError("independent wave decoder frame count mismatch")
        # Decode/read in bounded blocks so a two-minute or longer file is safe.
        read_bytes = 0
        while block := recording.readframes(8192):
            read_bytes += len(block)
        if read_bytes != data_length:
            raise ValueError("independent wave decoder could not read all frames")
    return {
        "path": str(path), "id": int(match[1]), "data_bytes": data_length,
        "frames": frames, "duration_seconds": frames / rate,
        "sample_rate": rate, "channels": channels, "bits_per_sample": bits,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="+", type=Path)
    parser.add_argument("--min-count", type=int, default=1)
    parser.add_argument("--min-duration", type=float, default=0,
                        help="require at least one recording this many seconds long")
    args = parser.parse_args()
    paths = []
    for path in args.paths:
        paths.extend(sorted(path.glob("*.wav")) if path.is_dir() else [path])
    paths = list(dict.fromkeys(path.resolve() for path in paths))
    records, errors = [], []
    ids = set()
    for path in paths:
        try:
            record = inspect(path)
            if record["id"] in ids:
                raise ValueError("duplicate memo ID in supplied files")
            ids.add(record["id"])
            records.append(record)
        except (OSError, ValueError, EOFError, wave.Error, struct.error) as error:
            errors.append({"path": str(path), "error": str(error)})
    if len(records) < args.min_count:
        errors.append({"error": f"need {args.min_count} valid recordings; found {len(records)}"})
    if args.min_duration > 0 and not any(
        record["duration_seconds"] >= args.min_duration for record in records
    ):
        errors.append({"error": f"need a recording >= {args.min_duration} seconds"})
    print(json.dumps({
        "status": "PASS" if not errors else "FAIL",
        "scope": "WAV file structure only; microphone, touch and listening require device evidence",
        "valid_count": len(records), "records": records, "errors": errors,
    }, ensure_ascii=False, indent=2))
    return bool(errors)


if __name__ == "__main__":
    sys.exit(main())
