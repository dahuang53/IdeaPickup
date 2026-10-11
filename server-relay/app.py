#!/usr/bin/env python3
"""Private HTTP relay for ESP-VoCat audio transcription and SiYuan delivery."""

from __future__ import annotations

import base64
import argparse
import array
import hashlib
import hmac
import io
import json
import os
import pathlib
import re
import secrets
import sqlite3
import sys
import tempfile
import time
import urllib.error
import urllib.request
import wave
from relay.identity import IdentityStore
from typing import BinaryIO, Callable, Mapping

from stream_service import MAX_CHUNK, STREAM_PATH, StreamError, StreamService


QWEN_URL = "https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation"
SIYUAN_URL = "https://ld246.com/apis/siyuan/inbox/addCloudShorthand"
MODEL = "qwen-audio-3.0-asr-flash"
# Match the recorder's signed 32-bit FAT/newlib file boundary. This is a file
# format/transport boundary, not a product recording-duration policy.
MAX_WAV_BYTES = 2_147_483_646
QWEN_CHUNK_SECONDS = 180
QUIET_SEARCH_SECONDS = 12
STALE_PROCESSING_MS = 10 * 60 * 1000

WAKE_PHRASE = r"(?:你好喵伴|喵喵同学|小鹿小鹿)"
CONTROL_START = re.compile(rf"^[\s，。！？、,.!?]*{WAKE_PHRASE}[\s，。！？、,.!?：:]*")
CONTROL_END_COMMAND = re.compile(
    rf"(?:[\s，。！？、,.!?：:]*{WAKE_PHRASE})*[\s，。！？、,.!?：:]*"
    rf"(?:保存录音|结束录音|停止录音|保存)[\s，。！？、,.!?]*$"
)
CONTROL_END_WAKE = re.compile(rf"(?:[\s，。！？、,.!?：:]*{WAKE_PHRASE})+[\s，。！？、,.!?]*$")
IDENTITY_PATH = re.compile(r"^/v1/memos/([A-Za-z0-9._-]{1,64})/([1-9][0-9]{0,9})(/reconcile)?$")
DEVICE_ID = re.compile(r"^[A-Za-z0-9._-]{1,64}$")
AUDIO_HASH = re.compile(r"^[a-f0-9]{64}$")
PAIRING_CODE = re.compile(r"^[A-HJ-NP-Z2-9]{10}$")
PAIRING_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
DEVICE_TOKEN_BYTES = 32


class RelayError(Exception):
    def __init__(self, status: int, code: str, message: str):
        super().__init__(message)
        self.status = status
        self.code = code


def clean_transcript(text: str) -> str:
    value = text.replace("\r\n", "\n").replace("\r", "\n").strip()
    value = CONTROL_START.sub("", value, count=1)
    value = CONTROL_END_COMMAND.sub("", value, count=1)
    value = CONTROL_END_WAKE.sub("", value, count=1)
    return re.sub(r"^[\s，。！？、,.!?]+|[\s，。！？、,.!?]+$", "", value)


