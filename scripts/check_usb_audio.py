#!/usr/bin/env python3
"""Inspect the explicitly named AVFoundation USB microphone without using a default input.

All raw device listings, ffmpeg logs, WAVs, and analysis JSON stay under private/.
Device selection follows FFmpeg's AVFoundation documentation:
https://ffmpeg.org/ffmpeg-devices.html#avfoundation
"""
from __future__ import annotations

import argparse
from array import array
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import wave

PRIVATE = (Path(__file__).resolve().parents[1] / "private").resolve()
FFMPEG = Path("/opt/homebrew/bin/ffmpeg")
DEVICE_LINE = re.compile(r"\]\s*\[(\d+)\]\s+(.+)$")
ID_SUFFIX = re.compile(r"\s+\(ID:\s*.+\)$")
BUILTIN_NAME = re.compile(r"built[- ]in|internal|macbook.*microphone|内建|内置", re.IGNORECASE)


class CheckError(Exception):
    pass


def private_path(value: str | Path) -> Path:
    path = Path(value)
    if not path.is_absolute():
        path = PRIVATE / path
    resolved = path.resolve(strict=False)
    if resolved == PRIVATE or not resolved.is_relative_to(PRIVATE):
        raise CheckError("output and input paths must be inside project private/")
    return resolved


def new_private_path(prefix: str, suffix: str) -> Path:
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    return private_path(f"{prefix}-{stamp}-{os.getpid()}{suffix}")


def write_exclusive(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("x", encoding="utf-8") as output:
        output.write(content)


def parse_audio_devices(raw: str) -> list[tuple[int, str]]:
    """Read only AVFoundation's audio section, never video device indices."""
    devices: list[tuple[int, str]] = []
    in_audio = False
    for line in raw.splitlines():
        if "AVFoundation audio devices:" in line:
            in_audio = True
            continue
        if "AVFoundation video devices:" in line:
            in_audio = False
            continue
        if not in_audio:
            continue
        match = DEVICE_LINE.search(line)
        if match:
            name = ID_SUFFIX.sub("", match.group(2)).strip()
            if name:
                devices.append((int(match.group(1)), name))
    return devices


def select_audio_index(raw: str, exact_name: str) -> int:
    if not exact_name or exact_name.strip() != exact_name or exact_name.lower() in {"default", "none"}:
        raise CheckError("--device-name must be one exact, non-default audio device name")
    if BUILTIN_NAME.search(exact_name):
        raise CheckError("refusing an apparent built-in microphone; specify the USB device name")
    matches = [index for index, name in parse_audio_devices(raw) if name == exact_name]
    if len(matches) != 1:
        raise CheckError(f"audio device name has {len(matches)} exact matches; need exactly one")
    return matches[0]


def list_devices() -> tuple[str, Path]:
    if not FFMPEG.is_file():
        raise CheckError(f"ffmpeg missing: {FFMPEG}")
    command = [str(FFMPEG), "-hide_banner", "-f", "avfoundation", "-list_devices", "true", "-i", ""]
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, errors="replace", timeout=15, check=False)
    raw = result.stdout
    log_path = new_private_path("usb-audio-devices", ".log")
    write_exclusive(log_path, raw)
    if "AVFoundation audio devices:" not in raw:
        raise CheckError(f"AVFoundation audio device list unavailable; details: {log_path}")
    return raw, log_path


def capture_command(index: int, seconds: float, destination: Path) -> list[str]:
    if index < 0 or not math.isfinite(seconds) or seconds <= 0 or seconds > 600:
        raise CheckError("invalid capture index or duration (0 < seconds <= 600)")
    return [str(FFMPEG), "-hide_banner", "-nostdin", "-n", "-f", "avfoundation",
            "-i", f":{index}", "-t", f"{seconds:g}", "-vn", "-ac", "1", "-ar", "16000",
            "-c:a", "pcm_s16le", "-f", "wav", str(destination)]


