"""Read-only mounted, isolated acceptance fixture. Never copied into runtime images.

Uses the production Runtime, HTTP app, broker and codecs. Provider callbacks,
response transport, an explicit clock offset and scoped removal failures may be
substituted; no test flag or route enters production.
"""
from __future__ import annotations
import asyncio
import ctypes
from contextlib import contextmanager
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
import math
import os
import resource
from pathlib import Path
import sqlite3
import struct
import sys
import threading
import time
import wave

DEVICE = "container-synthetic-device"
PHASES = {"transcribing", "transcribed", "sending", "sent", "chunk_ack", "finish_ack", "chunk_partial"}


def opus_chunk(frames=250, frequency=440):
    from stream_service import OpusLibrary
    lib = OpusLibrary().lib
    lib.opus_encoder_create.argtypes = [ctypes.c_int32, ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
    lib.opus_encoder_create.restype = ctypes.c_void_p
    lib.opus_encoder_destroy.argtypes = [ctypes.c_void_p]
    lib.opus_encoder_ctl.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.opus_encode.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int16), ctypes.c_int, ctypes.c_void_p, ctypes.c_int32]
    error = ctypes.c_int()
    encoder = lib.opus_encoder_create(16000, 1, 2048, ctypes.byref(error))
    if not encoder or error.value:
        raise RuntimeError("libopus encoder unavailable")
    try:
        for request, value in ((4002, 24000), (4006, 0), (4016, 0), (4010, 5)):
            if lib.opus_encoder_ctl(encoder, request, ctypes.c_int(value)):
                raise RuntimeError("libopus encoder configuration failed")
        output = (ctypes.c_ubyte * 1275)()
        result = bytearray()
        for frame in range(frames):
            pcm = (ctypes.c_int16 * 320)(*[round(12000 * math.sin(2 * math.pi * frequency * (frame * 320 + n) / 16000)) for n in range(320)])
            length = lib.opus_encode(encoder, pcm, 320, output, len(output))
            if length != 60:
                raise RuntimeError("unexpected CBR frame length")
            result.extend(struct.pack("<H", length)); result.extend(output[:length])
        return bytes(result)
    finally:
        lib.opus_encoder_destroy(encoder)


class Ledger:
    def __init__(self, path):
        self.path = path
        self.condition = threading.Condition()
        self.arms = set()
        self.partial_targets = set()
        self.modes = {}
        self.cleanup_faults = set()
        with self.db() as db:
            db.execute("CREATE TABLE IF NOT EXISTS events(id INTEGER PRIMARY KEY,kind TEXT,recording INTEGER,detail TEXT)")
            db.execute("CREATE TABLE IF NOT EXISTS fixture_clock(singleton INTEGER PRIMARY KEY CHECK(singleton=1),offset_ms INTEGER NOT NULL)")
            db.execute("INSERT OR IGNORE INTO fixture_clock VALUES(1,0)")
            db.execute("CREATE TABLE IF NOT EXISTS fixture_revoked(digest TEXT PRIMARY KEY)")
            self.offset_ms = db.execute("SELECT offset_ms FROM fixture_clock WHERE singleton=1").fetchone()[0]
            self.revoked = {row[0] for row in db.execute("SELECT digest FROM fixture_revoked")}

    def clock(self):
        with self.condition:
            return int(time.time() * 1000) + self.offset_ms

    def advance_clock(self, milliseconds):
        if type(milliseconds) is not int or not 0 < milliseconds <= 3 * 86400000:
            raise ValueError("bounded positive fixture time advance required")
        with self.condition:
            with self.db() as db:
                db.execute("UPDATE fixture_clock SET offset_ms=offset_ms+? WHERE singleton=1", (milliseconds,))
            self.offset_ms += milliseconds
            return {"kind": "synthetic-offset-clock", "offset_ms": self.offset_ms, "now_ms": self.clock()}

    def provider_mode(self, recording, mode):
        if type(recording) is not int or not 1 <= recording <= 0xffffffff or mode not in {"normal", "reject", "asr_fail"}:
            raise ValueError("invalid synthetic provider mode")
        with self.condition:
            self.modes[recording] = mode

    def revoke_key(self, digest):
        if not isinstance(digest, str) or len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest):
            raise ValueError("exact synthetic credential digest required")
        with self.condition:
            with self.db() as db:
                db.execute("INSERT OR IGNORE INTO fixture_revoked VALUES(?)", (digest,))
            self.revoked.add(digest)

    def cleanup_fault(self, recording, enabled):
        if type(recording) is not int or not 1 <= recording <= 0xffffffff or type(enabled) is not bool:
            raise ValueError("invalid synthetic cleanup failure target")
        with self.condition:
            if enabled: self.cleanup_faults.add(recording)
            else: self.cleanup_faults.discard(recording)

    @contextmanager
    def db(self):
        connection = sqlite3.connect(self.path, timeout=20)
        connection.execute("PRAGMA synchronous=FULL")
        try:
            with connection: yield connection
        finally: connection.close()

    def event(self, kind, recording, detail=None):
        with self.db() as db:
            db.execute("INSERT INTO events(kind,recording,detail) VALUES(?,?,?)", (kind, recording, json.dumps(detail)))

    def events(self, recording):
        with self.db() as db:
            return [{"kind": row[0], "detail": json.loads(row[1])} for row in db.execute("SELECT kind,detail FROM events WHERE recording=? ORDER BY id", (recording,))]

    def arm(self, phase, recording, sequence=None):
        if phase not in PHASES or type(recording) is not int or not 1 <= recording <= 0xffffffff:
            raise ValueError("invalid fixture phase")
        if phase == "chunk_partial" and (type(sequence) is not int or not 0 <= sequence <= 0x7fffffff):
            raise ValueError("partial request requires exact sequence")
        if phase != "chunk_partial" and sequence is not None:
            raise ValueError("sequence is only valid for partial request")
        with self.condition:
            self.arms.add((phase, recording))
            if phase == "chunk_partial": self.partial_targets.add((recording, sequence))

    def release(self):
        with self.condition:
            self.arms.clear(); self.partial_targets.clear(); self.condition.notify_all()

    def gate(self, phase, recording):
        with self.condition:
            if (phase, recording) not in self.arms:
                return
            self.event("gate:" + phase, recording)
            while (phase, recording) in self.arms:
                self.condition.wait(1)


