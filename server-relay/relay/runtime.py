"""Single-instance task runtime; durable configuration pins precede all ACKs.

WAV reception and the one ASR worker are bounded independently. Control and
cleanup do not run on that worker. The immutable task ledger is the authority
for configuration recovery. In-flight attempts keep their original snapshot;
failed attempts release it, and deliberate retries use the current configuration.
"""
from __future__ import annotations

import fcntl
import hashlib
import os
import pathlib
import queue
import re
import shutil
import threading
import time
import uuid
from dataclasses import dataclass

from app import MAX_WAV_BYTES, MemoStore, RelayError, RelayService, sha256_source, validate_wav
from config_broker.core import BrokerError
from stream_service import FORMAT, RETENTION_MS, StreamError, StreamService
from .jobs import RUNTIME_SCHEMA_VERSION, initialize_schema
from .providers import ConfiguredService, probe_config, target_id
from .auth import UploadAuthorization

TERMINAL = {"sent", "transcription_rejected", "audio_expired"}
STOPPED = TERMINAL | {"uncertain", "transcription_failed", "delivery_failed", "awaiting_audio"}
MAX_RECEIVERS = 2
MIN_FREE_BYTES = 64 * 1024 * 1024


@dataclass
class Admission:
    path: pathlib.Path
    device_id: str
    recording_id: int
    task_id: str
    length: int
    transferred: bool = False
    released: bool = False


