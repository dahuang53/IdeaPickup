#!/usr/bin/env python3
"""Synthetic-only checks. Never invokes ffmpeg, AVFoundation, or a microphone."""
from __future__ import annotations

from array import array
from contextlib import redirect_stderr
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import wave

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from scripts import check_usb_audio as check  # noqa: E402

RAW_DEVICES = """[AVFoundation indev @ 0x123] AVFoundation video devices:
[AVFoundation indev @ 0x123] [0] FaceTime HD Camera
[AVFoundation indev @ 0x123] AVFoundation audio devices:
[AVFoundation indev @ 0x123] [0] MacBook Pro Microphone (ID: BuiltInMic)
[AVFoundation indev @ 0x123] [2] Meow USB Microphone (ID: AppleUSBAudioEngine:abc)
[in#0 @ 0x123] Error opening input: Input/output error
"""


def create_wav(path: Path, samples: list[int], rate: int = 16000) -> None:
    data = array("h", samples)
    if sys.byteorder != "little":
        data.byteswap()
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(rate)
        output.writeframes(data.tobytes())


class UsbAudioCheckTests(unittest.TestCase):
    def setUp(self) -> None:
        check.PRIVATE.mkdir(parents=True, exist_ok=True)
        self.tmp = tempfile.TemporaryDirectory(dir=check.PRIVATE)
        self.addCleanup(self.tmp.cleanup)
        self.directory = Path(self.tmp.name)

    def test_exact_audio_selection_never_defaults_to_builtin(self) -> None:
        self.assertEqual(check.parse_audio_devices(RAW_DEVICES),
                         [(0, "MacBook Pro Microphone"), (2, "Meow USB Microphone")])
        self.assertEqual(check.select_audio_index(RAW_DEVICES, "Meow USB Microphone"), 2)
        for name in ("", "default", "none", "meow usb microphone", "Missing USB",
                     "MacBook Pro Microphone"):
            with self.subTest(name=name), self.assertRaises(check.CheckError):
                check.select_audio_index(RAW_DEVICES, name)
        with self.assertRaises(check.CheckError):
            check.select_audio_index(RAW_DEVICES +
                                     "[AVFoundation indev @ x] [3] Meow USB Microphone\n",
                                     "Meow USB Microphone")
        cmd = check.capture_command(2, 3.5, self.directory / "out.wav")
        self.assertEqual(cmd[cmd.index("-i") + 1], ":2")
        self.assertIn("-n", cmd)
        self.assertNotIn("default", cmd)
        self.assertEqual(cmd[cmd.index("-ac") + 1], "1")
        self.assertEqual(cmd[cmd.index("-c:a") + 1], "pcm_s16le")

    def test_missing_device_fails_before_capture(self) -> None:
        with patch.object(check, "list_devices", return_value=(RAW_DEVICES, self.directory / "devices.log")), \
             patch.object(check.subprocess, "run") as run, redirect_stderr(io.StringIO()) as errors:
            result = check.main(["capture", "--device-name", "Missing USB", "--seconds", "1",
                                 "--output", str(self.directory / "capture.wav")])
        self.assertEqual(result, 2)
        self.assertIn("0 exact matches", errors.getvalue())
        run.assert_not_called()
        self.assertFalse((self.directory / "capture.wav").exists())

    def test_silence_signal_and_longest_zero_gap(self) -> None:
        silent = self.directory / "silent.wav"
        create_wav(silent, [0] * 16000)
        stats = check.analyze_wav(silent)
        self.assertEqual(stats["duration_seconds"], 1.0)
        self.assertEqual(stats["rms"], 0.0)
        self.assertEqual(stats["peak"], 0)
        self.assertEqual(stats["nonzero_samples"], 0)
        self.assertEqual(stats["longest_zero_gap_seconds"], 1.0)
        self.assertTrue(check.verdict(stats, "silence"))
        self.assertFalse(check.verdict(stats, "signal"))

        signal = self.directory / "signal.wav"
        create_wav(signal, [0] * 8000 + [1000, -1000] + [0] * 4000)
        stats = check.analyze_wav(signal)
        self.assertEqual(stats["samples"], 12002)
        self.assertEqual(stats["nonzero_samples"], 2)
        self.assertEqual(stats["peak"], 1000)
        self.assertGreater(stats["rms"], 0.0)
        self.assertEqual(stats["longest_zero_gap_samples"], 8000)
        self.assertTrue(check.verdict(stats, "signal"))
        self.assertFalse(check.verdict(stats, "silence"))
        report = check.save_analysis(signal, stats, "signal")
        self.assertTrue(report.is_relative_to(check.PRIVATE))
        self.assertIn('"passed": true', report.read_text())

    def test_path_and_existing_output_protection(self) -> None:
        with self.assertRaises(check.CheckError):
            check.private_path(ROOT / "README.md")
        with self.assertRaises(check.CheckError):
            check.private_path("../README.md")
        with self.assertRaises(check.CheckError):
            check.private_path(self.directory / "../../outside.wav")
        existing = self.directory / "existing.wav"
        create_wav(existing, [0] * 16)
        with patch.object(check, "list_devices") as listing, redirect_stderr(io.StringIO()):
            self.assertEqual(check.main(["capture", "--device-name", "Meow USB Microphone",
                                         "--seconds", "1", "--output", str(existing)]), 2)
        listing.assert_not_called()
        self.assertEqual(check.analyze_wav(existing)["samples"], 16)

    def test_reject_non_pcm_mono_and_empty(self) -> None:
        stereo = self.directory / "stereo.wav"
        with wave.open(str(stereo), "wb") as output:
            output.setnchannels(2)
            output.setsampwidth(2)
            output.setframerate(16000)
            output.writeframes(b"\x00\x00\x00\x00")
        with self.assertRaises(check.CheckError):
            check.analyze_wav(stereo)
        empty = self.directory / "empty.wav"
        create_wav(empty, [])
        with self.assertRaises(check.CheckError):
            check.analyze_wav(empty)


if __name__ == "__main__":
    unittest.main()