def provider_factory(ledger):
    from app import RelayService, RelayError
    from relay.providers import target_id
    class SyntheticProvider(RelayService):
        def _deliver(self, row, now, *, title=None):
            recording = row["recording_id"]
            ledger.gate("transcribed", recording)  # real store.update already committed
            self.fixture_recording = recording
            result = super()._deliver(row, now, title=title)
            ledger.gate("sent", recording)  # real sent and text cleanup already committed
            return result
    def factory(store, config, *, progress=None):
        service = SyntheticProvider(store, config["qwen_api_key"], config["siyuan_token"], progress=progress)
        asr_digest = hashlib.sha256(config["qwen_api_key"].encode()).hexdigest()
        delivery_digest = hashlib.sha256(config["siyuan_token"].encode()).hexdigest()
        destination = target_id(config)
        def transcribe(data, duration):
            # The runtime has exactly one processing worker. Identify that real
            # SQLite task rather than inventing phase state in the fixture.
            with store.connect() as db:
                rows = db.execute("SELECT recording_id FROM memos WHERE status='transcribing'").fetchall()
            if len(rows) != 1:
                raise RuntimeError("ASR task cannot be uniquely identified")
            recording = rows[0][0]
            with wave.open(io.BytesIO(data), "rb") as audio:
                if audio.getnframes() * 1000 // 16000 != duration:
                    raise RuntimeError("ASR received invalid synthetic WAV duration")
            ledger.event("asr", recording, {"duration_ms": duration, "wav_bytes": len(data), "credential_sha256": asr_digest, "target_id": destination,
                "temporary_audio_bytes": sum(p.stat().st_size for folder in (ledger.path.parent / "streams", Path(os.environ.get("VOICE_MEMO_WORK_DIR", ledger.path.parent / "work"))) for p in folder.rglob("*") if p.is_file())})
            ledger.gate("transcribing", recording)
            with ledger.condition:
                mode, revoked = ledger.modes.get(recording, "normal"), asr_digest in ledger.revoked
            if mode == "asr_fail" or revoked:
                raise RelayError(502, "qwen_rejected", "synthetic provider rejection")
            if mode == "reject":
                return "", "synthetic-quality-rejection"
            sequence = sum(e["kind"] == "asr" for e in ledger.events(recording))
            return f"合成验收片段{sequence} 记录{recording} 独立内容{hashlib.sha256(str(sequence).encode()).hexdigest()[:16]}", "fixture"
        def send(title, content):
            recording = service.fixture_recording
            with ledger.condition:
                revoked = delivery_digest in ledger.revoked
            if revoked:
                ledger.event("delivery_rejected", recording, {"credential_sha256": delivery_digest, "target_id": destination})
                raise RelayError(502, "siyuan_rejected", "synthetic revoked provider credential")
            ledger.event("delivery", recording, {"sha256": hashlib.sha256(content.encode()).hexdigest(), "credential_sha256": delivery_digest, "target_id": destination})
            ledger.gate("sending", recording)  # provider accepted; response intentionally absent
        service._qwen_transcriber, service._siyuan_sender = transcribe, send
        return service
    return factory


