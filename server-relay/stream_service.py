"""Durable, private Opus upload sessions for cardless devices.

There is deliberately no audio download API. Acknowledged chunks survive a
process restart until processing completes or their bounded retry window ends.
Only one service process / worker may process this database at a time.
"""
from __future__ import annotations

import array
import ctypes
import ctypes.util
import errno
import fcntl
import hashlib
import os
import pathlib
import queue
import re
import shutil
import struct
import sys
import threading
import time
import wave

STREAM_PATH = re.compile(r"^/v1/streams/([A-Za-z0-9._-]{1,64})/([1-9][0-9]{0,9})(?:/(finish|retry)|/chunks/([0-9]{1,10}))?$")
FORMAT = {"format": "opus", "sample_rate": 16000, "channels": 1, "frame_ms": 20, "bitrate": 24000}
MAX_CHUNK = 65536
FRAME_SAMPLES = 320
RETENTION_MS = 24 * 60 * 60 * 1000
TERMINAL = {"sent", "transcription_rejected", "audio_expired"}


class StreamError(Exception):
    def __init__(self, status: int, code: str, message: str):
        super().__init__(message)
        self.status, self.code = status, code


def _integer(value, low=0, high=0xFFFFFFFF):
    return isinstance(value, int) and not isinstance(value, bool) and low <= value <= high


def _identity(device, recording):
    if not isinstance(device, str) or not re.fullmatch(r"[A-Za-z0-9._-]{1,64}", device):
        raise StreamError(400, "invalid_device_id", "invalid device ID")
    if not _integer(recording, 1):
        raise StreamError(400, "invalid_recording_id", "recording ID must be uint32 and nonzero")


def packets(body: bytes):
    """Validate the length-prefixed wire envelope, including a truncated tail."""
    if not 0 < len(body) <= MAX_CHUNK:
        raise StreamError(400, "invalid_chunk", "chunk must contain 1..65536 bytes")
    offset = 0
    while offset < len(body):
        if offset + 2 > len(body):
            raise StreamError(400, "invalid_chunk", "truncated packet length")
        length = struct.unpack_from("<H", body, offset)[0]
        offset += 2
        if not 1 <= length <= 1275 or offset + length > len(body):
            raise StreamError(400, "invalid_chunk", "invalid or truncated Opus packet")
        yield body[offset:offset + length]
        offset += length


class OpusLibrary:
    """Small binding to the maintained system libopus; no pip audio dependency."""
    def __init__(self):
        name = ctypes.util.find_library("opus")
        candidates = [name] if name else []
        if sys.platform == "darwin":
            # The project Python runtime may not inherit Homebrew's loader search path.
            candidates += ["/opt/homebrew/lib/libopus.dylib", "/usr/local/lib/libopus.dylib"]
        elif sys.platform.startswith("linux"):
            candidates += ["libopus.so.0"]
        for candidate in candidates:
            try:
                self.lib = ctypes.CDLL(candidate)
                break
            except OSError:
                pass
        else:
            raise StreamError(503, "opus_unavailable", "system libopus is required and must be loadable")
        self.lib.opus_decoder_create.argtypes = [ctypes.c_int32, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
        self.lib.opus_decoder_create.restype = ctypes.c_void_p
        self.lib.opus_decoder_destroy.argtypes = [ctypes.c_void_p]
        self.lib.opus_decode.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int32,
                                        ctypes.POINTER(ctypes.c_int16), ctypes.c_int, ctypes.c_int]
        self.lib.opus_decode.restype = ctypes.c_int
        self.lib.opus_packet_get_nb_samples.argtypes = [ctypes.c_void_p, ctypes.c_int32, ctypes.c_int32]
        self.lib.opus_packet_get_nb_samples.restype = ctypes.c_int
        self.lib.opus_packet_get_nb_channels.argtypes = [ctypes.c_void_p]
        self.lib.opus_packet_get_nb_channels.restype = ctypes.c_int
        self.lib.opus_get_version_string.restype = ctypes.c_char_p
        self.version = self.lib.opus_get_version_string().decode("ascii", errors="replace")

    def validate(self, body):
        count = 0
        for packet in packets(body):
            if (self.lib.opus_packet_get_nb_samples(packet, len(packet), 16000) != FRAME_SAMPLES or
                    self.lib.opus_packet_get_nb_channels(packet) != 1):
                raise StreamError(400, "invalid_chunk", "each Opus packet must be mono and cover 20 ms at 16 kHz")
            count += 1
        return count

    def decode(self, sources, target: pathlib.Path, total_samples: int):
        error = ctypes.c_int()
        decoder = self.lib.opus_decoder_create(16000, 1, ctypes.byref(error))
        if not decoder or error.value:
            raise StreamError(503, "opus_unavailable", "Opus decoder allocation failed")
        buffer = (ctypes.c_int16 * FRAME_SAMPLES)()
        written = 0
        try:
            with target.open("wb") as output:
                with wave.open(output, "wb") as wav:
                    wav.setnchannels(1)
                    wav.setsampwidth(2)
                    wav.setframerate(16000)
                    for source, digest in sources:
                        body = source.read_bytes()
                        if hashlib.sha256(body).hexdigest() != digest:
                            raise StreamError(500, "stream_corrupt", "stored chunk checksum mismatch")
                        for packet in packets(body):
                            count = self.lib.opus_decode(decoder, packet, len(packet), buffer, FRAME_SAMPLES, 0)
                            if count != FRAME_SAMPLES:
                                raise StreamError(400, "invalid_chunk", "Opus frame could not be decoded")
                            count = min(count, total_samples - written)
                            if count < 0:
                                raise StreamError(400, "invalid_finish", "unexpected extra audio frames")
                            pcm = array.array("h", buffer[:count])
                            if sys.byteorder != "little":
                                pcm.byteswap()
                            wav.writeframesraw(pcm.tobytes())
                            written += count
                    if written != total_samples:
                        raise StreamError(400, "invalid_finish", "audio does not cover final sample count")
                output.flush()
                os.fsync(output.fileno())
        finally:
            self.lib.opus_decoder_destroy(decoder)


