#!/usr/bin/env python3
"""Synthetic host-only protocol tests. No real serial port or hardware evidence."""
import base64
import importlib.util
import io
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location("memo_device", Path(__file__).parents[1] / "scripts/device.py")
device = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(device)


class Clock:
    def __init__(self):
        self.now = 0.0

    def __call__(self):
        return self.now


class FakeSerial:
    """Timed readline bytes, including deliberately incomplete lines."""
    def __init__(self, clock, events):
        self.clock = clock
        self.events = list(events)
        self.commands = []

    def write(self, payload):
        self.commands.append(payload)
        return len(payload)

    def readline(self):
        self.clock.now = round(self.clock.now + 0.1, 3)
        if self.events and self.events[0][0] <= self.clock.now:
            return self.events.pop(0)[1]
        return b""


def data_line(payload, offset=0):
    return b"DATA " + str(offset).encode() + b" " + base64.b64encode(payload) + b"\n"


class DeviceProtocolTests(unittest.TestCase):
    def session(self, events, **options):
        clock = Clock()
        connection = FakeSerial(clock, events)
        log = io.BytesIO()
        result = device.run_session(connection, log, clock=clock, printer=lambda *a, **k: None, **options)
        return result, connection, clock, log.getvalue()

    def test_line_buffer_waits_for_newline(self):
        buffer = device.LineBuffer()
        self.assertEqual(buffer.feed(b"DATA 0 AQ"), [])
        self.assertEqual(buffer.feed(b"IDBA==\r\nsecond\nthi"), [b"DATA 0 AQIDBA==\r\n", b"second\n"])
        self.assertEqual(buffer.feed(b"rd\n"), [b"third\n"])

    def test_fragmented_valid_dump(self):
        payload = bytes(range(44))
        encoded_line = data_line(payload)
        events = [(0.1, b"DUMP_BE"), (0.2, b"GIN 7 44\n"),
                  (0.3, encoded_line[:10]), (0.4, encoded_line[10:]),
                  (0.5, b"DUMP_END 7 44\n")]
        result, connection, _, log = self.session(events, dump_id=7)
        self.assertEqual(result, payload)
        self.assertEqual(connection.commands, [b"dump 7\n"])
        self.assertEqual(log, b"".join(chunk for _, chunk in events))

    def test_protocol_checks_offsets_ids_and_final_lengths(self):
        for invalid in (b"DATA 1 AAAA", b"DUMP_END 8 44", b"DUMP_END 7 44", b"DATA 0 !invalid!"):
            with self.subTest(invalid=invalid):
                receiver = device.DumpReceiver(7)
                self.assertEqual(receiver.consume(b"DUMP_BEGIN 7 44"), "begin")
                with self.assertRaises(RuntimeError):
                    receiver.consume(invalid)
        with self.assertRaises(RuntimeError):
            device.DumpReceiver(7).consume(b"DATA 0 AQ==")
        with self.assertRaises(RuntimeError):
            device.DumpReceiver(7).consume(b"DUMP_BEGIN 8 44")

    def test_error_fails_immediately(self):
        clock = Clock()
        connection = FakeSerial(clock, [(0.1, b"DUMP_ERROR 7\n")])
        with self.assertRaisesRegex(RuntimeError, "Device rejected or failed WAV export: DUMP_ERROR 7"):
            device.run_session(connection, io.BytesIO(), dump_id=7, clock=clock)
        self.assertLess(clock.now, 1)

    def test_idle_timeout_ignores_unrelated_logs(self):
        clock = Clock()
        events = [(0.1, b"DUMP_BEGIN 7 44\n")] + [(i, b"STATUS state=0 sd=1\n") for i in range(1, 20)]
        connection = FakeSerial(clock, events)
        with self.assertRaisesRegex(RuntimeError, "stalled for 15 seconds"):
            device.run_session(connection, io.BytesIO(), dump_id=7, seconds=300,
                               clock=clock, printer=lambda *a, **k: None)
        self.assertGreaterEqual(clock.now, 15)
        self.assertLess(clock.now, 16)

    def test_dump_timeout_expands_after_begin(self):
        payload = bytes(range(44))
        result, _, clock, _ = self.session([(0.1, b"DUMP_BEGIN 7 44\n"),
            (10, data_line(payload)), (11, b"DUMP_END 7 44\n")], dump_id=7, seconds=5)
        self.assertEqual(result, payload)
        self.assertGreater(clock.now, 5)
        self.assertGreater(device.dump_deadline(0, 5, 3840044), 100)
        self.assertEqual(device.dump_deadline(0, 1000, 3840044), 1000)

    def test_total_deadline_limits_slow_but_nonidle_export(self):
        clock = Clock()
        events = [(0.1, b"DUMP_BEGIN 7 44\n"), (10, data_line(b"a"))]
        connection = FakeSerial(clock, events)
        with self.assertRaisesRegex(RuntimeError, "size-based deadline"):
            device.run_session(connection, io.BytesIO(), dump_id=7, clock=clock)
        self.assertLess(clock.now, 16)

    def test_plain_observation_keeps_five_second_default(self):
        result, connection, clock, _ = self.session([])
        self.assertIsNone(result)
        self.assertEqual(connection.commands, [])
        self.assertGreaterEqual(clock.now, 5)
        self.assertLess(clock.now, 5.2)

    def test_usb_microphone_state_is_observable(self):
        self.session([(0.1, b"STATUS state=7 sd=1\n")], commands=["usb_open"], expect_state=7)

    def test_expect_state_polls_after_command(self):
        _, connection, clock, _ = self.session([(0.1, b"STATUS state=1 sd=1\n"),
            (0.7, b"STATUS state=3 sd=1\n")], commands=["stop"], expect_state=3)
        self.assertEqual(connection.commands[:2], [b"stop\n", b"status\n"])
        self.assertGreaterEqual(connection.commands.count(b"status\n"), 2)
        self.assertLess(clock.now, 1)

    def test_dump_waits_for_expected_state(self):
        payload = bytes(range(44))
        result, connection, _, _ = self.session([(0.1, b"STATUS state=3 sd=1\n"),
            (0.2, b"DUMP_BEGIN 7 44\n"), (0.3, data_line(payload)),
            (0.4, b"DUMP_END 7 44\n")], commands=["stop"], expect_state=3, dump_id=7)
        self.assertEqual(result, payload)
        self.assertEqual(connection.commands, [b"stop\n", b"status\n", b"dump 7\n"])

    def test_state_timeout_is_explicit(self):
        with self.assertRaisesRegex(RuntimeError, "waiting for device state 3"):
            self.session([(0.1, b"STATUS state=1 sd=1\n")], expect_state=3, seconds=1)

    def test_truncated_line_never_becomes_completed_dump(self):
        with self.assertRaisesRegex(RuntimeError, "stalled for 15 seconds"):
            self.session([(0.1, b"DUMP_BEGIN 7 44\n"), (0.2, b"DATA 0 AQ")], dump_id=7)


if __name__ == "__main__":
    print("HOST_ONLY: synthetic serial protocol and fake-clock tests; no device acceptance claimed", flush=True)
    unittest.main(verbosity=2)