class DropResponse:
    def __init__(self, app, ledger):
        self.app, self.ledger = app, ledger

    async def __call__(self, scope, receive, send):
        async def intercept_receive():
            message = await receive()
            path = scope.get("path", "").split("/")
            if (scope["type"] == "http" and scope.get("method") == "PUT" and
                    len(path) == 7 and path[:4] == ["", "v1", "streams", DEVICE] and path[5] == "chunks" and
                    path[4].isdigit() and path[6].isdigit() and message["type"] == "http.request" and
                    message.get("body") and message.get("more_body") is True):
                target = (int(path[4]), int(path[6]))
                with self.ledger.condition:
                    if target in self.ledger.partial_targets:
                        # The real handler requested receive() and transport
                        # returned a non-final body fragment. Record once for
                        # this exact device/recording/chunk before allowing kill.
                        self.ledger.event("gate:chunk_partial", target[0], {"sequence": target[1],
                            "body_bytes": len(message["body"]), "more_body": True})
                        self.ledger.partial_targets.remove(target)
            return message
        async def intercept(message):
            if scope["type"] == "http" and message["type"] == "http.response.start" and message["status"] < 300:
                path = scope["path"].split("/")
                if "streams" in path:
                    try:
                        index = path.index("streams"); recording = int(path[index + 2])
                        phase = "chunk_ack" if "chunks" in path else "finish_ack" if path[-1] == "finish" else None
                    except (IndexError, ValueError):
                        phase = None
                    if phase:
                        with self.ledger.condition:
                            armed = (phase, recording) in self.ledger.arms
                        if armed:
                            self.ledger.event("gate:" + phase, recording)
                            while True:
                                with self.ledger.condition:
                                    armed = (phase, recording) in self.ledger.arms
                                if not armed: break
                                await asyncio.sleep(.05)  # cancellation must not strand an executor thread
            await send(message)
        await self.app(scope, intercept_receive, intercept)