class Runtime:
    def __init__(self, storepath, broker, workdir, streamdir, maintenance=False, *,
                 provider_factory=None, clock=None, start_workers=True,
                 min_free_bytes=MIN_FREE_BYTES):
        self.path = pathlib.Path(storepath)
        self.broker = broker
        self.workdir, self.streamdir = pathlib.Path(workdir), pathlib.Path(streamdir)
        self._now = clock or (lambda: int(time.time() * 1000))
        self._provider_factory = provider_factory or ConfiguredService
        self._start_workers = start_workers
        self.min_free_bytes = min_free_bytes
        self.owner_generation = uuid.uuid4().hex
        self._mutex = threading.RLock()
        self._stop = threading.Event()
        self._queue = queue.Queue()
        self._scheduled = set()
        self._active = None
        self._admissions = {}
        self._probes = {}
        self._recovered = False
        self._worker_thread = self._cleanup_thread = None
        self._closed = False
        self._startup_error = None
        self._cleanup_error = None
        self._maintenance = bool(maintenance)
        self.draining = bool(maintenance)
        self.streams = None
        self._lock_file = None
        self.path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        self._lock_file = self.path.with_suffix(".runtime.lock").open("a+b")
        try:
            fcntl.flock(self._lock_file, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            self._lock_file.close()
            raise RuntimeError("another relay runtime owns this database") from None
        try:
            # Runtime already owns the exclusive lock. Its ordinary SQLite
            # startup must replay WAL; only the separate inspector is immutable.
            inspected = self._startup_schema()
            if not inspected["compatible"]:
                raise RuntimeError("unsupported or damaged storage schema")
            for path in (self.workdir, self.streamdir):
                if path.is_symlink():
                    raise RuntimeError("private data directory cannot be a symbolic link")
                path.mkdir(parents=True, exist_ok=True, mode=0o700)
            self.store = MemoStore(self.path)
            self.identity = self.store.identity
            self.instance_uuid = self.identity.server_instance_uuid
            initialize_schema(self.store, self.owner_generation)
            self.auth = UploadAuthorization(self.store)
            self.streams = StreamService(self.auth, directory=self.streamdir, clock=self._now,
                                        start_worker=False, service_factory=self._service_for,
                                        schedule_callback=self._stream_queued, completion_callback=self._settle,
                                        schedule_guard=self._may_process, mutex=self._mutex)
            self.auth.streams = self.streams
            try:
                self._remove_wav_orphans()
                self.streams.cleanup_orphans()
                self.streams.expire()  # Expired audio is removed before any task may resume.
            except OSError:
                self._cleanup_error = "audio_cleanup_failed"
            self._recover_owned_jobs()
            self._reconcile_pins()
            if not self.draining and not self._startup_error:
                self._recover_queues()
            self._start_threads()
        except Exception:
            if self.streams:
                self.streams.close()
            self._unlock()
            raise

    def _unlock(self):
        if self._lock_file and not self._lock_file.closed:
            fcntl.flock(self._lock_file, fcntl.LOCK_UN)
            self._lock_file.close()

    def _startup_schema(self):
        if not self.path.exists():
            return {"compatible": True}
        import sqlite3
        db = sqlite3.connect(self.path.resolve().as_uri() + "?mode=ro", uri=True)
        try:
            tables = {r[0] for r in db.execute("SELECT name FROM sqlite_master WHERE type='table'")}
            if "relay_runtime_meta" in tables:
                row = db.execute("SELECT schema_version FROM relay_runtime_meta WHERE singleton=1").fetchone()
                return {"compatible": bool(row and row[0] == RUNTIME_SCHEMA_VERSION)}
            # Empty tables made during first-owner initialization are allowed.
            # Audio/task rows without our ledger are not imported as new work.
            for table in ("memos", "streams"):
                if table in tables and db.execute("SELECT 1 FROM " + table + " LIMIT 1").fetchone():
                    return {"compatible": False}
            return {"compatible": True}
        finally:
            db.close()

    def _task_id(self, device, recording):
        value = f"{self.instance_uuid}/{device}/{recording}".encode()
        return "job:" + hashlib.sha256(value).hexdigest()

    def _job(self, device, recording):
        with self.store.connect() as db:
            row = db.execute("SELECT * FROM relay_jobs WHERE device_id=? AND recording_id=?", (device, recording)).fetchone()
        return dict(row) if row else None

    def _update_job(self, device, recording, **fields):
        allowed = {"revision", "pin_id", "target_id", "phase", "owner_generation", "updated_at", "claimed_hash", "audio_bytes", "wav_name"}
        if not fields or not set(fields) <= allowed:
            raise ValueError("invalid task update")
        with self.store.connect() as db:
            db.execute("UPDATE relay_jobs SET " + ",".join(name + "=?" for name in fields) + " WHERE device_id=? AND recording_id=?",
                       (*fields.values(), device, recording))

    def _remove_wav_orphans(self):
        # Every WAV original remains on SD. Never resume a transient receive or
        # ASR file after a process died; request a verified explicit re-upload.
        for path in self.workdir.iterdir():
            if re.fullmatch(r"wav-[a-f0-9]{32}\.wav", path.name):
                if path.is_dir() and not path.is_symlink():
                    raise RuntimeError("unexpected directory in private WAV work area")
                path.unlink()


    def _recover_owned_jobs(self):
        with self.store.connect() as db:
            jobs = [dict(r) for r in db.execute("SELECT * FROM relay_jobs")]
        for job in jobs:
            key = job["device_id"], job["recording_id"]
            if job["source"] == "stream" and self._discard_unstarted_stream(key):
                continue
            memo = self.store.get(*key)
            if memo and memo["status"] == "sending":
                self.store.update(*key, status="uncertain", error="restart_during_delivery", updated_at=self._now())
                self._update_job(*key, phase="uncertain", wav_name=None, owner_generation=None)
            elif job["source"] == "wav" and (not memo or memo["status"] in {"received", "transcribing"}):
                if memo:
                    self.store.update(*key, status="transcription_failed", error="wav_reupload_required", updated_at=self._now())
                self._update_job(*key, phase="awaiting_audio", wav_name=None, owner_generation=None)
            else:
                self._update_job(*key, owner_generation=None)
            self._settle(key)

    def _reconcile_pins(self):
        try:
            with self.store.connect() as db:
                refs = {r["pin_id"]: r["revision"] for r in db.execute(
                    "SELECT pin_id,revision,phase FROM relay_jobs WHERE revision IS NOT NULL")
                        if r["phase"] not in STOPPED}
            refs.update({task: revision for task, revision in self._probes.items() if revision})
            generation = self.broker.begin_maintenance()["generation"]
            self.broker.reconcile(generation, refs)
            for task, revision in refs.items():
                self.broker.read_snapshot(revision, task)
            self.broker.end_maintenance(generation)
            self._startup_error = None
        except (BrokerError, OSError):
            self._startup_error = "configuration_recovery_required"

    def _may_process(self, key):
        job = self._job(*key)
        return bool(job and job["revision"] and job["phase"] not in STOPPED and not self._maintenance and not self._startup_error)

    def _recover_queues(self):
        if self._recovered:
            return
        self.streams.recover()
        with self.store.connect() as db:
            rows = [dict(r) for r in db.execute("SELECT * FROM relay_jobs WHERE source='wav' AND revision IS NOT NULL")]
        for job in rows:
            key = job["device_id"], job["recording_id"]
            memo = self.store.get(*key)
            if job["phase"] not in STOPPED and memo and memo["status"] == "transcribed" and memo["cloud_content"]:
                self._enqueue("wav", key)
        self._recovered = True

    def _start_threads(self):
        if not self._start_workers:
            return
        if self._worker_thread is None and not (self._maintenance or self._startup_error):
            self._worker_thread = threading.Thread(target=self._worker, name="relay-task-worker", daemon=True)
            self._worker_thread.start()
        if self._cleanup_thread is None:
            self._cleanup_thread = threading.Thread(target=self._cleanup_worker, name="relay-cleanup", daemon=True)
            self._cleanup_thread.start()

    def readiness(self):
        reason = self._startup_error
        configured = False
        try:
            view = self.broker.view()
            if view.get("maintenance"):
                with self._mutex:
                    if not (self._active or self._admissions or self._probes or self._closed):
                        self._reconcile_pins()
                        view = self.broker.view()
                        if not self._startup_error:
                            self._recover_queues()
                            self._start_threads()
                    if view.get("maintenance"):
                        self._startup_error = "configuration_recovery_required"
            config = view.get("active_config") or {}
            configured = bool(view.get("active_revision") and config.get("qwen_api_key_configured") and config.get("siyuan_token_configured"))
            reason = self._startup_error
        except (BrokerError, OSError):
            reason = reason or "broker_unavailable"
        cleanup_ok = bool(self.streams.cleanup_ok and not self._cleanup_error)
        worker_ok = not self._start_workers or bool(self._worker_thread and self._worker_thread.is_alive())
        if self._closed:
            reason = "closed"
        elif self.draining:
            reason = reason or "maintenance"
        elif not cleanup_ok:
            reason = reason or "audio_cleanup_failed"
        elif not configured:
            reason = reason or "not_configured"
        elif self.streams.opus is None:
            reason = reason or "opus_unavailable"
        elif not worker_ok:
            reason = reason or "worker_unavailable"
        return {"ready": reason is None, "reason": reason, "configured": configured,
                "cleanup_ok": cleanup_ok, "opus_available": self.streams.opus is not None,
                "worker_available": not self._closed and worker_ok,
                "maintenance": self.draining, "instance_uuid": self.instance_uuid,
                "schema_version": RUNTIME_SCHEMA_VERSION}

    def _require_new(self):
        ready = self.readiness()
        if not ready["ready"]:
            raise RelayError(503, ready["reason"] or "unavailable", "relay is not accepting new recordings")

    def diagnostics(self):
        return {**self.readiness(), **self.drain_status(),
                "worker_alive": bool(self._worker_thread and self._worker_thread.is_alive()),
                "free_work_bytes": shutil.disk_usage(self.workdir).free}



    def _pin_job(self, device, recording, source, claimed_hash=None, audio_bytes=None):
        existing = self._job(device, recording)
        if existing:
            if source == "wav" and existing["source"] == "wav" and existing["phase"] == "awaiting_audio" and not self.store.get(device, recording):
                # A rejected/incomplete body never established an audio identity.
                self._update_job(device, recording, claimed_hash=claimed_hash, audio_bytes=audio_bytes)
                existing = self._job(device, recording)
            if (existing["source"] != source or
                    (claimed_hash and existing["claimed_hash"] and existing["claimed_hash"] != claimed_hash) or
                    (audio_bytes and existing["audio_bytes"] and existing["audio_bytes"] != audio_bytes)):
                raise RelayError(409, "recording_conflict", "recording identity conflicts with existing task")
            if existing["pin_id"] and existing["phase"] in STOPPED:
                self._settle((device, recording))
                existing = self._job(device, recording)
                if existing["pin_id"]:
                    raise RelayError(503, "configuration_cleanup_pending", "previous attempt cleanup is pending")
            if existing["revision"]:
                self.broker.pin_existing(existing["pin_id"], existing["revision"])
                return existing
        return self._bind_active(device, recording, source, existing, claimed_hash, audio_bytes)

    def _bind_active(self, device, recording, source, existing=None, claimed_hash=None, audio_bytes=None, expected_revision=None):
        if existing and existing["pin_id"]:
            # Never overwrite the only durable reference to an unfinished release.
            self._settle((device, recording))
            existing = self._job(device, recording)
            if existing["pin_id"]:
                raise RelayError(503, "configuration_cleanup_pending", "previous attempt cleanup is pending")
        task = self._task_id(device, recording)
        pin = "attempt:" + uuid.uuid4().hex
        pinned = self.broker.pin_active(pin)
        revision = pinned["revision"]
        try:
            if expected_revision is not None and expected_revision != revision:
                raise RelayError(409, "revision_conflict", "configuration changed before retry")
            config = self.broker.read_snapshot(revision, pin)["config"]
            if not config["qwen_api_key"] or not config["siyuan_token"]:
                raise RelayError(503, "not_configured", "ASR and delivery credentials are required")
        except Exception:
            self.broker.release(pin)
            raise
        try:
            with self.store.connect() as db:
                if existing:
                    db.execute("UPDATE relay_jobs SET revision=?,pin_id=?,target_id=?,phase='receiving',updated_at=? WHERE task_id=?",
                               (revision, pin, target_id(config), self._now(), task))
                else:
                    db.execute("INSERT INTO relay_jobs(task_id,device_id,recording_id,source,revision,target_id,phase,owner_generation,created_at,updated_at,claimed_hash,audio_bytes,pin_id) VALUES(?,?,?,?,?,?,'receiving',?,?,?,?,?,?)",
                               (task, device, recording, source, revision, target_id(config), self.owner_generation,
                                self._now(), self._now(), claimed_hash, audio_bytes, pin))
        except Exception:
            # A failed commit can be uncertain. Do not drop a possibly owned pin.
            try:
                durable = self._job(device, recording)
                if not durable or durable["pin_id"] != pin:
                    self.broker.release(pin)
                else:
                    self._startup_error = "task_storage_failed"
                    self.draining = True
            except Exception:
                self._startup_error = "task_storage_failed"
                self.draining = True
            raise
        return self._job(device, recording)

    def admit_wav(self, headers, length):
        device, recording, claimed = self.auth.preflight_upload(headers)
        if type(length) is not int or length < 45 or length > MAX_WAV_BYTES:
            raise RelayError(413, "request_too_large", "invalid WAV body length")
        self._require_new()
        key = device, recording
        with self._mutex:
            self._require_new()
            if key in self._admissions or len(self._admissions) >= MAX_RECEIVERS or key == self._active:
                raise RelayError(429, "upload_busy", "bounded upload capacity is in use")
            job = self._job(*key)
            if job and (job["source"] != "wav" or job["phase"] in {"queued", "transcribing", "sending", "uncertain"} | TERMINAL):
                raise RelayError(409, "recording_state_conflict", "query the existing recording before uploading")
            reserved = sum(a.length for a in self._admissions.values())
            if shutil.disk_usage(self.workdir).free < length + reserved + self.min_free_bytes:
                raise RelayError(507, "storage_full", "insufficient private temporary storage")
            job = self._pin_job(device, recording, "wav", claimed, length)
            path = self.workdir / ("wav-" + uuid.uuid4().hex + ".wav")
            admission = Admission(path, device, recording, job["task_id"], length)
            self._admissions[key] = admission
            try:
                descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
                os.close(descriptor)
                self._update_job(*key, phase="receiving", wav_name=path.name, owner_generation=self.owner_generation, updated_at=self._now())
            except Exception as error:
                self.release_admission(admission)
                if isinstance(error, OSError):
                    raise RelayError(507, "storage_unavailable", "could not reserve private receive storage") from None
                raise
            return admission

    def release_admission(self, admission):
        if not isinstance(admission, Admission):
            raise ValueError("invalid admission")
        with self._mutex:
            if admission.released:
                return
            key = admission.device_id, admission.recording_id
            if self._admissions.get(key) is not admission:
                raise ValueError("admission does not belong to runtime")
            try:
                if not admission.transferred:
                    memo = self.store.get(*key)
                    if memo and memo["status"] in {"received", "transcribing"}:
                        self.store.update(*key, status="transcription_failed", error="wav_reupload_required", updated_at=self._now())
                    self._update_job(*key, phase="awaiting_audio", wav_name=None, owner_generation=None, updated_at=self._now())
            except Exception:
                # Storage failure may make the durable result uncertain. Stop
                # accepting work; startup recovery can repair it without ACKs.
                self._startup_error = "task_storage_failed"
                self.draining = True
                raise
            finally:
                self._admissions.pop(key)
                admission.released = True
            if not admission.transferred:
                try:
                    self._settle(key)
                except Exception:
                    self._startup_error = "task_storage_failed"
                    self.draining = True
                    raise
                try:
                    admission.path.unlink(missing_ok=True)
                except OSError:
                    # The durable job no longer protects abandoned receive
                    # audio. Ordinary cleanup retries transient file failures.
                    self._cleanup_error = "wav_cleanup_failed"

    def upload_path(self, headers, path, actual_hash=None, *, admission=None):
        device, recording, claimed = self.auth.preflight_upload(headers)
        key = device, recording
        if admission is None or self._admissions.get(key) is not admission or admission.released or pathlib.Path(path) != admission.path:
            raise RelayError(409, "upload_admission_required", "a live upload admission is required")
        if admission.path.stat().st_size != admission.length:
            raise RelayError(400, "incomplete_body", "WAV body length differs from Content-Length")
        digest = actual_hash or sha256_source(admission.path)
        if digest != claimed:
            raise RelayError(400, "audio_hash_mismatch", "audio SHA-256 does not match body")
        wav = validate_wav(admission.path)
        with admission.path.open("rb") as file:
            os.fsync(file.fileno())
        created, memo = self.store.create(device, recording, digest, wav["duration_ms"], self._now())
        if memo["audio_sha256"] != digest or memo["duration_ms"] != wav["duration_ms"]:
            raise RelayError(409, "recording_conflict", "recording ID belongs to different audio")
        if not created and memo["status"] not in {"transcription_failed", "received", "transcribing"}:
            raise RelayError(409, "recording_state_conflict", "recording cannot accept replacement audio")
        memo = self.store.update(*key, status="received", audio_bytes=admission.length, error=None, updated_at=self._now())
        self._update_job(*key, phase="queued", claimed_hash=digest, audio_bytes=admission.length, updated_at=self._now())
        admission.transferred = True
        self._enqueue("wav", key)
        return self.auth.public_memo(memo), 202

    def _service_for(self, key):
        job = self._job(*key)
        if not job or not job["revision"]:
            raise RelayError(409, "configuration_unavailable", "task configuration is not bound")
        snapshot = self.broker.read_snapshot(job["revision"], job["pin_id"] or job["task_id"])
        if snapshot["revision"] != job["revision"]:
            raise RelayError(503, "configuration_mismatch", "task configuration revision differs")
        if target_id(snapshot["config"]) != job["target_id"]:
            raise RelayError(503, "configuration_mismatch", "task destination differs from its durable binding")
        return self._provider_factory(self.store, snapshot["config"], progress=lambda: self._progress(key))

    def _progress(self, key):
        self._update_job(*key, updated_at=self._now(), owner_generation=self.owner_generation)
        job = self._job(*key)
        if job["source"] == "stream":
            self.streams._update(*key, updated_at=self._now(), expires_at=self._now() + RETENTION_MS)

    def _enqueue(self, source, key):
        with self._mutex:
            work = source, key
            if work not in self._scheduled:
                self._scheduled.add(work)
                self._queue.put(work)

    def _stream_queued(self, key):
        self._update_job(*key, phase="queued", updated_at=self._now())
        self._enqueue("stream", key)

    def _run_wav(self, key):
        job = self._job(*key)
        memo = self.store.get(*key)
        name = job["wav_name"]
        if name and not re.fullmatch(r"wav-[a-f0-9]{32}\.wav", name):
            raise RelayError(503, "damaged_task_metadata", "invalid temporary audio reference")
        wav = self.workdir / name if name else None
        try:
            service = self._service_for(key)
            if memo["status"] in TERMINAL | {"uncertain", "sending"}:
                return
            if memo["status"] not in {"transcribed", "delivery_failed"} or not memo["cloud_content"]:
                if not wav or wav.is_symlink() or not wav.is_file() or sha256_source(wav) != memo["audio_sha256"]:
                    raise RelayError(409, "wav_reupload_required", "verified SD original is required")
                self._update_job(*key, phase="transcribing", owner_generation=self.owner_generation, updated_at=self._now())
                self.store.update(*key, status="transcribing", error=None, updated_at=self._now())
                text, request = service._transcribe_source(wav, memo["duration_ms"])
                imported = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(memo["created_at"] / 1000))
                content = f"**喵伴 #{key[1]}** · 云端导入时间 {imported}\n\n{text}"
                memo = self.store.update(*key, status="transcribed", transcript=text, cloud_content=content,
                                         transcript_sha256=hashlib.sha256(text.encode()).hexdigest(), qwen_request_id=request,
                                         error=None, updated_at=self._now())
            self._update_job(*key, phase="sending", updated_at=self._now())
            service._deliver(memo, self._now())
        except Exception as error:
            memo = self.store.get(*key)
            code = getattr(error, "code", "processing_failed")
            if not isinstance(code, str) or not re.fullmatch(r"[a-z_]{1,80}", code):
                code = "processing_failed"
            if memo and memo["status"] == "sending":
                self.store.update(*key, status="uncertain", error=code, updated_at=self._now())
            elif memo and memo["status"] == "transcribed":
                # Durable text exists, but the sending boundary was not reached.
                self.store.update(*key, status="delivery_failed", error=code, updated_at=self._now())
            elif memo and memo["status"] not in TERMINAL | {"uncertain", "delivery_failed"}:
                self.store.update(*key, status="transcription_rejected" if code == "transcript_rejected" else "transcription_failed",
                                  error=code, updated_at=self._now())
        finally:
            if wav:
                try:
                    wav.unlink(missing_ok=True)
                except OSError:
                    self._cleanup_error = "wav_cleanup_failed"
            self._update_job(*key, wav_name=None, owner_generation=None)
            self._settle(key)

    def _settle(self, key):
        with self._mutex:
            job = self._job(*key)
            if not job or key == self._active or key in self._admissions or (job["source"], key) in self._scheduled:
                return
            memo = self.store.get(*key)
            state = memo["status"] if memo else job["phase"]
            if job["source"] == "stream" and memo and state in {"transcription_failed", "delivery_failed"}:
                # An accepted explicit retry queues a new stream attempt while
                # retaining its previous failed memo/text until the worker runs.
                # On restart, that old result must not release the queued pin.
                if self.streams._row(*key)["phase"] == "queued":
                    state = "queued"
            if job["source"] == "stream" and not memo:
                try:
                    row = self.streams._row(*key)
                except StreamError as error:
                    if error.code == "not_found" and self._discard_unstarted_stream(key):
                        return
                    raise
                state = row["phase"]
            fields = {"phase": state, "owner_generation": None}
            idle = state in STOPPED
            pending_release = job["phase"] in STOPPED and job["pin_id"]
            if idle or pending_release:
                # A stopped phase plus its existing pin is the durable release
                # record. Keep it until acknowledgement; release is idempotent.
                # Startup reconciliation excludes these stopped references, so
                # a lost reply cannot resurrect an already-collected snapshot.
                if job["pin_id"]:
                    if not pending_release:
                        self._update_job(*key, **fields, updated_at=self._now())
                    if not self._release_stopped_pin(job["pin_id"]):
                        return
                fields.update(revision=None, pin_id=None)
            if any(job.get(name) != value for name, value in fields.items()):
                self._update_job(*key, **fields, updated_at=self._now())

    def _release_stopped_pin(self, pin):
        try:
            self.broker.release(pin)
            return True
        except (BrokerError, OSError):
            # Routine cleanup retries; a temporary broker failure must not
            # poison the processing worker or discard the durable reference.
            self._cleanup_error = "configuration_cleanup_pending"
            return False

    def _discard_unstarted_stream(self, key):
        """Recover an interrupted create that never established an ACK-able row.

        Evidence of memo/chunk data forbids this cleanup: missing authoritative
        stream metadata in that case is corruption, not an unused reservation.
        """
        with self._mutex:
            with self.store.connect() as db:
                db.execute("BEGIN IMMEDIATE")
                job = db.execute("SELECT * FROM relay_jobs WHERE device_id=? AND recording_id=?", key).fetchone()
                if not job or job["source"] != "stream":
                    return False
                for table in ("streams", "stream_chunks", "memos"):
                    if db.execute("SELECT 1 FROM " + table + " WHERE device_id=? AND recording_id=? LIMIT 1", key).fetchone():
                        return False
            # Also retain a reservation interrupted before its stream row.
            self._update_job(*key, phase="awaiting_audio", owner_generation=None)
            if not self._release_stopped_pin(job["pin_id"] or job["task_id"]):
                return True
            with self.store.connect() as db:
                db.execute("DELETE FROM relay_jobs WHERE task_id=?", (job["task_id"],))
            return True

    def run_pending_once(self, timeout=0):
        if self._maintenance or self._startup_error or self._cleanup_error or not self.streams.cleanup_ok:
            return False
        try:
            source, key = self._queue.get(timeout=timeout)
        except queue.Empty:
            return False
        work = source, key
        try:
            with self._mutex:
                self._active = key
            if source == "wav":
                self._run_wav(key)
            else:
                self.streams.run_pending_once()
        finally:
            with self._mutex:
                self._active = None
                self._scheduled.discard(work)
            self._queue.task_done()
        self._settle(key)
        return True

    def _worker(self):
        while not self._stop.is_set():
            try:
                if not self.run_pending_once(timeout=0.25):
                    self._stop.wait(0.05)
            except Exception:
                self._startup_error = "task_recovery_required"
                self._stop.wait(0.25)

    def cleanup_once(self):
        self.streams.expire()
        cleanup_failed = False
        with self.store.connect() as db:
            protected = {r[0] for r in db.execute("SELECT wav_name FROM relay_jobs WHERE wav_name IS NOT NULL AND phase IN ('receiving','queued','transcribing','sending')")}
        with self._mutex:
            protected.update(a.path.name for a in self._admissions.values())
        for path in self.workdir.iterdir():
            if path.name not in protected and re.fullmatch(r"wav-[a-f0-9]{32}\.wav", path.name):
                try:
                    path.unlink()
                except OSError:
                    cleanup_failed = True
        self._cleanup_error = "wav_cleanup_failed" if cleanup_failed else None
        with self.store.connect() as db:
            keys = [(r[0], r[1]) for r in db.execute("SELECT device_id,recording_id FROM relay_jobs WHERE phase NOT IN ('sent','transcription_rejected','audio_expired') OR pin_id IS NOT NULL")]
        for key in keys:
            if key != self._active and key not in self._admissions:
                self._settle(key)

    def _cleanup_worker(self):
        while not self._stop.is_set():
            try:
                self.cleanup_once()
            except Exception:
                self._cleanup_error = "cleanup_failed"
            self._stop.wait(0.25)

    def authorize_stream(self, authorization, device, recording):
        self.streams.authorize(authorization, device, recording)

    def stream_create(self, headers, body):
        device, text = headers.get("X-Device-ID", ""), headers.get("X-Recording-ID", "")
        if not isinstance(text, str) or not re.fullmatch(r"[1-9][0-9]{0,9}", text) or int(text) > 0xffffffff:
            raise StreamError(400, "invalid_recording_id", "valid recording ID required")
        key = device, int(text)
        self.streams.authorize(headers.get("Authorization"), *key)
        if (body != FORMAT or not isinstance(body, dict) or
                any(type(body.get(name)) is not int for name in ("sample_rate", "channels", "frame_ms", "bitrate"))):
            raise StreamError(400, "unsupported_format", "supported Opus stream format required")
        with self._mutex:
            old = self._job(*key)
            if old and old["source"] == "stream":
                try:
                    return self.streams.status(headers.get("Authorization"), *key)
                except StreamError as error:
                    if error.code != "not_found":
                        raise
            self._require_new()
            self._pin_job(*key, "stream")
            try:
                return self.streams.create(headers, body)
            except Exception:
                self._discard_unstarted_stream(key)
                raise

    def _require_existing(self, key):
        if self._maintenance or self._startup_error:
            raise RelayError(503, "maintenance", "task recovery is not ready")
        job = self._job(*key)
        if not job:
            raise RelayError(404, "not_found", "task not found")
        if job["revision"]:
            self.broker.read_snapshot(job["revision"], job["pin_id"])

    def stream_put(self, authorization, device, recording, seq, headers, body):
        self.authorize_stream(authorization, device, recording)
        self._require_existing((device, recording))
        result = self.streams.put_chunk(authorization, device, recording, seq, headers, body)
        self._update_job(device, recording, updated_at=self._now())
        return result

    def stream_status(self, authorization, device, recording):
        result = self.streams.status(authorization, device, recording)
        self._settle((device, recording))
        return result

    def stream_finish(self, authorization, device, recording, body):
        self.authorize_stream(authorization, device, recording)
        if self.streams._row(device, recording)["final_samples"] is not None:
            return self.streams.finish(authorization, device, recording, body)
        self._require_existing((device, recording))
        return self.streams.finish(authorization, device, recording, body)

    def stream_retry(self, authorization, device, recording):
        self.authorize_stream(authorization, device, recording)
        if self.streams._row(device, recording)["phase"] == "audio_expired":
            raise StreamError(410, "audio_expired", "temporary audio has expired")
        return self.retry_job(device, recording)

    def memo_retry(self, authorization, device, recording):
        # Device adapter for the existing deliberate WAV delivery retry.
        self.auth.status(authorization, device, recording)
        job = self._job(device, recording)
        if not job or job["source"] != "wav":
            raise RelayError(409, "wrong_task_source", "WAV retry requires a WAV task")
        return self.retry_job(device, recording)

    def memo_status(self, authorization, device, recording):
        return self.auth.status(authorization, device, recording)


    def list_jobs(self, limit=50, offset=0):
        if type(limit) is not int or not 1 <= limit <= 100 or type(offset) is not int or not 0 <= offset <= 1000000:
            raise RelayError(400, "invalid_pagination", "bounded pagination required")
        with self.store.connect() as db:
            jobs = [dict(r) for r in db.execute("SELECT * FROM relay_jobs ORDER BY created_at DESC,device_id,recording_id LIMIT ? OFFSET ?", (limit, offset))]
            count = db.execute("SELECT COUNT(*) FROM relay_jobs").fetchone()[0]
        items = []
        for job in jobs:
            key = job["device_id"], job["recording_id"]
            memo = self.store.get(*key)
            status = memo["status"] if memo else job["phase"]
            error = memo["error"] if memo else None
            # Error text is reduced to a code; no provider response or user text.
            reason = (error or "").split(":", 1)[0]
            if not re.fullmatch(r"[a-z_]{0,80}", reason):
                reason = "processing_failed"
            retryable = status in {"transcribed", "delivery_failed"} or (job["source"] == "stream" and status == "transcription_failed")
            duration = memo["duration_ms"] if memo else 0
            if job["source"] == "stream":
                try:
                    public = self.streams._public(self.streams._row(*key))
                    retryable, duration = public["can_retry"], public["duration_ms"]
                except StreamError:
                    retryable = False
            if job["phase"] == "awaiting_audio" and not reason:
                reason = "wav_reupload_required"
            items.append({"device_id": key[0], "recording_id": key[1], "source": job["source"],
                          "duration_ms": duration, "status": status, "reason": reason or None,
                          "can_retry": retryable and key != self._active,
                          "target_id": job["target_id"], "revision": job["revision"], "updated_at": job["updated_at"]})
        return {"items": items, "total": count, "limit": limit, "offset": offset}

    def retry_job(self, device, recording, expected_revision=None):
        """A deliberate retry starts an attempt using the current configuration.

        It never mutates a running attempt or retries an uncertain delivery.
        """
        self._validate_key(device, recording)
        if expected_revision is not None and (not isinstance(expected_revision, str) or not re.fullmatch(r"[a-f0-9]{32}", expected_revision)):
            raise RelayError(400, "invalid_retry", "invalid configuration revision")
        self._require_new()
        key = device, recording
        with self._mutex:
            job, memo = self._job(*key), self.store.get(*key)
            if not job or not memo:
                raise RelayError(404, "not_found", "task not found")
            if key == self._active or key in self._admissions or (job["source"], key) in self._scheduled:
                raise RelayError(409, "processing_active", "task is already active")
            if memo["status"] in {"uncertain", "sending"}:
                raise RelayError(409, "delivery_uncertain", "resolve this delivery result before retrying")
            if memo["status"] not in {"transcribed", "delivery_failed", "transcription_failed"}:
                raise RelayError(409, "cannot_retry", "task cannot be retried")
            if job["source"] == "wav" and (memo["status"] not in {"transcribed", "delivery_failed"} or not memo["cloud_content"]):
                raise RelayError(409, "wav_reupload_required", "upload the SD original to retry transcription")
            if job["source"] == "stream" and not self.streams._public(self.streams._row(*key))["can_retry"]:
                raise RelayError(409, "cannot_retry", "retry content is unavailable")
            self._bind_active(device, recording, job["source"], job, expected_revision=expected_revision)
            try:
                if job["source"] == "wav":
                    self.store.update(*key, status="transcribed", error=None, updated_at=self._now())
                    self._update_job(*key, phase="queued", updated_at=self._now())
                    self._enqueue("wav", key)
                    return self.auth.public_memo(self.store.get(*key)), 202
                return self.streams.retry_authorized(device, recording)
            except Exception:
                self._settle(key)
                raise

    def reconcile_job(self, device, recording, present):
        self._validate_key(device, recording)
        if type(present) is not bool:
            raise RelayError(400, "invalid_reconciliation", "present must be boolean")
        key = device, recording
        with self._mutex:
            if key == self._active:
                raise RelayError(409, "processing_active", "active delivery cannot be reconciled")
            memo = self.store.get(*key)
            if not memo or memo["status"] not in {"uncertain", "sending"}:
                raise RelayError(409, "not_uncertain", "task does not need reconciliation")
            if present and not memo["audio_sha256"]:
                raise RelayError(409, "audio_identity_unavailable", "missing audio identity cannot authorize a completed recording")
            missing_content = not present and not memo["cloud_content"]
            fields = ({"status": "sent", "transcript": None, "cloud_content": None, "sent_at": self._now()} if present else
                      {"status": "audio_expired", "transcript": None, "cloud_content": None} if missing_content else
                      {"status": "transcribed"})
            updated = self.store.update(*key, **fields,
                error="retry_content_unavailable" if missing_content else None, updated_at=self._now())
            self.streams.reconciled(*key, present)
            self._settle(key)
            return self.auth.public_memo(updated), 200

    @staticmethod
    def _validate_key(device, recording):
        if (not isinstance(device, str) or not re.fullmatch(r"[A-Za-z0-9._-]{1,64}", device) or
                type(recording) is not int or not 1 <= recording <= 0xffffffff):
            raise RelayError(400, "invalid_recording_id", "valid device and uint32 recording ID required")



    def probe(self, provider, mode="validate", allow_side_effect=False):
        task = "probe:" + uuid.uuid4().hex
        with self._mutex:
            if self._closed or self.draining:
                raise RelayError(503, "maintenance", "configuration probes are paused for maintenance")
            self._probes[task] = None
        pinned = None
        try:
            pinned = self.broker.pin_active(task)
            with self._mutex:
                self._probes[task] = pinned["revision"]
            config = self.broker.read_snapshot(pinned["revision"], task)["config"]
            return probe_config(config, provider, mode, allow_side_effect)
        finally:
            try:
                if pinned:
                    self.broker.release(task)
            finally:
                with self._mutex:
                    self._probes.pop(task, None)

    def drain(self):
        with self._mutex:
            self.draining = True
        return self.drain_status()

    def drain_status(self):
        with self.store.connect() as db:
            sessions = db.execute("SELECT COUNT(*) FROM streams WHERE phase='receiving'").fetchone()[0]
            queued = db.execute("SELECT COUNT(*) FROM relay_jobs WHERE phase IN ('queued','transcribing','sending')").fetchone()[0]
        with self._mutex:
            requests = len(self._admissions) + len(self._probes)
            jobs = max(queued, len(self._scheduled), int(self._active is not None))
        return {"draining": self.draining, "active_sessions": sessions, "active_jobs": jobs,
                "active_requests": requests, "safe_to_stop": not (sessions or jobs or requests)}

    def resume(self):
        if self._closed:
            raise RelayError(409, "closed", "runtime is closed")
        with self._mutex:
            if self._startup_error or self.broker.view().get("maintenance"):
                if self._active is not None or self._admissions or self._probes:
                    raise RelayError(409, "recovery_busy", "wait for existing operations")
                self._reconcile_pins()
                if self._startup_error:
                    raise RelayError(503, self._startup_error, "configuration recovery remains incomplete")
            self._maintenance = False
            self.draining = False
            self._recover_queues()
            self._start_threads()
        return self.readiness()

    def close(self, timeout=10):
        if self._closed:
            return
        with self._mutex:
            self.draining = True
        self._stop.set()
        for thread in (self._worker_thread, self._cleanup_thread):
            if thread:
                thread.join(timeout)
                if thread.is_alive():
                    raise RuntimeError("active task has not stopped; runtime lock retained")
        if self._active or self._admissions or self._probes:
            raise RuntimeError("active request has not stopped; runtime lock retained")
        self.streams.close(timeout)
        with self.store.connect() as db:
            db.execute("PRAGMA wal_checkpoint(TRUNCATE)")
        self._closed = True
        self._unlock()