def _fsync_dir(path):
    descriptor = os.open(path, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


class StreamService:
    def __init__(self, relay, *, directory=None, clock=None, start_worker=True,
                 service_factory=None, schedule_callback=None, completion_callback=None,
                 schedule_guard=None, mutex=None):
        self.relay = relay
        self.store = relay.store
        self.directory = pathlib.Path(directory or self.store.path.parent / "streams")
        self.directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        self._now = clock or (lambda: int(time.time() * 1000))
        # Runtime composition shares one reentrant state lock. A schedule
        # callback must never acquire an oppositely ordered second state lock.
        self._mutex = mutex if mutex is not None else threading.RLock()
        self._queue = queue.Queue()
        self._scheduled = set()
        self._active = None
        self._stop = threading.Event()
        self._thread = None
        self._cleanup_thread = None
        self._service_factory = service_factory
        self._schedule_callback = schedule_callback
        self._completion_callback = completion_callback
        self._schedule_guard = schedule_guard
        self.cleanup_ok = True
        self.cleanup_error = None
        self._orphan_cleanup_error = None
        # Tie exclusivity to the database, even when an operator changes the
        # audio directory. Never steal a live worker merely because it is slow.
        self._lock_file = self.store.path.with_suffix(".streams.lock").open("a+b")
        try:
            fcntl.flock(self._lock_file, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            self._lock_file.close()
            raise RuntimeError("another stream worker owns this database")
        try:
            self.opus = OpusLibrary()
        except StreamError:
            self.opus = None  # Existing WAV upload remains available.
        with self.store.connect() as db:
            db.executescript("""
                CREATE TABLE IF NOT EXISTS streams (
                    device_id TEXT NOT NULL, recording_id INTEGER NOT NULL,
                    phase TEXT NOT NULL DEFAULT 'receiving',
                    next_seq INTEGER NOT NULL DEFAULT 0,
                    received_samples INTEGER NOT NULL DEFAULT 0,
                    final_samples INTEGER, final_chunks INTEGER,
                    interrupted INTEGER NOT NULL DEFAULT 0, reason TEXT NOT NULL DEFAULT '',
                    error TEXT, created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL,
                    expires_at INTEGER NOT NULL,
                    PRIMARY KEY(device_id, recording_id)
                );
                CREATE INDEX IF NOT EXISTS streams_expiry_idx ON streams(expires_at);
                CREATE INDEX IF NOT EXISTS streams_phase_idx ON streams(phase);
                CREATE TABLE IF NOT EXISTS stream_chunks (
                    device_id TEXT NOT NULL, recording_id INTEGER NOT NULL, seq INTEGER NOT NULL,
                    sha256 TEXT NOT NULL, samples INTEGER NOT NULL,
                    PRIMARY KEY(device_id, recording_id, seq)
                );
            """)
        if start_worker:
            self.recover()
            self.start()

    def start(self):
        if self._thread is None:
            self._thread = threading.Thread(target=self._worker, name="cardless-stream-worker", daemon=True)
            self._thread.start()
        self.start_cleanup()

    def start_cleanup(self):
        if self._cleanup_thread is None:
            self._cleanup_thread = threading.Thread(target=self._cleanup_worker, name="cardless-cleanup", daemon=True)
            self._cleanup_thread.start()

    def close(self, timeout=10):
        self._stop.set()
        if self._thread:
            self._thread.join(timeout)
            if self._thread.is_alive():
                raise RuntimeError("stream worker is still processing; database lock retained")
        if self._cleanup_thread:
            self._cleanup_thread.join(timeout)
            if self._cleanup_thread.is_alive():
                raise RuntimeError("stream cleanup is still processing; database lock retained")
        if not self._lock_file.closed:
            fcntl.flock(self._lock_file, fcntl.LOCK_UN)
            self._lock_file.close()

    def _row(self, device, recording):
        with self.store.connect() as db:
            row = db.execute("SELECT * FROM streams WHERE device_id=? AND recording_id=?",
                             (device, recording)).fetchone()
        if not row:
            raise StreamError(404, "not_found", "stream not found")
        return dict(row)

    def _update(self, device, recording, **fields):
        allowed = {"phase", "next_seq", "received_samples", "final_samples", "final_chunks",
                   "interrupted", "reason", "error", "updated_at", "expires_at"}
        if not fields or not set(fields) <= allowed:
            raise ValueError("invalid stream update")
        with self.store.connect() as db:
            db.execute("UPDATE streams SET " + ",".join(f"{name}=?" for name in fields) +
                       " WHERE device_id=? AND recording_id=?", (*fields.values(), device, recording))

    def _path(self, device, recording):
        # IDs never become path components, so even '.' / '..' device IDs are safe.
        identity = hashlib.sha256(f"{device}/{recording}".encode()).hexdigest()
        return self.directory / identity

    def _public(self, row):
        memo = self.store.get(row["device_id"], row["recording_id"])
        state = "receiving" if row["phase"] == "receiving" else "received"
        error = row["error"]
        if memo:
            state, error = memo["status"], memo["error"]
        if row["phase"] == "queued" and state not in TERMINAL | {"uncertain", "sending"}:
            state, error = "received", None
        if row["phase"] == "audio_expired" and state not in {"sent", "transcription_rejected", "uncertain", "sending", "transcribed", "delivery_failed"}:
            state, error = "audio_expired", row["error"] or "audio_expired"
        # A finished transcript can be re-delivered after audio expiration.
        retry_text = bool(memo and memo["status"] in {"transcribed", "delivery_failed"} and memo["cloud_content"])
        permanent_audio_error = any(marker in (error or "") for marker in
                                    ("stream_corrupt", "invalid_chunk", "FileNotFoundError"))
        retry_audio = state == "transcription_failed" and row["phase"] != "audio_expired" and not permanent_audio_error
        busy = row["phase"] in {"queued", "working"}
        samples = row["final_samples"] if row["final_samples"] is not None else row["received_samples"]
        return {"device_id": row["device_id"], "recording_id": row["recording_id"],
                "status": state, "next_seq": row["next_seq"], "received_samples": samples,
                "duration_ms": round(samples * 1000 / 16000), "error": error,
                "can_retry": bool((retry_text or retry_audio) and not busy),
                "interrupted": bool(row["interrupted"])}

    def authorize(self, authorization, device, recording):
        _identity(device, recording)
        self.relay.authorize(authorization, device)

    def create(self, headers, body):
        device, text = headers.get("X-Device-ID", ""), headers.get("X-Recording-ID", "")
        if not isinstance(text, str) or not re.fullmatch(r"[1-9][0-9]{0,9}", text):
            raise StreamError(400, "invalid_recording_id", "invalid recording ID")
        recording = int(text)
        self.authorize(headers.get("Authorization"), device, recording)
        if (not isinstance(body, dict) or body != FORMAT or
                any(type(body.get(name)) is not int for name in ("sample_rate", "channels", "frame_ms", "bitrate"))):
            raise StreamError(400, "unsupported_format", "expected Opus mono 16 kHz, 20 ms, 24000 bps")
        if self.opus is None:
            raise StreamError(503, "opus_unavailable", "system libopus is required")
        with self._mutex:
            self._expire_locked()
            now = self._now()
            with self.store.connect() as db:
                db.execute("BEGIN IMMEDIATE")
                old = db.execute("SELECT * FROM streams WHERE device_id=? AND recording_id=?",
                                 (device, recording)).fetchone()
                if old:
                    return self._public(dict(old)), 200
                if db.execute("SELECT 1 FROM memos WHERE device_id=? AND recording_id=?",
                              (device, recording)).fetchone():
                    raise StreamError(409, "recording_conflict", "ID belongs to a WAV recording")
                db.execute("INSERT INTO streams (device_id,recording_id,created_at,updated_at,expires_at) "
                           "VALUES(?,?,?,?,?)", (device, recording, now, now, now + RETENTION_MS))
            return self._public(self._row(device, recording)), 201

    def put_chunk(self, authorization, device, recording, seq, headers, body):
        self.authorize(authorization, device, recording)
        if not _integer(seq):
            raise StreamError(400, "invalid_chunk", "sequence must be uint32")
        if (headers.get("Content-Type") or "").split(";", 1)[0] != "application/octet-stream":
            raise StreamError(415, "unsupported_media_type", "expected application/octet-stream")
        claimed = headers.get("X-Chunk-SHA256", "")
        if not re.fullmatch(r"[a-fA-F0-9]{64}", claimed):
            raise StreamError(400, "invalid_chunk_hash", "SHA-256 header required")
        digest = hashlib.sha256(body).hexdigest()
        if claimed.lower() != digest:
            raise StreamError(400, "chunk_hash_mismatch", "chunk SHA-256 does not match body")
        if self.opus is None:
            raise StreamError(503, "opus_unavailable", "system libopus is required")
        samples = self.opus.validate(body) * FRAME_SAMPLES
        with self._mutex:
            self._expire_locked()
            row = self._row(device, recording)
            if row["phase"] == "audio_expired":
                raise StreamError(410, "audio_expired", "temporary audio has expired")
            with self.store.connect() as db:
                db.execute("BEGIN IMMEDIATE")
                old = db.execute("SELECT sha256 FROM stream_chunks WHERE device_id=? AND recording_id=? AND seq=?",
                                 (device, recording, seq)).fetchone()
                if old:
                    if old["sha256"] != digest:
                        raise StreamError(409, "chunk_conflict", "sequence already has different audio")
                    return self._public(row), 200
                if row["phase"] == "audio_expired":
                    raise StreamError(410, "audio_expired", "temporary audio has expired")
                if row["phase"] != "receiving":
                    raise StreamError(409, "stream_closed", "stream is already finished")
                if seq != row["next_seq"]:
                    raise StreamError(409, "chunk_out_of_order", "send next_seq before later chunks")
                directory = self._path(device, recording)
                directory.mkdir(mode=0o700, exist_ok=True)
                _fsync_dir(self.directory)
                temporary = directory / "chunk.pending"
                try:
                    with temporary.open("wb") as output:
                        os.chmod(temporary, 0o600)
                        output.write(body)
                        output.flush()
                        os.fsync(output.fileno())
                    os.replace(temporary, directory / f"{seq:010d}.opus")
                    _fsync_dir(directory)
                    now = self._now()
                    db.execute("INSERT INTO stream_chunks VALUES(?,?,?,?,?)", (device, recording, seq, digest, samples))
                    db.execute("UPDATE streams SET next_seq=next_seq+1,received_samples=received_samples+?,"
                               "updated_at=?,expires_at=? WHERE device_id=? AND recording_id=?",
                               (samples, now, now + RETENTION_MS, device, recording))
                except OSError as error:
                    temporary.unlink(missing_ok=True)
                    if error.errno in {errno.ENOSPC, errno.EDQUOT}:
                        raise StreamError(507, "storage_full", "server temporary storage is full") from error
                    raise
            return self._public(self._row(device, recording)), 201

    def finish(self, authorization, device, recording, body):
        self.authorize(authorization, device, recording)
        if not isinstance(body, dict):
            raise StreamError(400, "invalid_finish", "JSON object required")
        chunks, samples = body.get("chunks"), body.get("samples")
        interrupted, reason = body.get("interrupted"), body.get("reason")
        if (not _integer(chunks) or not _integer(samples, 0, 0x7FFFFFFFFFFFFFFF) or
                not isinstance(interrupted, bool) or not isinstance(reason, str) or len(reason) > 160 or
                any(ord(c) < 32 for c in reason)):
            raise StreamError(400, "invalid_finish", "chunks, samples, interrupted and bounded reason required")
        with self._mutex:
            self._expire_locked()
            row = self._row(device, recording)
            if row["phase"] == "audio_expired":
                raise StreamError(410, "audio_expired", "temporary audio has expired")
            if row["final_samples"] is not None:
                if (samples, chunks, interrupted, reason) != (row["final_samples"], row["final_chunks"],
                                                             bool(row["interrupted"]), row["reason"]):
                    raise StreamError(409, "finish_conflict", "finish parameters differ from accepted finish")
                result = self._public(row)
                return result, 200 if result["status"] in TERMINAL else 202
            if chunks != row["next_seq"]:
                raise StreamError(409, "missing_chunks", "chunks must equal acknowledged next_seq")
            if not (row["received_samples"] - FRAME_SAMPLES < samples <= row["received_samples"]):
                raise StreamError(400, "invalid_finish", "samples may trim padding only in the final 20 ms frame")
            if samples == 0 and not (chunks == 0 and interrupted):
                raise StreamError(400, "invalid_finish", "empty recording requires interrupted=true")
            # Persist the complete intent before enqueuing. A crash between these
            # operations is repaired on startup without needing the device.
            self._update(device, recording, final_chunks=chunks, final_samples=samples,
                         interrupted=int(interrupted), reason=reason, phase="queued",
                         updated_at=self._now(), expires_at=self._now() + RETENTION_MS)
            self._schedule(device, recording)
            return self._public(self._row(device, recording)), 202

    def status(self, authorization, device, recording):
        self.authorize(authorization, device, recording)
        with self._mutex:
            self._expire_locked()
            return self._public(self._row(device, recording)), 200

    def retry(self, authorization, device, recording):
        self.authorize(authorization, device, recording)
        return self.retry_authorized(device, recording)

    def retry_authorized(self, device, recording):
        """Local administrator adapter calls only after its own authorization."""
        _identity(device, recording)
        with self._mutex:
            self._expire_locked()
            row = self._row(device, recording)
            result = self._public(row)
            if self._active == (device, recording):
                return result, 202
            if row["phase"] in {"queued", "working"}:
                return result, 202
            if result["status"] == "sent":
                return result, 200
            if result["status"] in {"sending", "uncertain"}:
                raise StreamError(409, "delivery_uncertain", "reconcile delivery before retrying")
            if not result["can_retry"]:
                if result["status"] == "audio_expired":
                    raise StreamError(410, "audio_expired", "audio no longer available for transcription")
                raise StreamError(409, "cannot_retry", "stream has no retryable operation")
            self._update(device, recording, phase="queued", error=None, updated_at=self._now(),
                         expires_at=self._now() + RETENTION_MS)
            self._schedule(device, recording)
            return self._public(self._row(device, recording)), 202

    def reconciliation_guard(self, device, recording):
        with self._mutex:
            if self._active == (device, recording):
                raise StreamError(409, "processing_active", "wait for the active stream worker before reconciliation")

    def reconciled(self, device, recording, present):
        with self._mutex:
            try:
                row = self._row(device, recording)
            except StreamError as error:
                if error.code == "not_found":
                    return
                raise
            if present:
                self._complete(row)

    def _schedule(self, device, recording):
        key = (device, recording)
        if self._schedule_guard and not self._schedule_guard(key):
            return
        if key not in self._scheduled:
            self._scheduled.add(key)
            self._queue.put(key)
            if self._schedule_callback:
                self._schedule_callback(key)

    def recover(self):
        """Explicit recovery; start_worker=False construction never mutates jobs."""
        with self._mutex:
            self._expire_locked()
            with self.store.connect() as db:
                rows = [dict(row) for row in db.execute("SELECT * FROM streams WHERE phase!='done'")]
            for row in rows:
                device, recording = row["device_id"], row["recording_id"]
                memo = self.store.get(device, recording)
                if memo and memo["status"] == "sending":
                    # A process can die after the remote accepted delivery and
                    # before we committed 'sent'. Never automatically re-send.
                    self.store.update(device, recording, status="uncertain", error="restart_during_delivery",
                                      updated_at=self._now())
                    self._update(device, recording, phase="failed")
                elif memo and memo["status"] in {"sent", "transcription_rejected"}:
                    self._complete(row)
                elif row["phase"] == "working" and memo and memo["status"] in {"uncertain", "delivery_failed", "transcription_failed"}:
                    self._update(device, recording, phase="failed")
                elif row["phase"] in {"queued", "working"}:
                    self._update(device, recording, phase="queued")
                    self._schedule(device, recording)
            self._expire_locked()

    _recover = recover  # Existing callers may still request explicit recovery.

    def cleanup_orphans(self):
        """Startup-only file reconciliation before any decoder/worker can run."""
        with self._mutex:
            self._orphan_cleanup_error = None
            if self._active is not None:
                raise RuntimeError("cannot reconcile files while a stream is active")
            with self.store.connect() as db:
                rows = [dict(r) for r in db.execute("SELECT * FROM streams")]
                chunks = [dict(r) for r in db.execute("SELECT * FROM stream_chunks")]
            records = {self._path(r["device_id"], r["recording_id"]).name: r for r in rows}
            expected = {}
            for chunk in chunks:
                key = chunk["device_id"], chunk["recording_id"]
                expected.setdefault(key, {})[f"{chunk['seq']:010d}.opus"] = chunk["sha256"]
            for directory in self.directory.iterdir():
                if not re.fullmatch(r"[a-f0-9]{64}", directory.name):
                    self._orphan_cleanup_error = "unexpected_audio_file"
                    self.cleanup_ok, self.cleanup_error = False, "unexpected_audio_file"
                    continue
                row = records.get(directory.name)
                if row is None:
                    if directory.is_symlink() or not directory.is_dir():
                        directory.unlink()
                    else:
                        shutil.rmtree(directory)
                    continue
                key = row["device_id"], row["recording_id"]
                if directory.is_symlink() or not directory.is_dir():
                    raise RuntimeError("invalid private stream directory")
                for path in directory.iterdir():
                    if path.name in {"chunk.pending", "assembled.wav"} or (re.fullmatch(r"[0-9]{10}\.opus", path.name) and path.name not in expected.get(key, {})):
                        path.unlink()
                    elif path.name not in expected.get(key, {}):
                        self._orphan_cleanup_error = "unexpected_audio_file"
                        self.cleanup_ok, self.cleanup_error = False, "unexpected_audio_file"
            for row in rows:
                key = row["device_id"], row["recording_id"]
                memo = self.store.get(*key)
                if row["phase"] in {"done", "audio_expired"} or (memo and memo["status"] in TERMINAL):
                    continue
                corrupt = False
                for name, digest in expected.get(key, {}).items():
                    path = self._path(*key) / name
                    if path.is_symlink() or not path.is_file() or path.stat().st_size > MAX_CHUNK or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
                        corrupt = True
                        break
                if corrupt:
                    self._update(*key, phase="failed", error="stream_corrupt")
                    if memo is None:
                        with self.store.connect() as db:
                            db.execute("INSERT INTO memos(device_id,recording_id,audio_sha256,duration_ms,status,created_at,updated_at,error) "
                                       "VALUES(?,?,?,?,'transcription_failed',?,?,'stream_corrupt')",
                                       (*key, "", round(row["received_samples"] * 1000 / 16000), row["created_at"], self._now()))
                    elif memo["status"] in {"received", "transcribing", "transcription_failed"}:
                        self.store.update(*key, status="transcription_failed", error="stream_corrupt", updated_at=self._now())
            _fsync_dir(self.directory)

    def _remove_audio(self, row):
        directory = self._path(row["device_id"], row["recording_id"])
        if directory.exists():
            shutil.rmtree(directory)
            _fsync_dir(self.directory)

    def _complete(self, row):
        key = (row["device_id"], row["recording_id"])
        self._update(*key, phase="cleanup", error=None, updated_at=self._now())
        try:
            self._remove_audio(row)
        except OSError:
            self.cleanup_ok, self.cleanup_error = False, "audio_cleanup_failed"
            self._update(*key, error="audio_cleanup_failed")
            raise
        self._update(*key, phase="done")

    def _expire_locked(self):
        failures = False
        with self.store.connect() as db:
            rows = [dict(row) for row in db.execute(
                "SELECT * FROM streams WHERE (expires_at<=? AND phase NOT IN ('done','audio_expired')) OR phase='cleanup'", (self._now(),))]
        for row in rows:
            key = (row["device_id"], row["recording_id"])
            if key == self._active:
                continue
            memo = self.store.get(*key)
            try:
                if memo and memo["status"] in {"sent", "transcription_rejected"}:
                    self._complete(row)
                    continue
            # Finished plaintext may still be used to retry delivery; audio is
            # never kept merely because delivery requires human reconciliation.
                self._remove_audio(row)
                self._update(*key, phase="audio_expired", error="audio_expired", updated_at=self._now())
                if memo and memo["status"] in {"received", "transcribing", "transcription_failed"}:
                    self.store.update(*key, status="audio_expired", error="audio_expired", updated_at=self._now())
            except OSError:
                failures = True
                self._update(*key, error="audio_cleanup_failed")
        self.cleanup_ok = not (failures or self._orphan_cleanup_error)
        self.cleanup_error = "audio_cleanup_failed" if failures else self._orphan_cleanup_error

    def expire(self):
        with self._mutex:
            self._expire_locked()

    def _worker(self):
        while not self._stop.is_set():
            try:
                self.run_pending_once(timeout=0.5)
            except Exception as error:
                # No content/credentials in diagnostics. Storage failures must
                # not silently kill the sole processing / expiry worker.
                print("stream_worker_error=" + type(error).__name__, file=sys.stderr)
                self._stop.wait(0.5)

    def _cleanup_worker(self):
        while not self._stop.is_set():
            try:
                self.expire()
                with self.store.connect() as db:
                    deadline = db.execute("SELECT MIN(expires_at) FROM streams WHERE phase NOT IN ('done','audio_expired')").fetchone()[0]
                delay = min(1.0, max(0.01, (deadline - self._now()) / 1000)) if deadline else 1.0
            except Exception:
                self.cleanup_ok, self.cleanup_error = False, "audio_cleanup_failed"
                delay = 0.25
            self._stop.wait(delay)

    def run_pending_once(self, timeout=0):
        """One worker iteration; exposed for deterministic restart/fault tests."""
        try:
            key = self._queue.get(timeout=timeout)
        except queue.Empty:
            return False
        try:
            with self._mutex:
                row = self._row(*key)
                if row["phase"] != "queued":
                    return True
                self._active = key
                self._update(*key, phase="working")
            self._process(key)
        finally:
            with self._mutex:
                self._active = None
                self._scheduled.discard(key)
            self._queue.task_done()
            if self._completion_callback:
                self._completion_callback(key)
        return True

    def _process(self, key):
        row = self._row(*key)
        now = self._now()
        wav_path = self._path(*key) / "assembled.wav"
        display_id = key[1] & 0x7FFFFFFF
        title = f"喵伴无卡语音笔记 #{display_id}"
        progressed = False
        try:
            relay = self._service_factory(key) if self._service_factory else self.relay
            memo = self.store.get(*key)
            if memo and memo["status"] in {"sent", "transcription_rejected"}:
                return
            if memo and memo["status"] in {"sending", "uncertain"}:
                self._update(*key, phase="failed", error="delivery_uncertain")
                return
            if memo and memo["status"] in {"transcribed", "delivery_failed"} and memo["cloud_content"]:
                progressed = True
                relay._deliver(memo, now, title=title)
                return
            if row["final_samples"] == 0:
                with self.store.connect() as db:
                    db.execute("INSERT OR IGNORE INTO memos(device_id,recording_id,audio_sha256,duration_ms,status,created_at,updated_at,error) "
                               "VALUES(?,?,?,0,'transcription_rejected',?,?,?)", (*key, hashlib.sha256(b'').hexdigest(), now, now, "empty"))
                return
            if self.opus is None:
                raise StreamError(503, "opus_unavailable", "system libopus is required")
            with self.store.connect() as db:
                chunks = [dict(item) for item in db.execute("SELECT * FROM stream_chunks WHERE device_id=? AND recording_id=? ORDER BY seq", key)]
            if len(chunks) != row["final_chunks"] or any(item["seq"] != index for index, item in enumerate(chunks)):
                raise StreamError(500, "stream_corrupt", "acknowledged chunk metadata is incomplete")
            sources = [(self._path(*key) / f"{item['seq']:010d}.opus", item["sha256"]) for item in chunks]
            self.opus.decode(sources, wav_path, row["final_samples"])
            progressed = True
            self._update(*key, updated_at=self._now(), expires_at=self._now() + RETENTION_MS)
            digest = hashlib.sha256()
            with wav_path.open("rb") as audio:
                for block in iter(lambda: audio.read(MAX_CHUNK), b""):
                    digest.update(block)
            duration = round(row["final_samples"] * 1000 / 16000)
            with self.store.connect() as db:
                db.execute("INSERT OR IGNORE INTO memos(device_id,recording_id,audio_sha256,duration_ms,status,created_at,updated_at) "
                           "VALUES(?,?,?,?,'received',?,?)", (*key, digest.hexdigest(), duration, now, now))
            memo = self.store.get(*key)
            if memo["audio_sha256"] == "" and memo["status"] == "transcription_failed":
                with self.store.connect() as db:
                    db.execute("UPDATE memos SET audio_sha256=? WHERE device_id=? AND recording_id=? AND audio_sha256=''",
                               (digest.hexdigest(), *key))
                memo = self.store.get(*key)
            if memo["audio_sha256"] != digest.hexdigest():
                raise StreamError(409, "recording_conflict", "ID already belongs to different audio")
            self.store.update(*key, status="transcribing", error=None, updated_at=now)
            transcript, request_id = relay._transcribe_source(wav_path, duration)
            imported_at = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(row["created_at"] / 1000))
            note = " · 中断录音（仅已接收部分）" if row["interrupted"] else ""
            content = f"**喵伴无卡 #{display_id}** · 云端导入时间 {imported_at}{note}\n\n{transcript}"
            memo = self.store.update(*key, status="transcribed", transcript=transcript, cloud_content=content,
                                     transcript_sha256=hashlib.sha256(transcript.encode()).hexdigest(),
                                     qwen_request_id=request_id, error=None, updated_at=self._now())
            relay._deliver(memo, self._now(), title=title)
        except Exception as error:
            memo = self.store.get(*key)
            code = getattr(error, "code", "processing_failed")
            detail = str(error)[:240] if hasattr(error, "code") else type(error).__name__
            failure = f"{code}: {detail}"
            if memo is None:
                with self.store.connect() as db:
                    db.execute("INSERT OR IGNORE INTO memos(device_id,recording_id,audio_sha256,duration_ms,status,created_at,updated_at,error) "
                               "VALUES(?,?,?,?,'transcription_failed',?,?,?)",
                               (*key, "", round(row["final_samples"] * 1000 / 16000), now, now, failure))
            elif memo["status"] in {"received", "transcribing", "transcription_failed"}:
                self.store.update(*key, status="transcription_rejected" if code == "transcript_rejected" else "transcription_failed",
                                  error=failure, updated_at=self._now())
            elif memo["status"] == "transcribed":
                self.store.update(*key, status="delivery_failed", error=failure, updated_at=self._now())
            elif memo["status"] == "sending":
                # An unexpected exception may also happen after remote acceptance.
                self.store.update(*key, status="uncertain", error=failure, updated_at=self._now())
            # Only work that actually decoded audio or began durable-text
            # delivery can end a processing interval. Merely failing to obtain
            # configuration must not slide the audio's previous deadline.
            fields = {"phase": "failed", "error": failure, "updated_at": self._now()}
            if progressed:
                fields["expires_at"] = self._now() + RETENTION_MS
            self._update(*key, **fields)
        finally:
            memo = self.store.get(*key)
            if memo and memo["status"] in {"sent", "transcription_rejected"}:
                self._complete(row)
            else:
                wav_path.unlink(missing_ok=True)