def main():
    # This read-only fixture is mounted only by the acceptance driver. Default
    # runtime images/entrypoints contain no provider override, clock or probes.
    sys.path.insert(0, os.getcwd())
    import uvicorn
    from config_broker import BrokerClient
    from relay.runtime import Runtime
    from relay.web import create_app
    from relay.control import ControlServer
    from relay.jobs import RUNTIME_SCHEMA_VERSION
    from relay import __version__
    if os.geteuid() != 10001 or os.environ.get("VOICE_MEMO_FIXTURE_ISOLATED") != "1":
        raise RuntimeError("fixture requires isolated nonroot container")
    database = Path(os.environ["VOICE_MEMO_DB"])
    workdir = Path(os.environ["VOICE_MEMO_WORK_DIR"])
    streamdir = Path(os.environ["VOICE_MEMO_STREAM_DIR"])
    ledger = Ledger(database.parent / "fixture-ledger.sqlite3")
    broker = BrokerClient(os.environ["VOICE_MEMO_BROKER_SOCKET"])
    runtime = Runtime(database, broker, workdir, streamdir,
                      provider_factory=provider_factory(ledger), clock=ledger.clock)
    original_remove = runtime.streams._remove_audio
    def remove_audio(row):
        with ledger.condition:
            blocked = row["device_id"] == DEVICE and row["recording_id"] in ledger.cleanup_faults
        if blocked:
            ledger.event("cleanup_failure", row["recording_id"])
            raise OSError("synthetic per-record cleanup failure")
        return original_remove(row)
    runtime.streams._remove_audio = remove_audio
    app = create_app(database, public_url=os.environ["VOICE_MEMO_PUBLIC_URL"], broker=broker, runtime=runtime)
    # Initial owner, keys and device enrollment are deliberately NOT created
    # here: the driver uses the shipped CLI and public HTTP routes from empty DB.
    chunk, alternative_chunk = opus_chunk(), opus_chunk(frequency=660)
    def inspect(recording):
        with runtime.store.connect() as db:
            def row(table):
                found = db.execute(f"SELECT * FROM {table} WHERE device_id=? AND recording_id=?", (DEVICE, recording)).fetchone()
                return dict(found) if found else None
            memo, job, stream = row("memos"), row("relay_jobs"), row("streams")
            chunks = [dict(item) for item in db.execute("SELECT seq,sha256,samples FROM stream_chunks WHERE device_id=? AND recording_id=? ORDER BY seq", (DEVICE, recording))]
        directory = runtime.streams._path(DEVICE, recording)
        for item in chunks:
            path = directory / f"{item['seq']:010d}.opus"
            item["file_sha256"] = hashlib.sha256(path.read_bytes()).hexdigest() if path.exists() else None
        if memo:
            for field in ("transcript", "cloud_content"):
                memo[field] = bool(memo[field])
        return {"memo": memo, "job": job, "stream": stream, "chunks": chunks, "events": ledger.events(recording),
                "stream_files": sorted(p.name for p in directory.glob("*")), "work_files": sorted(p.name for p in workdir.glob("*")),
                "peak_process_rss_bytes": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss * 1024,
                "disk_bytes": sum(p.stat().st_size for directory in (workdir, streamdir) for p in directory.rglob("*") if p.is_file()),
                "cleanup_ok": runtime.streams.cleanup_ok, "cleanup_error": runtime.streams.cleanup_error,
                "fixture_clock": {"kind": "synthetic-offset-clock", "offset_ms": ledger.offset_ms, "now_ms": ledger.clock()}}
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_): pass
        def do_GET(self):
            if self.path in {"/chunk", "/chunk-alt"}:
                self.send_response(200); self.end_headers(); self.wfile.write(chunk if self.path == "/chunk" else alternative_chunk); return
            elif self.path.startswith("/inspect/"):
                value = inspect(int(self.path.rsplit("/", 1)[1]))
            elif self.path == "/clock":
                value = {"kind": "synthetic-offset-clock", "offset_ms": ledger.offset_ms, "now_ms": ledger.clock()}
            else:
                self.send_error(404); return
            data = json.dumps(value).encode(); self.send_response(200); self.send_header("Content-Length", str(len(data))); self.end_headers(); self.wfile.write(data)
        def do_POST(self):
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 <= length <= 1024:
                self.send_error(400); return
            value = json.loads(self.rfile.read(length) or b"{}")
            if self.path in {"/clock", "/provider-mode", "/revoke-key", "/cleanup-fault"}:
                try:
                    if self.path == "/clock":
                        if set(value) != {"advance_ms"}: raise ValueError("invalid fields")
                        result = ledger.advance_clock(value["advance_ms"])
                    elif self.path == "/provider-mode":
                        if set(value) != {"recording", "mode"}: raise ValueError("invalid fields")
                        ledger.provider_mode(value["recording"], value["mode"]); result = {"configured": True}
                    elif self.path == "/revoke-key":
                        if set(value) != {"sha256"}: raise ValueError("invalid fields")
                        ledger.revoke_key(value["sha256"]); result = {"configured": True}
                    else:
                        if set(value) != {"recording", "enabled"}: raise ValueError("invalid fields")
                        ledger.cleanup_fault(value["recording"], value["enabled"]); result = {"configured": True}
                except (ValueError, TypeError):
                    self.send_error(400); return
                data = json.dumps(result).encode(); self.send_response(200); self.send_header("Content-Length", str(len(data))); self.end_headers(); self.wfile.write(data); return
            if self.path == "/arm": ledger.arm(value["phase"], value["recording"], value.get("sequence"))
            elif self.path == "/release": ledger.release()
            else: self.send_error(404); return
            self.send_response(204); self.end_headers()
    probe = ThreadingHTTPServer(("0.0.0.0", 8766), Handler)
    threading.Thread(target=probe.serve_forever, daemon=True).start()
    control = ControlServer(Path(os.environ["VOICE_MEMO_CONTROL_SOCKET"]), runtime,
                            {"code_version": __version__, "schema_version": RUNTIME_SCHEMA_VERSION,
                             "instance_uuid": runtime.instance_uuid})
    try:
        uvicorn.run(DropResponse(app, ledger), host="0.0.0.0", port=8765, workers=1,
                    proxy_headers=True, forwarded_allow_ips=os.environ["VOICE_MEMO_FORWARDED_ALLOW_IPS"],
                    access_log=False, server_header=False, timeout_graceful_shutdown=90,
                    limit_concurrency=32, timeout_keep_alive=5)
    finally:
        control.close(); runtime.close(timeout=90); probe.shutdown()


if __name__ == "__main__":
    main()
