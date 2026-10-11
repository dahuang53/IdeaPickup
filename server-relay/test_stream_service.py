from test_support import pair_device, resolve_data_fixture
"""Synthetic audio only. No provider calls, user audio, or real credentials."""
import ctypes
import hashlib
import http.client
import io
import json
import math
import os
import pathlib
import socket
import sqlite3
import struct
import tempfile
import threading
import time
import unittest
import wave
from http.server import ThreadingHTTPServer
from unittest import mock

from app import _ClosingConnection, MemoStore, RelayError, RelayService, read_exact
from stream_service import FORMAT, MAX_CHUNK, RETENTION_MS, OpusLibrary, StreamError, StreamService
from test_app import wav_bytes

AUTH = "Bearer " + "a" * 64
DEVICE = "test-vocat"
RECORDING = 0x80000001


def synthetic_chunks(frequencies=(440, 660), frames=50):
    """Real libopus CBR 24 kbps encoder; each frame has exactly 320 samples."""
    lib = OpusLibrary().lib
    lib.opus_encoder_create.argtypes = [ctypes.c_int32, ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
    lib.opus_encoder_create.restype = ctypes.c_void_p
    lib.opus_encoder_destroy.argtypes = [ctypes.c_void_p]
    lib.opus_encoder_ctl.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.opus_encoder_ctl.restype = ctypes.c_int
    lib.opus_encode.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int16), ctypes.c_int,
                               ctypes.c_void_p, ctypes.c_int32]
    lib.opus_encode.restype = ctypes.c_int32
    error = ctypes.c_int()
    encoder = lib.opus_encoder_create(16000, 1, 2048, ctypes.byref(error))
    if not encoder or error.value:
        raise RuntimeError("synthetic Opus encoder unavailable")
    try:
        for request, value in [(4002, 24000), (4006, 0), (4016, 0), (4010, 5)]:
            if lib.opus_encoder_ctl(encoder, request, ctypes.c_int(value)):
                raise RuntimeError("Opus encoder parameter rejected")
        output = (ctypes.c_ubyte * 1275)()
        chunks = []
        for frequency in frequencies:
            body = bytearray()
            for frame in range(frames):
                pcm = (ctypes.c_int16 * 320)(*[round(12000 * math.sin(2 * math.pi * frequency * (frame * 320 + i) / 16000)) for i in range(320)])
                size = lib.opus_encode(encoder, pcm, 320, output, len(output))
                if size != 60:
                    raise RuntimeError(f"expected CBR 60 byte frame, got {size}")
                body.extend(struct.pack("<H", size))
                body.extend(bytes(output[:size]))
            chunks.append(bytes(body))
        return chunks
    finally:
        lib.opus_encoder_destroy(encoder)


class StreamTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Missing libopus is a failed dependency check, never a skipped decoder test.
        cls.chunks = synthetic_chunks()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="meow-stream-test-")
        self.store = MemoStore(pathlib.Path(self.temp.name) / "state.sqlite3")
        pair_device(self.store, DEVICE, "a" * 64)
        self.auth = AUTH
        self.now = 1_000_000
        self.asr_calls, self.deliveries = [], []

        def transcribe(data, duration):
            self.asr_calls.append((data, duration))
            return "合成音频用于验证完整传输", "synthetic"

        self.relay = RelayService(self.store, "q" * 32, "s" * 32,
                                  qwen_transcriber=transcribe,
                                  siyuan_sender=lambda title, content: self.deliveries.append((title, content)))
        self.stream = self.new_service()
        self.relay.streams = self.stream
        self.headers = {"Authorization": self.auth, "X-Device-ID": DEVICE, "X-Recording-ID": str(RECORDING)}

    def new_service(self, **kwargs):
        service = StreamService(self.relay, clock=lambda: self.now, start_worker=False, **kwargs)
        service.recover()
        return service

    def tearDown(self):
        self.stream.close()
        self.temp.cleanup()

    def create(self, recording=RECORDING):
        return self.stream.create({**self.headers, "X-Recording-ID": str(recording)}, FORMAT.copy())

    def put(self, seq, body=None, recording=RECORDING):
        body = self.chunks[seq] if body is None else body
        return self.stream.put_chunk(self.auth, DEVICE, recording, seq,
                                      {"Content-Type": "application/octet-stream", "X-Chunk-SHA256": hashlib.sha256(body).hexdigest()}, body)

    def finish(self, chunks=2, samples=32000, interrupted=False, reason="", recording=RECORDING):
        return self.stream.finish(self.auth, DEVICE, recording, {"chunks": chunks, "samples": samples, "interrupted": interrupted, "reason": reason})

    def status(self, recording=RECORDING):
        return self.stream.status(self.auth, DEVICE, recording)[0]

    def assert_error(self, code, call):
        with self.assertRaises((StreamError, RelayError)) as caught:
            call()
        self.assertEqual(caught.exception.code, code)
        return caught.exception

    def upload(self):
        self.create()
        self.put(0)
        self.put(1)
        self.finish()

    def test_real_libopus_order_and_last_frame_trim(self):
        self.create()
        self.put(0)
        self.put(1)
        value, code = self.finish(samples=31840)
        self.assertEqual((code, value["status"]), (202, "received"))
        self.assertEqual(self.asr_calls, [])  # finish never blocks on provider
        self.stream.run_pending_once()
        self.assertEqual(self.status()["status"], "sent")
        self.assertEqual(len(self.deliveries), 1)
        self.assertEqual(self.deliveries[0][0], "喵伴无卡语音笔记 #1")
        self.assertIn("**喵伴无卡 #1**", self.deliveries[0][1])
        self.assertNotIn(str(RECORDING), self.deliveries[0][1])
        data, duration = self.asr_calls[0]
        with wave.open(io.BytesIO(data), "rb") as wav:
            self.assertEqual((wav.getnframes(), wav.getframerate(), wav.getnchannels()), (31840, 16000, 1))
            pcm = struct.unpack("<" + "h" * 31840, wav.readframes(31840))
        self.assertEqual(duration, 1990)
        # Count sign changes well inside each tone: order is 440 Hz then 660 Hz.
        for start, expected in [(4000, 440), (20000, 660)]:
            section = pcm[start:start + 8000]
            crossings = sum((a < 0) != (b < 0) for a, b in zip(section, section[1:]))
            self.assertAlmostEqual(crossings, expected, delta=4)
            self.assertGreater(sum(x * x for x in section) / len(section), 1_000_000)
        self.assertFalse(self.stream._path(DEVICE, RECORDING).exists())
        self.assertIsNone(self.store.get(DEVICE, RECORDING)["cloud_content"])

    @unittest.skipUnless(os.environ.get("VOICE_MEMO_LONG_STREAM_TEST") == "1",
                         "opt-in 60-minute synthetic Opus/server workload")
    def test_one_hour_stream_and_transcription_chunking(self):
        """Exercise the actual server codec/SQLite/files with provider substitutes."""
        frames_per_chunk = 250  # Five seconds, matching the ESP32 sender.
        chunks = 60 * 60 // 5
        body = synthetic_chunks(frequencies=(440,), frames=frames_per_chunk)[0]
        durations = []

        def transcribe(wav_data, duration):
            assembled = self.stream._path(DEVICE, RECORDING) / "assembled.wav"
            self.assertEqual(assembled.stat().st_size, 60 * 60 * 16000 * 2 + 44)
            with wave.open(io.BytesIO(wav_data), "rb") as audio:
                self.assertEqual(audio.getnframes() * 1000 // 16000, duration)
            durations.append(duration)
            # Distinct text avoids triggering the intentional repetition guard.
            return f"合成片段{len(durations):03d} {hashlib.sha256(str(len(durations)).encode()).hexdigest()[:16]}", None

        self.relay._qwen_transcriber = transcribe
        self.create()
        for seq in range(chunks):
            result, code = self.put(seq, body=body)
            self.assertEqual((code, result["next_seq"], result["received_samples"]),
                             (201, seq + 1, (seq + 1) * frames_per_chunk * 320))
            self.now += 1000  # New chunks keep a live recording from expiring.
        temporary_opus = list(self.stream._path(DEVICE, RECORDING).glob("*.opus"))
        self.assertEqual(len(temporary_opus), chunks)
        self.assertEqual(sum(path.stat().st_size for path in temporary_opus), chunks * len(body))
        total_samples = 60 * 60 * 16000
        result, code = self.finish(chunks=chunks, samples=total_samples)
        self.assertEqual((code, result["duration_ms"]), (202, 3_600_000))
        self.assertTrue(self.stream.run_pending_once())
        self.assertEqual(self.status()["status"], "sent")
        self.assertEqual(sum(durations), 3_600_000)
        self.assertGreaterEqual(len(durations), 20)
        self.assertTrue(all(0 < duration <= 180_000 for duration in durations))
        self.assertEqual(len(self.deliveries), 1)
        self.assertFalse(self.stream._path(DEVICE, RECORDING).exists())
        self.assertIsNone(self.store.get(DEVICE, RECORDING)["cloud_content"])

    def test_chunk_durable_before_ack_duplicate_and_conflict(self):
        self.create()
        events = []
        original_fsync = os.fsync
        def fsync(fd):
            events.append("fsync")
            return original_fsync(fd)
        with mock.patch("stream_service.os.fsync", side_effect=fsync):
            result, code = self.put(0)
        self.assertEqual((code, result["next_seq"], result["received_samples"]), (201, 1, 16000))
        self.assertGreaterEqual(len(events), 3)
        self.assertEqual(self.put(0)[1], 200)
        self.assert_error("chunk_conflict", lambda: self.put(0, self.chunks[1]))
        self.assert_error("chunk_out_of_order", lambda: self.put(2, self.chunks[1]))
        self.stream.close()
        self.stream = self.new_service()
        self.relay.streams = self.stream
        self.assertEqual(self.status()["next_seq"], 1)
        self.put(1)
        self.finish()
        self.stream.run_pending_once()
        self.assertEqual(self.status()["status"], "sent")

    def test_envelope_hash_size_and_frame_duration_validation(self):
        self.create()
        for invalid in [b"", b"\x3c", b"\x3c\x00short", b"\x00\x00", self.chunks[0] + b"\x01", b"x" * (MAX_CHUNK + 1)]:
            self.assert_error("invalid_chunk", lambda: self.put(0, invalid))
        self.assert_error("chunk_hash_mismatch", lambda: self.stream.put_chunk(self.auth, DEVICE, RECORDING, 0,
                          {"Content-Type": "application/octet-stream", "X-Chunk-SHA256": "0" * 64}, self.chunks[0]))
        # Opus TOC 0x08 represents a 20ms SILK frame; TOC 0 is 10ms.
        wrong_duration = struct.pack("<H", 60) + bytes(60)
        self.assert_error("invalid_chunk", lambda: self.put(0, wrong_duration))
        stereo = bytearray(self.chunks[0])
        stereo[2] |= 4
        self.assert_error("invalid_chunk", lambda: self.put(0, bytes(stereo)))
        self.assertEqual(self.status()["next_seq"], 0)
        self.assert_error("unsupported_media_type", lambda: self.stream.put_chunk(self.auth, DEVICE, RECORDING, 0,
                          {"Content-Type": "audio/wav"}, self.chunks[0]))
        with self.assertRaisesRegex(RelayError, "ended before Content-Length"):
            read_exact(io.BytesIO(self.chunks[0][:-1]), len(self.chunks[0]))

    def test_finish_gap_padding_conflict_and_idempotency(self):
        self.create()
        self.put(0)
        self.assert_error("missing_chunks", lambda: self.finish())
        self.assert_error("invalid_finish", lambda: self.finish(chunks=1, samples=16001))
        self.assert_error("invalid_finish", lambda: self.finish(chunks=1, samples=15680))
        self.finish(chunks=1, samples=16000, interrupted=True, reason="power_lost")
        self.assertEqual(self.finish(chunks=1, samples=16000, interrupted=True, reason="power_lost")[1], 202)
        self.assert_error("finish_conflict", lambda: self.finish(chunks=1, samples=16000))
        self.assert_error("stream_closed", lambda: self.put(1))
        self.stream.run_pending_once()
        self.assertEqual(self.finish(chunks=1, samples=16000, interrupted=True, reason="power_lost")[1], 200)
        self.assertTrue(self.status()["interrupted"])
        self.assertIn("仅已接收部分", self.deliveries[0][1])
        self.assertFalse(self.stream.run_pending_once())
        self.assertEqual(len(self.deliveries), 1)

    def test_restart_finished_pending_and_processing_resume(self):
        self.upload()
        self.stream._update(DEVICE, RECORDING, phase="working")
        self.stream.close()
        self.stream = self.new_service()
        self.relay.streams = self.stream
        self.stream.run_pending_once()
        self.assertEqual(self.status()["status"], "sent")
        self.stream.close()
        self.stream = self.new_service()
        self.relay.streams = self.stream
        self.assertFalse(self.stream.run_pending_once())
        self.assertEqual(len(self.deliveries), 1)

    def test_restart_during_delivery_never_sends_twice(self):
        self.upload()
        with self.store.connect() as db:
            db.execute("INSERT INTO memos(device_id,recording_id,audio_sha256,duration_ms,status,cloud_content,created_at,updated_at) "
                       "VALUES(?,?,?,2000,'sending','synthetic',?,?)", (DEVICE, RECORDING, "a" * 64, self.now, self.now))
        self.stream._update(DEVICE, RECORDING, phase="working")
        self.stream.close()
        self.stream = self.new_service()
        self.relay.streams = self.stream
        self.assertEqual(self.status()["status"], "uncertain")
        self.assertFalse(self.status()["can_retry"])
        self.assertFalse(self.stream.run_pending_once())
        self.assert_error("delivery_uncertain", lambda: self.stream.retry(self.auth, DEVICE, RECORDING))
        self.assertEqual(self.deliveries, [])
        resolve_data_fixture(self.store, self.stream, DEVICE, RECORDING, False, lambda: self.now)
        self.stream.retry(self.auth, DEVICE, RECORDING)
        self.stream.run_pending_once()
        self.assertEqual(len(self.deliveries), 1)
        self.assertEqual(self.asr_calls, [])

    def test_expiry_refresh_and_metadata_survival(self):
        self.create()
        self.now += RETENTION_MS - 1000
        self.put(0)
        self.now += 1001
        self.stream.expire()
        self.assertEqual(self.status()["status"], "receiving")
        self.now += RETENTION_MS
        self.stream.expire()
        result = self.status()
        self.assertEqual(result["status"], "audio_expired")
        self.assertEqual(result["next_seq"], 1)
        self.assertFalse(result["can_retry"])
        self.assertFalse(self.stream._path(DEVICE, RECORDING).exists())
        self.assert_error("audio_expired", lambda: self.put(0))
        self.assert_error("audio_expired", lambda: self.put(1))
        self.assert_error("audio_expired", lambda: self.finish(chunks=1, samples=16000))
        self.assert_error("audio_expired", lambda: self.stream.retry(self.auth, DEVICE, RECORDING))
        self.assertEqual(self.create()[0]["status"], "audio_expired")

    def test_active_worker_not_expired_and_retry_on_explicit_request(self):
        self.upload()
        def failed(data, duration):
            self.now += RETENTION_MS + 1
            self.stream.expire()
            self.assertTrue(self.stream._path(DEVICE, RECORDING).exists())
            raise RelayError(502, "qwen_transport", "synthetic network failure")
        self.relay._qwen_transcriber = failed
        self.stream.run_pending_once()
        self.assertEqual(self.status()["status"], "transcription_failed")
        self.assertTrue(self.status()["can_retry"])
        self.assertFalse(self.stream.run_pending_once())
        self.relay._qwen_transcriber = lambda data, duration: ("合成测试正文", None)
        self.assertEqual(self.stream.retry(self.auth, DEVICE, RECORDING)[0]["status"], "received")
        self.stream.retry(self.auth, DEVICE, RECORDING)
        self.stream.run_pending_once()
        self.assertFalse(self.stream.run_pending_once())
        self.assertEqual(self.status()["status"], "sent")
        self.assertEqual(len(self.deliveries), 1)

    def test_delivery_retry_uses_text_without_asr_or_audio(self):
        self.upload()
        def failed(title, content):
            raise RelayError(502, "siyuan_rejected", "synthetic HTTP 401")
        self.relay._siyuan_sender = failed
        self.stream.run_pending_once()
        self.assertEqual(self.status()["status"], "delivery_failed")
        self.assertTrue(self.status()["can_retry"])
        self.now += RETENTION_MS + 1
        self.stream.expire()
        self.assertFalse(self.stream._path(DEVICE, RECORDING).exists())
        self.assertEqual(self.status()["status"], "delivery_failed")
        self.relay._siyuan_sender = lambda title, text: self.deliveries.append((title, text))
        self.stream.retry(self.auth, DEVICE, RECORDING)
        self.stream.run_pending_once()
        self.assertEqual(self.status()["status"], "sent")
        self.assertEqual(len(self.asr_calls), 1)

    def test_no_words_cleans_audio_and_cannot_retry(self):
        self.upload()
        self.relay._qwen_transcriber = lambda data, duration: ("", None)
        self.stream.run_pending_once()
        self.assertEqual(self.status()["status"], "transcription_rejected")
        self.assertFalse(self.status()["can_retry"])
        self.assertFalse(self.stream._path(DEVICE, RECORDING).exists())
        self.assert_error("cannot_retry", lambda: self.stream.retry(self.auth, DEVICE, RECORDING))

    def test_interrupted_zero_audio_reports_rejection(self):
        self.create()
        self.finish(chunks=0, samples=0, interrupted=True, reason="power_lost")
        self.stream.run_pending_once()
        self.assertEqual(self.status()["status"], "transcription_rejected")
        self.assertEqual(self.asr_calls, [])

    def test_device_auth_uint32_collision_and_process_lock(self):
        token = pair_device(self.store, DEVICE, "d" * 64)
        self.auth = "Bearer " + token
        self.headers["Authorization"] = "Bearer " + token
        self.create(recording=0xFFFFFFFF)
        for recording in [0, 4294967296, True]:
            self.assert_error("invalid_recording_id", lambda: self.create(recording=recording))
        self.assert_error("unauthorized", lambda: self.stream.status("Bearer " + token, "other-device", 0xFFFFFFFF))
        self.assert_error("unauthorized", lambda: self.stream.finish("Bearer " + token, "other-device", 0xFFFFFFFF, {}))
        self.assert_error("unauthorized", lambda: self.stream.retry("Bearer " + token, "other-device", 0xFFFFFFFF))
        self.assert_error("unauthorized", lambda: self.stream.put_chunk("Bearer " + token, "other-device", 0xFFFFFFFF, 0, {}, b""))
        with self.assertRaisesRegex(RuntimeError, "another stream worker"):
            self.new_service()
        self.create()
        data = wav_bytes()
        headers = {"Authorization": self.auth, "Content-Type": "audio/wav", "X-Device-ID": DEVICE,
                   "X-Recording-ID": str(RECORDING), "X-Audio-SHA256": hashlib.sha256(data).hexdigest()}
        self.assert_error("recording_conflict", lambda: self.relay.upload(headers, data))
        self.store.create(DEVICE, 100, "a" * 64, 1000, self.now)
        self.assert_error("recording_conflict", lambda: self.create(recording=100))

    def test_file_commit_failure_does_not_acknowledge_or_advance(self):
        self.create()
        with mock.patch("stream_service.os.replace", side_effect=OSError(28, "synthetic full disk")):
            self.assert_error("storage_full", lambda: self.put(0))
        self.assertEqual(self.status()["next_seq"], 0)
        self.assertEqual(self.put(0)[0]["next_seq"], 1)

    def test_database_failure_after_fsync_does_not_acknowledge(self):
        self.create()
        original = _ClosingConnection.execute
        def fail_insert(db, statement, parameters=()):
            if statement.startswith("INSERT INTO stream_chunks"):
                raise sqlite3.OperationalError("synthetic commit failure")
            return original(db, statement, parameters)
        with mock.patch.object(_ClosingConnection, "execute", fail_insert):
            with self.assertRaises(sqlite3.OperationalError):
                self.put(0)
        self.assertEqual(self.status()["next_seq"], 0)
        self.assertTrue((self.stream._path(DEVICE, RECORDING) / "0000000000.opus").exists())
        self.assertEqual(self.put(0)[0]["next_seq"], 1)

    def test_cleanup_failure_persists_for_retry_without_resending(self):
        self.upload()
        with mock.patch("stream_service.shutil.rmtree", side_effect=OSError("synthetic cleanup failure")):
            with self.assertRaises(OSError):
                self.stream.run_pending_once()
        self.assertEqual(self.stream._row(DEVICE, RECORDING)["phase"], "cleanup")
        self.stream.expire()
        self.assertEqual(self.stream._row(DEVICE, RECORDING)["phase"], "done")
        self.assertFalse(self.stream._path(DEVICE, RECORDING).exists())
        self.assertEqual(len(self.deliveries), 1)

    def test_corrupted_acknowledged_chunk_never_delivered(self):
        self.upload()
        (self.stream._path(DEVICE, RECORDING) / "0000000000.opus").write_bytes(b"broken")
        self.stream.run_pending_once()
        self.assertEqual(self.status()["status"], "transcription_failed")
        self.assertIn("stream_corrupt", self.status()["error"])
        self.assertFalse(self.status()["can_retry"])
        self.assertEqual(self.deliveries, [])

    def test_restart_in_asr_reuses_acknowledged_audio(self):
        self.upload()
        self.relay._qwen_transcriber = mock.Mock(side_effect=SystemExit("simulated crash"))
        with self.assertRaises(SystemExit):
            self.stream.run_pending_once()
        self.assertEqual(self.status()["status"], "transcribing")
        self.stream.close()
        self.stream = self.new_service()
        self.relay.streams = self.stream
        self.relay._qwen_transcriber = lambda data, duration: ("恢复后的合成测试正文", None)
        self.stream.run_pending_once()
        self.assertEqual(self.status()["status"], "sent")
        self.assertEqual(len(self.deliveries), 1)

    def test_background_worker_no_duplicate_and_expiry_does_not_interrupt(self):
        self.stream.close()
        self.stream = StreamService(self.relay, clock=lambda: self.now, start_worker=True)
        self.relay.streams = self.stream
        entered, release = threading.Event(), threading.Event()
        calls = []
        def blocking(data, duration):
            calls.append(duration)
            entered.set()
            if not release.wait(5):
                raise RuntimeError("test failed to release provider")
            return "后台合成测试正文", None
        self.relay._qwen_transcriber = blocking
        try:
            self.upload()
            self.assertTrue(entered.wait(3))
            self.assertEqual(self.status()["status"], "transcribing")
            self.assertEqual(self.stream.retry(self.auth, DEVICE, RECORDING)[1], 202)
            self.finish()
            self.now += RETENTION_MS + 1
            self.stream.expire()
            self.assertTrue(self.stream._path(DEVICE, RECORDING).exists())
        finally:
            release.set()
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline and self.status()["status"] != "sent":
            time.sleep(0.01)
        self.assertEqual(self.status()["status"], "sent")
        self.assertEqual(calls, [2000])
        self.assertEqual(len(self.deliveries), 1)

    def test_uncertain_reconciliation_cleans_audio_without_redelivery(self):
        self.upload()
        def uncertain(title, content):
            raise RelayError(503, "siyuan_uncertain", "synthetic timeout")
        self.relay._siyuan_sender = uncertain
        self.stream.run_pending_once()
        self.assertEqual(self.status()["status"], "uncertain")
        self.assertTrue(self.stream._path(DEVICE, RECORDING).exists())
        resolve_data_fixture(self.store, self.stream, DEVICE, RECORDING, True, lambda: self.now)
        self.assertEqual(self.status()["status"], "sent")
        self.assertFalse(self.stream._path(DEVICE, RECORDING).exists())
        self.assertEqual(self.deliveries, [])



if __name__ == "__main__":
    unittest.main()
