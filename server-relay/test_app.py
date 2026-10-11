import uuid
from relay.identity import IdentityError
from test_support import pair_device, resolve_data_fixture
import hashlib
import io
import pathlib
import tempfile
import unittest
import urllib.error
import wave
from unittest import mock

from app import (
    MemoStore,
    RelayError,
    RelayService,
    clean_transcript,
    iter_wav_chunks,
    quality_issues,
    read_exact,
    read_to_temporary,
    validate_wav,
)


def wav_bytes(seconds: int = 1, sample: int = 1000) -> bytes:
    stream = io.BytesIO()
    with wave.open(stream, "wb") as audio:
        audio.setnchannels(1)
        audio.setsampwidth(2)
        audio.setframerate(16000)
        audio.writeframes(sample.to_bytes(2, "little", signed=True) * (seconds * 16000))
    return stream.getvalue()


class RelayTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.store = MemoStore(pathlib.Path(self.temp.name) / "state.sqlite3")
        pair_device(self.store, "meow-vocat-1", "a" * 64)
        self.qwen_calls = 0
        self.siyuan_calls = 0

        def qwen(data, duration):
            self.qwen_calls += 1
            return clean_transcript("你好喵伴，云端测试正文。你好喵伴保存录音"), "qwen-1"

        def siyuan(title, content):
            self.siyuan_calls += 1
            self.assertEqual(title, "喵伴语音笔记 #179")
            self.assertIn("云端测试正文", content)

        self.service = RelayService(
            self.store, "q" * 32, "s" * 32,
            qwen_transcriber=qwen, siyuan_sender=siyuan,
        )

    def tearDown(self):
        self.temp.cleanup()

    def headers(self, data: bytes, recording=179):
        return {
            "Authorization": "Bearer " + "a" * 64,
            "Content-Type": "audio/wav",
            "X-Device-ID": "meow-vocat-1",
            "X-Recording-ID": str(recording),
            "X-Audio-SHA256": hashlib.sha256(data).hexdigest(),
        }

    def test_wav_and_cleaning_and_quality(self):
        self.assertEqual(validate_wav(wav_bytes(2))["duration_ms"], 2000)
        with self.assertRaises(RelayError):
            validate_wav(b"not a wav")
        self.assertEqual(clean_transcript("你好喵伴，正文。你好喵伴保存录音"), "正文")
        self.assertEqual(quality_issues("", 1000), ["empty"])

    def test_qwen_http_error_keeps_bounded_structured_diagnostics(self):
        body = io.BytesIO(
            b'{"code":"InvalidParameter","message":"audio request rejected",'
            b'"request_id":"req-123","input_audio":"must-not-be-retained"}'
        )
        upstream = urllib.error.HTTPError("https://example.invalid", 400, "bad", {}, body)
        with mock.patch("app.urllib.request.urlopen", side_effect=upstream):
            with self.assertRaises(RelayError) as caught:
                self.service._call_qwen(wav_bytes(), 1000)
        message = str(caught.exception)
        self.assertIn("HTTP 400", message)
        self.assertIn("code=InvalidParameter", message)
        self.assertIn("message=audio request rejected", message)
        self.assertIn("request_id=req-123", message)
        self.assertNotIn("input_audio", message)
        self.assertNotIn("must-not-be-retained", message)

    def test_qwen_no_words_is_a_terminal_quality_rejection(self):
        body = io.BytesIO(
            b'{"code":"CLIENT_ERROR","message":"ASR_RESPONSE_HAVE_NO_WORDS",'
            b'"request_id":"req-empty"}'
        )
        upstream = urllib.error.HTTPError("https://example.invalid", 400, "bad", {}, body)
        with mock.patch("app.urllib.request.urlopen", side_effect=upstream):
            with self.assertRaises(RelayError) as caught:
                self.service._call_qwen(wav_bytes(), 1000)
        self.assertEqual((caught.exception.status, caught.exception.code), (422, "transcript_rejected"))
        self.assertEqual(str(caught.exception), "empty")

    def test_streamed_request_body_is_read_completely(self):
        class FragmentedStream:
            def __init__(self, data):
                self.data = data

            def read(self, size):
                block, self.data = self.data[:7], self.data[7:]
                return block

        self.assertEqual(read_exact(FragmentedStream(b"RIFF" + b"x" * 96), 100), b"RIFF" + b"x" * 96)
        with self.assertRaisesRegex(RelayError, "ended before Content-Length"):
            read_exact(io.BytesIO(b"short"), 10)

    def test_streamed_upload_uses_a_transient_file(self):
        data = wav_bytes(2)
        path, digest = read_to_temporary(io.BytesIO(data), len(data))
        try:
            self.assertEqual(digest, hashlib.sha256(data).hexdigest())
            self.assertEqual(validate_wav(path)["duration_ms"], 2000)
        finally:
            path.unlink()

    def test_recording_longer_than_three_minutes_is_chunked(self):
        data = wav_bytes(182)
        self.assertEqual(validate_wav(data)["duration_ms"], 182000)
        self.assertEqual(len(list(iter_wav_chunks(data))), 2)
        result, status = self.service.upload(self.headers(data), data, now=1_000_000)
        self.assertEqual((status, result["status"]), (201, "sent"))
        self.assertEqual(self.qwen_calls, 2)

    def test_silent_leading_chunks_do_not_discard_later_speech(self):
        calls = []

        def qwen(data, duration):
            calls.append((data, duration))
            if data == b"silent-error":
                raise RelayError(422, "transcript_rejected", "empty")
            if data == b"silent-blank":
                return "  ", "blank-request"
            return "后面的对话正文", "speech-request"

        self.service._qwen_transcriber = qwen
        chunks = [(b"silent-error", 180000), (b"silent-blank", 180000), (b"speech", 120000)]
        with mock.patch("app.iter_wav_chunks", return_value=iter(chunks)):
            transcript, request_ids = self.service._transcribe_source(b"unused", 480000)
        self.assertEqual(transcript, "后面的对话正文")
        self.assertEqual(request_ids, "speech-request")
        self.assertEqual(calls, chunks)

    def test_all_silent_chunks_still_fail_the_whole_recording(self):
        def qwen(data, duration):
            raise RelayError(422, "transcript_rejected", "empty")

        self.service._qwen_transcriber = qwen
        chunks = [(b"silent-1", 180000), (b"silent-2", 120000)]
        with mock.patch("app.iter_wav_chunks", return_value=iter(chunks)):
            with self.assertRaises(RelayError) as caught:
                self.service._transcribe_source(b"unused", 300000)
        self.assertEqual((caught.exception.status, caught.exception.code, str(caught.exception)),
                         (422, "transcript_rejected", "empty"))

    def test_authentication(self):
        with self.assertRaisesRegex(RelayError, "device authorization"):
            self.service.authorize("Bearer wrong")

    def test_one_time_pairing_stores_only_hash_and_binds_device_token(self):
        code, expiry = self.store.issue_pairing_code(900, now=1_000_000)
        self.assertEqual(expiry, 1_900_000)
        identifier, token = str(uuid.uuid4()), "b" * 64
        identity = self.store.identity
        identity.create_enrollment("meow-vocat-aabbcc", code.lower(), identifier, token, 1_000_001)
        self.assertFalse(self.store.device_token_valid("meow-vocat-aabbcc", token, now=1_000_002))
        identity.activate_enrollment(identifier, token, 1_000_002)
        identity.finalize_enrollment(identifier, token, 1_000_003)
        with self.store.connect() as db:
            row = db.execute("SELECT token_sha256 FROM devices WHERE device_id=?", ("meow-vocat-aabbcc",)).fetchone()
        self.assertNotEqual(row[0], token)
        self.assertTrue(self.store.device_token_valid("meow-vocat-aabbcc", token, now=1_000_004))
        self.assertFalse(self.store.device_token_valid("other", token, now=1_000_004))
        with self.assertRaises(IdentityError):
            identity.create_enrollment("other", code, str(uuid.uuid4()), "c" * 64, 1_000_004)

    def test_expired_pairing_code_is_rejected(self):
        code, _ = self.store.issue_pairing_code(60, now=1_000_000)
        with self.assertRaises(IdentityError):
            self.store.identity.create_enrollment("another", code, str(uuid.uuid4()), "b" * 64, 1_060_001)
        with self.store.connect() as db:
            attempts = db.execute("SELECT attempts FROM pairing_codes WHERE code_sha256=?", (self.store._secret_hash(code),)).fetchone()[0]
        self.assertEqual(attempts, 1)

    def test_device_token_upload_is_bound_and_cannot_reconcile(self):
        token = pair_device(self.store, "meow-vocat-aabbcc", "b" * 64)
        data = wav_bytes(); headers = self.headers(data)
        headers.update(Authorization="Bearer " + token, **{"X-Device-ID": "meow-vocat-aabbcc"})
        result, status = self.service.upload(headers, data)
        self.assertEqual((status, result["status"]), (201, "sent"))
        headers["X-Device-ID"] = "other"
        with self.assertRaises(RelayError): self.service.upload(headers, data)
        with self.assertRaises(RelayError): self.service.authorize("Bearer " + token, "meow-vocat-aabbcc", admin=True)

    def test_upload_sends_once_and_duplicate_is_idempotent(self):
        data = wav_bytes()
        first, status = self.service.upload(self.headers(data), data, now=1_000_000)
        self.assertEqual((status, first["status"], first["duplicate"]), (201, "sent", False))
        duplicate, status = self.service.upload(self.headers(data), data, now=1_000_001)
        self.assertEqual((status, duplicate["status"], duplicate["duplicate"]), (200, "sent", True))
        self.assertEqual((self.qwen_calls, self.siyuan_calls), (1, 1))
        row = self.store.get("meow-vocat-1", 179)
        self.assertIsNone(row["transcript"])
        self.assertIsNone(row["cloud_content"])
        self.assertRegex(row["transcript_sha256"], r"^[a-f0-9]{64}$")

    def test_declared_siyuan_failure_reuses_transcript(self):
        data = wav_bytes()

        def fail_once(title, content):
            self.siyuan_calls += 1
            if self.siyuan_calls == 1:
                raise RelayError(502, "siyuan_rejected", "declared failure")

        self.service._siyuan_sender = fail_once
        with self.assertRaisesRegex(RelayError, "declared failure"):
            self.service.upload(self.headers(data), data, now=1_000_000)
        self.assertEqual(self.store.get("meow-vocat-1", 179)["status"], "delivery_failed")
        recovered, status = self.service.upload(self.headers(data), data, now=1_000_001)
        self.assertEqual((status, recovered["status"]), (201, "sent"))
        self.assertEqual((self.qwen_calls, self.siyuan_calls), (1, 2))

    def test_uncertain_delivery_requires_reconciliation(self):
        data = wav_bytes()

        def uncertain(title, content):
            raise RelayError(503, "siyuan_uncertain", "timeout")

        self.service._siyuan_sender = uncertain
        with self.assertRaises(RelayError):
            self.service.upload(self.headers(data), data, now=1_000_000)
        self.assertEqual(self.store.get("meow-vocat-1", 179)["status"], "uncertain")
        with self.assertRaisesRegex(RelayError, "reconcile"):
            self.service.upload(self.headers(data), data, now=1_000_001)
        reconciled, status = resolve_data_fixture(self.store, type("NoStreams", (), {"reconciled": lambda *_: None})(), "meow-vocat-1", 179, True, lambda: 1_000_002)
        self.assertEqual((status, reconciled["status"]), (200, "sent"))
        row = self.store.get("meow-vocat-1", 179)
        self.assertIsNone(row["transcript"])
        self.assertIsNone(row["cloud_content"])

    def test_recording_conflict(self):
        data = wav_bytes()
        self.service.upload(self.headers(data), data, now=1_000_000)
        changed = wav_bytes(sample=500)
        with self.assertRaisesRegex(RelayError, "different audio"):
            self.service.upload(self.headers(changed), changed, now=1_000_001)


if __name__ == "__main__":
    unittest.main()
