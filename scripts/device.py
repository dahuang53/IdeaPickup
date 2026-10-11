#!/usr/bin/env python3
"""Capture device logs, issue app commands, and fetch WAVs with one serial owner.

Dump logs contain the recording as base64 audio: keep both --log and --output
under private/. This tool does not prove real touch input or audible playback.
"""
import argparse
import base64
import binascii
import math
import pathlib
import re
import time

DUMP_IDLE_SECONDS = 15.0
# Conservative rate below 115200-baud base64 throughput. Firmware also yields
# one 10 ms tick per 384-byte block (about 38 kB/s).
DUMP_BYTES_PER_SECOND = 8000.0
ANSI_COLOR = re.compile(rb"\x1b\[[0-9;]*m")
STATUS = re.compile(rb"STATUS state=([0-7])(?:\s|$)")


class LineBuffer:
    """readline(timeout) may return fragments; expose only complete lines."""
    def __init__(self):
        self.pending = bytearray()

    def feed(self, chunk):
        self.pending.extend(chunk)
        lines = []
        while b"\n" in self.pending:
            end = self.pending.index(b"\n") + 1
            lines.append(bytes(self.pending[:end]))
            del self.pending[:end]
        if len(self.pending) > 65536:
            raise RuntimeError("Serial line exceeded 64 KiB without a newline")
        return lines


class DumpReceiver:
    """Validate the app's export protocol; WAV format is checked separately."""
    def __init__(self, record_id):
        self.record_id = record_id
        self.expected = None
        self.blob = bytearray()
        self.completed = False

    def consume(self, line):
        tokens = line.split()
        if not tokens:
            return None
        verb = tokens[0]
        if verb == b"DUMP_ERROR":
            raise RuntimeError("Device rejected or failed WAV export: " + line.decode(errors="replace"))
        if verb == b"DUMP_BEGIN":
            if len(tokens) != 3 or self.expected is not None:
                raise RuntimeError("Malformed or duplicate DUMP_BEGIN")
            record_id, size = self._numbers(tokens[1:])
            if record_id != self.record_id or not 44 <= size <= 0x7FFFFFFF:
                raise RuntimeError("DUMP_BEGIN has the wrong ID or invalid file size")
            self.expected = size
            return "begin"
        if verb == b"DATA":
            if len(tokens) != 3 or self.expected is None or self.completed:
                raise RuntimeError("DATA received outside a valid WAV export")
            (offset,) = self._numbers(tokens[1:2])
            if offset != len(self.blob):
                raise RuntimeError("Missing or duplicate WAV data block")
            try:
                block = base64.b64decode(tokens[2], validate=True)
            except (binascii.Error, ValueError) as error:
                raise RuntimeError(f"Invalid base64 WAV data: {error}") from error
            if not 1 <= len(block) <= 384 or len(self.blob) + len(block) > self.expected:
                raise RuntimeError("WAV data block exceeds the declared file size")
            self.blob.extend(block)
            return "data"
        if verb == b"DUMP_END":
            if len(tokens) != 3 or self.expected is None or self.completed:
                raise RuntimeError("DUMP_END received outside a valid WAV export")
            record_id, size = self._numbers(tokens[1:])
            if record_id != self.record_id or len(self.blob) != self.expected or size != self.expected:
                raise RuntimeError("WAV export ended with the wrong ID or final length")
            self.completed = True
            return "end"
        return None

    @staticmethod
    def _numbers(tokens):
        if any(not token.isdigit() for token in tokens):
            raise RuntimeError("Non-numeric WAV export ID, offset or size")
        return tuple(int(token) for token in tokens)


def dump_deadline(now, minimum_deadline, size):
    return max(minimum_deadline, now + DUMP_IDLE_SECONDS + size / DUMP_BYTES_PER_SECOND)


def run_session(connection, log, *, commands=(), seconds=5.0, dump_id=None,
                expect_state=None, clock=time.monotonic, printer=print):
    """Tests supply in-memory transport and a fake clock, never a real port."""
    def send(command):
        payload = (command + "\n").encode()
        if connection.write(payload) != len(payload):
            raise RuntimeError("Serial command was not written completely")

    started = clock()
    deadline = started + seconds
    for command in commands:
        send(command)
    receiver = DumpReceiver(dump_id) if dump_id is not None else None
    lines = LineBuffer()
    state_reached = expect_state is None
    next_status = started
    dump_started = False
    last_dump_progress = None
    while True:
        now = clock()
        if state_reached and receiver is not None and not dump_started:
            send(f"dump {dump_id}")
            dump_started = True
            last_dump_progress = now
            deadline = max(deadline, now + DUMP_IDLE_SECONDS)
        if receiver is not None and receiver.completed:
            return bytes(receiver.blob)
        if expect_state is not None and state_reached and receiver is None:
            return None
        if dump_started and now - last_dump_progress >= DUMP_IDLE_SECONDS:
            raise RuntimeError("WAV dump stalled for 15 seconds; no completed output written")
        if now >= deadline:
            if not state_reached:
                raise RuntimeError(f"Timed out waiting for device state {expect_state}")
            if receiver is not None:
                raise RuntimeError("WAV dump exceeded its size-based deadline; no completed output written")
            return None
        if not state_reached and now >= next_status:
            send("status")
            next_status = now + 0.5
        chunk = connection.readline()
        if not chunk:
            continue
        log.write(chunk)
        log.flush()
        for line in lines.feed(chunk):
            clean = ANSI_COLOR.sub(b"", line).strip()
            if dump_started:
                event = receiver.consume(clean)
                if event is not None:
                    last_dump_progress = clock()
                    if event == "begin":
                        deadline = dump_deadline(clock(), deadline, receiver.expected)
                    continue
            match = STATUS.match(clean)
            if match and int(match[1]) == expect_state:
                state_reached = True
            printer(line.decode(errors="replace").rstrip(), flush=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--seconds", type=float, default=5,
                        help="observation/state-wait timeout; minimum total timeout for a dump (default: 5)")
    parser.add_argument("--command", action="append", default=[])
    parser.add_argument("--log", type=pathlib.Path, required=True)
    parser.add_argument("--dump", type=int)
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--expect-state", type=int, choices=range(8),
                        help="poll status until this state, then start --dump if supplied")
    args = parser.parse_args(argv)
    if not math.isfinite(args.seconds) or args.seconds <= 0:
        parser.error("--seconds must be a positive finite number")
    if args.dump is not None:
        if not 1 <= args.dump <= 0xFFFFFFFF:
            parser.error("--dump must be a nonzero 32-bit recording ID")
        if args.output is None:
            parser.error("--dump requires --output")
        if args.output.exists():
            parser.error("--output already exists; existing recordings will not be replaced")
        if args.output.resolve() == args.log.resolve():
            parser.error("--output and --log must be different files")
    elif args.output is not None:
        parser.error("--output requires --dump")
    # Imported only for real use; parser tests neither import nor open a device.
    import serial
    args.log.parent.mkdir(parents=True, exist_ok=True)
    try:
        with serial.Serial(args.port, 115200, timeout=.1, write_timeout=2) as connection, args.log.open("wb") as log:
            blob = run_session(connection, log, commands=args.command, seconds=args.seconds,
                               dump_id=args.dump, expect_state=args.expect_state)
        if blob is not None:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            with args.output.open("xb") as output:
                output.write(blob)
            print(f"Received {len(blob)} bytes: {args.output}")
    except (OSError, RuntimeError, ValueError) as error:
        parser.exit(1, f"ERROR: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