def analyze_wav(path: Path) -> dict[str, int | float | str | bool]:
    try:
        with wave.open(str(path), "rb") as wav:
            if wav.getnchannels() != 1 or wav.getsampwidth() != 2 or wav.getcomptype() != "NONE":
                raise CheckError("expected uncompressed 16-bit mono PCM WAV")
            rate = wav.getframerate()
            if rate <= 0:
                raise CheckError("invalid WAV sample rate")
            frames = wav.getnframes()
            if frames <= 0:
                raise CheckError("WAV has no samples")
            total = nonzero = peak = sum_sq = zero_run = max_zero_run = 0
            while data := wav.readframes(65536):
                samples = array("h")
                samples.frombytes(data)
                if sys.byteorder != "little":
                    samples.byteswap()
                for sample in samples:
                    magnitude = abs(sample)
                    total += 1
                    sum_sq += sample * sample
                    if magnitude:
                        nonzero += 1
                        zero_run = 0
                        if magnitude > peak:
                            peak = magnitude
                    else:
                        zero_run += 1
                        if zero_run > max_zero_run:
                            max_zero_run = zero_run
            if total != frames:
                raise CheckError(f"WAV frame count mismatch: header={frames}, read={total}")
    except (OSError, EOFError, ValueError, wave.Error) as exc:
        raise CheckError(f"cannot analyze WAV: {exc}") from exc
    return {
        "sample_rate_hz": rate,
        "samples": total,
        "duration_seconds": total / rate,
        "rms": math.sqrt(sum_sq / total),
        "peak": peak,
        "nonzero_samples": nonzero,
        "nonzero_fraction": nonzero / total,
        "longest_zero_gap_samples": max_zero_run,
        "longest_zero_gap_seconds": max_zero_run / rate,
        "digital_silence": nonzero == 0,
    }


def verdict(stats: dict[str, int | float | str | bool], expectation: str | None) -> bool:
    if expectation == "silence":
        return bool(stats["digital_silence"])
    if expectation == "signal":
        return int(stats["nonzero_samples"]) > 0 and float(stats["rms"]) > 0
    return True


def save_analysis(path: Path, stats: dict[str, int | float | str | bool], expectation: str | None) -> Path:
    report = new_private_path("usb-audio-analysis", ".json")
    write_exclusive(report, json.dumps({"wav": str(path), "expectation": expectation,
                                        "passed": verdict(stats, expectation), "metrics": stats},
                                       ensure_ascii=False, indent=2) + "\n")
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("list", help="Save raw AVFoundation device list under private/")
    capture = commands.add_parser("capture", help="Capture an exact named audio device to private/")
    capture.add_argument("--device-name", required=True, help="Exact AVFoundation audio device name; no default alias")
    capture.add_argument("--seconds", required=True, type=float)
    capture.add_argument("--output", help="New WAV path under private/; never overwritten")
    analyze = commands.add_parser("analyze", help="Analyze a 16-bit mono WAV already under private/")
    analyze.add_argument("wav")
    for sub in (capture, analyze):
        expected = sub.add_mutually_exclusive_group()
        expected.add_argument("--expect-silence", action="store_true")
        expected.add_argument("--expect-signal", action="store_true")
    args = parser.parse_args(argv)
    expectation = "silence" if getattr(args, "expect_silence", False) else (
        "signal" if getattr(args, "expect_signal", False) else None)
    try:
        PRIVATE.mkdir(parents=True, exist_ok=True)
        if args.command == "list":
            raw, log_path = list_devices()
            print(f"AVFoundation audio devices: {len(parse_audio_devices(raw))}; raw list: {log_path}")
            return 0
        if args.command == "capture":
            destination = private_path(args.output) if args.output else new_private_path("usb-audio", ".wav")
            if destination.suffix.lower() != ".wav":
                raise CheckError("capture output must end in .wav")
            if destination.exists():
                raise CheckError(f"refusing to overwrite existing file: {destination}")
            raw, list_log = list_devices()
            index = select_audio_index(raw, args.device_name)
            destination.parent.mkdir(parents=True, exist_ok=True)
            cmd = capture_command(index, args.seconds, destination)
            try:
                result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        text=True, errors="replace", timeout=args.seconds + 30, check=False)
                capture_log = new_private_path("usb-audio-capture", ".log")
                write_exclusive(capture_log, result.stdout)
            except subprocess.TimeoutExpired as exc:
                raise CheckError(f"ffmpeg capture timed out; device list: {list_log}") from exc
            if result.returncode != 0 or not destination.is_file():
                raise CheckError(f"ffmpeg capture failed; details: {capture_log}")
            print(f"capture: {destination}; ffmpeg log: {capture_log}; device list: {list_log}")
            path = destination
        else:
            path = private_path(args.wav)
            if not path.is_file():
                raise CheckError(f"WAV missing: {path}")
        stats = analyze_wav(path)
        report = save_analysis(path, stats, expectation)
        passed = verdict(stats, expectation)
        print(f"{'PASS' if passed else 'FAIL'} digital {'silence' if stats['digital_silence'] else 'signal'}; analysis: {report}")
        if expectation == "signal":
            print("Digital signal confirms samples only; speech clarity still requires listening.")
        return 0 if passed else 1
    except (CheckError, subprocess.TimeoutExpired) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