def quality_issues(text: str, duration_ms: int) -> list[str]:
    compact = re.sub(r"\s", "", text)
    if not compact:
        return ["empty"]
    issues: list[str] = []
    if len(compact) > max(80, (duration_ms * 12 + 999) // 1000):
        issues.append("implausible-length")
    if len(compact) >= 48:
        grams = [compact[index:index + 8] for index in range(len(compact) - 7)]
        repeated_fraction = 1 - len(set(grams)) / len(grams)
        if repeated_fraction > 0.8:
            issues.append("repetition-loop")
    return issues


def _open_wav(source: bytes | pathlib.Path):
    return wave.open(io.BytesIO(source), "rb") if isinstance(source, bytes) else wave.open(str(source), "rb")


def source_size(source: bytes | pathlib.Path) -> int:
    return len(source) if isinstance(source, bytes) else source.stat().st_size


def validate_wav(source: bytes | pathlib.Path) -> dict:
    if source_size(source) > MAX_WAV_BYTES:
        raise RelayError(413, "audio_too_large", f"maximum WAV size is {MAX_WAV_BYTES} bytes")
    try:
        with _open_wav(source) as audio:
            channels = audio.getnchannels()
            sample_width = audio.getsampwidth()
            sample_rate = audio.getframerate()
            frames = audio.getnframes()
            compression = audio.getcomptype()
            if channels != 1 or sample_width != 2 or sample_rate != 16_000 or compression != "NONE":
                raise RelayError(400, "invalid_wav", "expected 16 kHz, 16-bit, mono PCM WAV")
            if frames <= 0:
                raise RelayError(400, "invalid_wav", "WAV contains no audio frames")
            # Header parsing alone accepts truncated PCM. Read in bounded blocks
            # before publishing any trusted audio identity or successful ACK.
            remaining = frames
            while remaining:
                count = min(remaining, 32768)
                if len(audio.readframes(count)) != count * channels * sample_width:
                    raise RelayError(400, "invalid_wav", "WAV PCM data is incomplete")
                remaining -= count
            if audio.readframes(1):
                raise RelayError(400, "invalid_wav", "WAV PCM data contains an incomplete sample")
    except (EOFError, wave.Error) as error:
        raise RelayError(400, "invalid_wav", "expected RIFF/WAVE audio") from error
    duration_ms = round(frames * 1000 / sample_rate)
    return {"duration_ms": duration_ms, "frames": frames}


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_source(source: bytes | pathlib.Path) -> str:
    if isinstance(source, bytes):
        return sha256_bytes(source)
    digest = hashlib.sha256()
    with source.open("rb") as stream:
        for block in iter(lambda: stream.read(64 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _wav_chunk(frames: bytes) -> bytes:
    output = io.BytesIO()
    with wave.open(output, "wb") as target:
        target.setnchannels(1)
        target.setsampwidth(2)
        target.setframerate(16_000)
        target.writeframes(frames)
    return output.getvalue()


def iter_wav_chunks(source: bytes | pathlib.Path, chunk_seconds: int = QWEN_CHUNK_SECONDS):
    """Yield contiguous WAV chunks, preferring a quiet boundary near each limit."""
    with _open_wav(source) as audio:
        total_frames = audio.getnframes()
        maximum_frames = chunk_seconds * 16_000
        start = 0
        while start < total_frames:
            hard_end = min(start + maximum_frames, total_frames)
            end = hard_end
            if hard_end < total_frames:
                search_start = max(start + 16_000, hard_end - QUIET_SEARCH_SECONDS * 16_000)
                audio.setpos(search_start)
                window = audio.readframes(hard_end - search_start)
                samples = array.array("h")
                samples.frombytes(window)
                if sys.byteorder != "little":
                    samples.byteswap()
                block = 320  # 20 ms at 16 kHz
                best_energy = None
                for offset in range(0, max(1, len(samples) - block + 1), block):
                    candidate = samples[offset:offset + block]
                    if len(candidate) < block:
                        break
                    energy = sum(abs(value) for value in candidate)
                    if best_energy is None or energy < best_energy:
                        best_energy = energy
                        end = search_start + offset + block
            if end <= start:
                raise RelayError(500, "chunking_failed", "audio chunking made no progress")
            audio.setpos(start)
            frames = audio.readframes(end - start)
            expected = (end - start) * 2
            if len(frames) != expected:
                raise RelayError(400, "invalid_wav", "WAV audio data is truncated")
            yield _wav_chunk(frames), round((end - start) * 1000 / 16_000)
            start = end


def read_to_temporary(stream: BinaryIO, length: int) -> tuple[pathlib.Path, str]:
    """Receive one authenticated upload without retaining audio after the request."""
    descriptor, name = tempfile.mkstemp(prefix="meow-memo-", suffix=".wav")
    path = pathlib.Path(name)
    digest = hashlib.sha256()
    remaining = length
    try:
        with os.fdopen(descriptor, "wb") as output:
            while remaining:
                block = stream.read(min(64 * 1024, remaining))
                if not block:
                    raise RelayError(400, "incomplete_body", "request body ended before Content-Length")
                output.write(block)
                digest.update(block)
                remaining -= len(block)
        return path, digest.hexdigest()
    except Exception:
        path.unlink(missing_ok=True)
        raise


def read_exact(stream: BinaryIO, length: int) -> bytes:
    """Read a streamed request body completely or reject a truncated upload."""
    body = bytearray()
    while len(body) < length:
        block = stream.read(min(64 * 1024, length - len(body)))
        if not block:
            raise RelayError(400, "incomplete_body", "request body ended before Content-Length")
        body.extend(block)
    return bytes(body)


class _ClosingConnection(sqlite3.Connection):
    def __exit__(self, *args):
        try:
            return super().__exit__(*args)
        finally:
            self.close()


class MemoStore:
    ALLOWED_FIELDS = {
        "status", "transcript", "cloud_content", "transcript_sha256", "qwen_request_id",
        "error", "updated_at", "sent_at",
        "audio_bytes",
    }

    def __init__(self, path: pathlib.Path):
        self.path = path
        self.identity = IdentityStore(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with self.connect() as connection:
            connection.execute("PRAGMA journal_mode=WAL")
            connection.execute("""
                CREATE TABLE IF NOT EXISTS memos (
                    device_id TEXT NOT NULL,
                    recording_id INTEGER NOT NULL,
                    audio_sha256 TEXT NOT NULL,
                    duration_ms INTEGER NOT NULL,
                    status TEXT NOT NULL,
                    transcript TEXT,
                    cloud_content TEXT,
                    transcript_sha256 TEXT,
                    qwen_request_id TEXT,
                    audio_bytes INTEGER NOT NULL DEFAULT 0,
                    error TEXT,
                    created_at INTEGER NOT NULL,
                    updated_at INTEGER NOT NULL,
                    sent_at INTEGER,
                    PRIMARY KEY (device_id, recording_id)
                )
            """)
            connection.execute("CREATE INDEX IF NOT EXISTS memos_status_idx ON memos(status, updated_at)")

    def connect(self) -> sqlite3.Connection:
        connection = sqlite3.connect(self.path, timeout=15, factory=_ClosingConnection)
        connection.row_factory = sqlite3.Row
        connection.execute("PRAGMA synchronous=FULL")
        return connection

    def get(self, device_id: str, recording_id: int) -> dict | None:
        with self.connect() as connection:
            row = connection.execute(
                "SELECT * FROM memos WHERE device_id = ? AND recording_id = ?",
                (device_id, recording_id),
            ).fetchone()
        return dict(row) if row else None

    def create(self, device_id: str, recording_id: int, audio_hash: str, duration_ms: int, now: int) -> tuple[bool, dict]:
        with self.connect() as connection:
            connection.execute("BEGIN IMMEDIATE")
            # A stream owns its identity through its entire metadata lifetime.
            # Reject the old WAV route before it can race a stream worker.
            if connection.execute("SELECT 1 FROM sqlite_master WHERE type='table' AND name='streams'").fetchone():
                if connection.execute("SELECT 1 FROM streams WHERE device_id=? AND recording_id=?",
                                      (device_id, recording_id)).fetchone():
                    raise RelayError(409, "recording_conflict", "ID belongs to an Opus stream")
            cursor = connection.execute(
                "INSERT OR IGNORE INTO memos "
                "(device_id, recording_id, audio_sha256, duration_ms, status, created_at, updated_at) "
                "VALUES (?, ?, ?, ?, 'received', ?, ?)",
                (device_id, recording_id, audio_hash, duration_ms, now, now),
            )
            created = cursor.rowcount == 1
        return created, self.get(device_id, recording_id)  # type: ignore[return-value]

    def update(self, device_id: str, recording_id: int, **fields) -> dict:
        if not fields or not set(fields).issubset(self.ALLOWED_FIELDS):
            raise ValueError("invalid memo update")
        setters = ", ".join(f"{name} = ?" for name in fields)
        values = list(fields.values()) + [device_id, recording_id]
        with self.connect() as connection:
            connection.execute(
                f"UPDATE memos SET {setters} WHERE device_id = ? AND recording_id = ?",
                values,
            )
        return self.get(device_id, recording_id)  # type: ignore[return-value]

    @staticmethod
    def _secret_hash(value: str) -> str:
        return hashlib.sha256(value.encode()).hexdigest()

    def issue_pairing_code(self, ttl_seconds: int = 900, now: int | None = None) -> tuple[str, int]:
        return self.identity.issue_pairing_code(ttl_seconds, now)


    def device_token_valid(self, device_id: str, token: str, now: int | None = None) -> bool:
        return self.identity.device_token_valid(device_id, token, now)

    def revoke_device(self, device_id: str, now: int | None = None) -> bool:
        return self.identity.revoke_device(device_id, now)


class RelayService:
    def __init__(
        self,
        store: MemoStore,
        qwen_api_key: str,
        siyuan_cloud_token: str,
        *,
        qwen_transcriber: Callable[[bytes, int], tuple[str, str | None]] | None = None,
        siyuan_sender: Callable[[str, str], None] | None = None,
        qwen_url: str = QWEN_URL,
        siyuan_url: str = SIYUAN_URL,
        model: str = MODEL,
        http_open=None,
        progress=None,
    ):
        self.store = store
        self.qwen_api_key = qwen_api_key
        self.siyuan_cloud_token = siyuan_cloud_token
        self.qwen_url, self.siyuan_url, self.model = qwen_url, siyuan_url, model
        self._http_open = http_open
        self._progress = progress or (lambda: None)
        self._qwen_transcriber = qwen_transcriber or self._call_qwen
        self._siyuan_sender = siyuan_sender or self._send_siyuan
        self.streams: StreamService | None = None

    def authorize(self, authorization, device_id=None, *, admin=False):
        candidate = authorization[7:] if isinstance(authorization, str) and authorization.startswith("Bearer ") else ""
        if not admin and device_id and self.store.device_token_valid(device_id, candidate):
            return device_id
        raise RelayError(401, "unauthorized", "valid device authorization required")


    def public_memo(self, row: Mapping, duplicate: bool = False) -> dict:
        return {
            "server_instance_uuid": self.store.identity.server_instance_uuid,
            "device_id": row["device_id"],
            "recording_id": row["recording_id"],
            "audio_sha256": row["audio_sha256"],
            "audio_bytes": row.get("audio_bytes", 0),
            "duration_ms": row["duration_ms"],
            "status": row["status"],
            "model": self.model,
            "transcript_sha256": row["transcript_sha256"],
            "duplicate": duplicate,
        }

    @staticmethod
    def _parse_qwen_text(result: dict) -> str | None:
        output = result.get("output") or {}
        text = output.get("text")
        if not text:
            text = (((output.get("output") or {}).get("sentence") or {}).get("text"))
        return text if isinstance(text, str) else None

    def _call_qwen(self, wav_data: bytes, duration_ms: int) -> tuple[str, str | None]:
        payload = {
            "model": self.model,
            "input": {"messages": [{
                "role": "user",
                "content": [{"type": "input_audio", "input_audio": {
                    "data": "data:audio/wav;base64," + base64.b64encode(wav_data).decode("ascii")
                }}],
            }]},
            "parameters": {"format": "wav", "sample_rate": "16000", "language_hints": ["zh"]},
        }
        request = urllib.request.Request(
            self.qwen_url,
            data=json.dumps(payload, ensure_ascii=False).encode(),
            method="POST",
            headers={
                "Authorization": f"Bearer {self.qwen_api_key}",
                "Content-Type": "application/json",
                "X-DashScope-SSE": "disable",
                "User-Agent": "MeowVoiceMemoRelay/0.1",
            },
        )
        try:
            with (self._http_open or urllib.request.urlopen)(request, timeout=240) as response:
                response_data = response.read(2 * 1024 * 1024 + 1)
                if len(response_data) > 2 * 1024 * 1024:
                    raise RelayError(502, "qwen_invalid_response", "Qwen response exceeds limit")
        except urllib.error.HTTPError as error:
            info = self._qwen_error_info(error)
            error.close()
            if info.get("message") == "ASR_RESPONSE_HAVE_NO_WORDS":
                raise RelayError(422, "transcript_rejected", "empty") from error
            detail = "; ".join(f"{name}={value}" for name, value in info.items())
            suffix = f" ({detail})" if detail else ""
            raise RelayError(502, "qwen_rejected", f"Qwen request failed: HTTP {error.code}{suffix}") from error
        except (urllib.error.URLError, TimeoutError, OSError) as error:
            raise RelayError(502, "qwen_transport", f"Qwen transport failed: {type(error).__name__}") from error
        try:
            result = json.loads(response_data)
        except (json.JSONDecodeError, UnicodeDecodeError) as error:
            raise RelayError(502, "qwen_invalid_response", "Qwen returned non-JSON data") from error
        raw_text = self._parse_qwen_text(result)
        if not raw_text or not raw_text.strip():
            raise RelayError(422, "transcript_rejected", "empty")
        transcript = clean_transcript(raw_text)
        issues = quality_issues(transcript, duration_ms)
        if issues:
            raise RelayError(422, "transcript_rejected", ",".join(issues))
        return transcript, result.get("request_id")

    @staticmethod
    def _qwen_error_info(error: urllib.error.HTTPError) -> dict[str, str]:
        """Keep bounded provider diagnostics without retaining request audio."""
        try:
            response_data = error.read(8192)
            result = json.loads(response_data)
        except (OSError, json.JSONDecodeError, UnicodeDecodeError):
            return {}
        if not isinstance(result, dict):
            return {}
        fields: dict[str, str] = {}
        for name in ("code", "message", "request_id"):
            value = result.get(name)
            if not isinstance(value, (str, int, float)):
                continue
            clean = re.sub(r"[\x00-\x1f\x7f]+", " ", str(value)).strip()[:240]
            if clean:
                fields[name] = clean
        return fields

    def _transcribe_source(self, source: bytes | pathlib.Path, duration_ms: int) -> tuple[str, str | None]:
        texts: list[str] = []
        request_ids: list[str] = []
        chunk_duration_total = 0
        for wav_data, chunk_duration_ms in iter_wav_chunks(source):
            self._progress()
            chunk_duration_total += chunk_duration_ms
            try:
                text, request_id = self._qwen_transcriber(wav_data, chunk_duration_ms)
                self._progress()
            except RelayError as error:
                # A long memo may contain intentional silence before or between
                # spoken sections. Qwen reports an individual silent chunk as
                # ASR_RESPONSE_HAVE_NO_WORDS, mapped by _call_qwen to this exact
                # terminal-quality result. Skip only that chunk; the whole memo
                # is still rejected by the final quality gate when every chunk
                # is empty.
                if error.code == "transcript_rejected" and str(error) == "empty":
                    continue
                raise
            issues = quality_issues(text, chunk_duration_ms)
            if issues == ["empty"]:
                continue
            if issues:
                raise RelayError(422, "transcript_rejected", ",".join(issues))
            texts.append(text)
            if request_id:
                request_ids.append(request_id)
        if abs(chunk_duration_total - duration_ms) > 1:
            raise RelayError(500, "chunking_failed", "audio chunk durations do not cover the recording")
        transcript = clean_transcript("\n\n".join(texts))
        issues = quality_issues(transcript, duration_ms)
        if issues:
            raise RelayError(422, "transcript_rejected", ",".join(issues))
        return transcript, ",".join(request_ids) or None

    def _send_siyuan(self, title: str, content: str) -> None:
        request = urllib.request.Request(
            self.siyuan_url,
            data=json.dumps({"title": title, "content": content}, ensure_ascii=False).encode(),
            method="POST",
            headers={
                "Authorization": f"token {self.siyuan_cloud_token}",
                "Content-Type": "application/json",
                "User-Agent": "MeowInboxSync/0.1",
            },
        )
        try:
            with (self._http_open or urllib.request.urlopen)(request, timeout=30) as response:
                status = response.status
                response_data = response.read(65537)
                if len(response_data) > 65536:
                    raise RelayError(503, "siyuan_uncertain", "SiYuan response exceeds limit")
        except urllib.error.HTTPError as error:
            status = error.code
            error.close()
            if status >= 500:
                raise RelayError(503, "siyuan_uncertain", f"SiYuan HTTP {status}") from error
            raise RelayError(502, "siyuan_rejected", f"SiYuan rejected request: HTTP {status}") from error
        except (urllib.error.URLError, TimeoutError, OSError) as error:
            raise RelayError(503, "siyuan_uncertain", f"SiYuan result uncertain: {type(error).__name__}") from error
        try:
            result = json.loads(response_data)
        except (json.JSONDecodeError, UnicodeDecodeError) as error:
            raise RelayError(503, "siyuan_uncertain", f"SiYuan returned non-JSON data: HTTP {status}") from error
        if result.get("code") != 0:
            raise RelayError(502, "siyuan_rejected", f"SiYuan rejected request: {result.get('code', status)}")

    def _deliver(self, row: dict, now: int, *, title: str | None = None) -> tuple[dict, int]:
        self.store.update(row["device_id"], row["recording_id"], status="sending", error=None, updated_at=now)
        try:
            self._siyuan_sender(title or f"喵伴语音笔记 #{row['recording_id']}", row["cloud_content"])
        except RelayError as error:
            self.store.update(
                row["device_id"], row["recording_id"],
                status="uncertain" if error.code == "siyuan_uncertain" else "delivery_failed",
                error=f"{error.code}: {error}", updated_at=int(time.time() * 1000),
            )
            raise
        sent_at = int(time.time() * 1000)
        sent = self.store.update(
            row["device_id"], row["recording_id"], status="sent", transcript=None,
            cloud_content=None, error=None, updated_at=sent_at, sent_at=sent_at,
        )
        return self.public_memo(sent), 201

    def preflight_upload(self, headers: Mapping[str, str]) -> tuple[str, int, str]:
        if (headers.get("Content-Type") or "").split(";", 1)[0] != "audio/wav":
            raise RelayError(415, "unsupported_media_type", "Content-Type must be audio/wav")
        device_id = headers.get("X-Device-ID", "")
        recording_text = headers.get("X-Recording-ID", "")
        claimed_hash = headers.get("X-Audio-SHA256", "").lower()
        if not DEVICE_ID.fullmatch(device_id):
            raise RelayError(400, "invalid_device_id", "invalid device ID")
        if not re.fullmatch(r"[1-9][0-9]{0,9}", recording_text):
            raise RelayError(400, "invalid_recording_id", "invalid recording ID")
        if int(recording_text) > 0xFFFFFFFF:
            raise RelayError(400, "invalid_recording_id", "recording ID must be uint32")
        if not AUDIO_HASH.fullmatch(claimed_hash):
            raise RelayError(400, "invalid_audio_hash", "invalid audio SHA-256")
        self.authorize(headers.get("Authorization"), device_id)
        return device_id, int(recording_text), claimed_hash

    def _upload(
        self,
        headers: Mapping[str, str],
        source: bytes | pathlib.Path,
        actual_hash: str,
        now: int | None = None,
    ) -> tuple[dict, int]:
        device_id, recording_id, claimed_hash = self.preflight_upload(headers)
        wav = validate_wav(source)
        if actual_hash != claimed_hash:
            raise RelayError(400, "audio_hash_mismatch", "audio SHA-256 does not match body")
        now = now if now is not None else int(time.time() * 1000)
        created, row = self.store.create(device_id, recording_id, actual_hash, wav["duration_ms"], now)
        if row["audio_sha256"] != actual_hash or row["duration_ms"] != wav["duration_ms"]:
            raise RelayError(409, "recording_conflict", "recording ID already belongs to different audio")
        if not row.get("audio_bytes"):
            row = self.store.update(device_id, recording_id, audio_bytes=source_size(source))
        if not created:
            if row["status"] == "sent":
                return self.public_memo(row, True), 200
            if row["status"] == "transcription_rejected":
                raise RelayError(422, "transcript_rejected", "recording previously failed the quality gate")
            if row["status"] in {"uncertain", "sending"}:
                raise RelayError(409, "delivery_uncertain", "reconcile this recording before retrying")
            if row["status"] in {"received", "transcribing"} and now - row["updated_at"] < STALE_PROCESSING_MS:
                return self.public_memo(row, True), 202
            if row["status"] in {"transcribed", "delivery_failed"} and row["transcript"] and row["cloud_content"]:
                return self._deliver(row, now)
        self.store.update(device_id, recording_id, status="transcribing", error=None, updated_at=now)
        try:
            transcript, request_id = self._transcribe_source(source, wav["duration_ms"])
        except RelayError as error:
            self.store.update(
                device_id, recording_id,
                status="transcription_rejected" if error.code == "transcript_rejected" else "transcription_failed",
                error=f"{error.code}: {error}", updated_at=int(time.time() * 1000),
            )
            raise
        imported_at = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(now / 1000))
        content = f"**喵伴 #{recording_id}** · 云端导入时间 {imported_at}\n\n{transcript}"
        row = self.store.update(
            device_id, recording_id, status="transcribed", transcript=transcript, cloud_content=content,
            transcript_sha256=hashlib.sha256(transcript.encode()).hexdigest(), qwen_request_id=request_id,
            error=None, updated_at=int(time.time() * 1000),
        )
        return self._deliver(row, int(time.time() * 1000))

    def upload(self, headers: Mapping[str, str], data: bytes, now: int | None = None) -> tuple[dict, int]:
        return self._upload(headers, data, sha256_bytes(data), now)

    def upload_path(
        self,
        headers: Mapping[str, str],
        path: pathlib.Path,
        actual_hash: str | None = None,
        now: int | None = None,
    ) -> tuple[dict, int]:
        return self._upload(headers, path, actual_hash or sha256_source(path), now)

    def status(self, authorization: str | None, device_id: str, recording_id: int) -> tuple[dict, int]:
        self.authorize(authorization, device_id)
        row = self.store.get(device_id, recording_id)
        if not row:
            raise RelayError(404, "not_found", "recording not found")
        return self.public_memo(row), 200



if __name__ == "__main__":
    raise SystemExit("Use python -m relay. The old standalone server entry is retired.")
