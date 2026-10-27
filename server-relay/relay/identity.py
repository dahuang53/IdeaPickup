"""Durable service identity and recoverable fresh-device pairing. Tokens remain hashes."""

from __future__ import annotations

import hashlib
import hmac
import ipaddress
import pathlib
import re
import secrets
import sqlite3
import time
import urllib.parse
import uuid
from contextlib import contextmanager
from typing import Callable


DEVICE_ID = re.compile(r"[A-Za-z0-9._-]{1,64}\Z")
TOKEN = re.compile(r"[a-f0-9]{64}\Z")
PAIRING_CODE = re.compile(r"[A-HJ-NP-Z2-9]{10}\Z")
PAIRING_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
CANDIDATE_TTL_MS = 15 * 60 * 1000
MAX_BASE_URL_BYTES = 240
PROTOCOL_VERSION = 2
ERRORS = {
    "invalid_device_id": 400, "invalid_url": 400, "invalid_enrollment": 400,
    "invalid_pairing_code": 401, "unauthorized": 401,
    "enrollment_expired": 410, "enrollment_revoked": 401,
    "enrollment_pending": 409, "enrollment_not_activated": 409,
    "enrollment_superseded": 409, "enrollment_conflict": 409,
}


class IdentityError(Exception):
    def __init__(self, status: int, code: str, message: str):
        super().__init__(message)
        self.status, self.code = status, code


class _CountedPairingRejection(Exception):
    """Commit only the failed-code counter, then return the public auth error."""


def _fail(code: str, message: str):
    raise IdentityError(ERRORS[code], code, message)


def _hash(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def _device(value: str):
    if not isinstance(value, str) or not DEVICE_ID.fullmatch(value):
        _fail("invalid_device_id", "invalid device ID")


def normalize_base_url(value: str) -> str:
    """Canonical HTTPS or literal RFC1918 HTTP origin/path prefix; no credentials or hidden URL parts."""
    try:
        encoded_length = len(value.encode("utf-8")) if isinstance(value, str) else 0
    except UnicodeError:
        _fail("invalid_url", "a valid service base URL is required")
    if (not isinstance(value, str) or not value or
            encoded_length > MAX_BASE_URL_BYTES or
            any(ord(c) <= 32 or ord(c) == 127 for c in value) or
            any(c in value for c in "?#\\")):
        _fail("invalid_url", "a bounded service base URL is required")
    try:
        if "://" not in value:
            bare = urllib.parse.urlsplit("//" + value)
            try:
                address = ipaddress.IPv4Address(bare.hostname or "")
                lan = any(address in ipaddress.IPv4Network(net) for net in ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16"))
            except ipaddress.AddressValueError:
                lan = False
            authority = bare.netloc + (":8765" if lan and bare.port is None else "")
            value = urllib.parse.urlunsplit(("http" if lan else "https", authority, bare.path, "", ""))
        parts = urllib.parse.urlsplit(value)
        if (parts.scheme not in {"http", "https"} or not parts.hostname or
                parts.username is not None or parts.password is not None):
            raise ValueError()
        host = parts.hostname
        if parts.scheme == "http":
            address = ipaddress.IPv4Address(host)
            if not any(address in ipaddress.IPv4Network(net) for net in ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16")):
                raise ValueError()
        try:
            address = ipaddress.ip_address(host)
            if getattr(address, "scope_id", None):
                raise ValueError()
            host = f"[{address.compressed}]" if address.version == 6 else address.compressed
        except ValueError:
            if re.fullmatch(r"[0-9.]+", host):
                raise ValueError()
            host = host.encode("ascii").decode("ascii").lower()
            if (len(host) > 253 or any(not re.fullmatch(r"[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?", p)
                                       for p in host.split("."))):
                raise ValueError()
        port_text = parts.netloc.rsplit("]", 1)[-1] if parts.netloc.startswith("[") else parts.netloc[len(parts.hostname):]
        if port_text and (not re.fullmatch(r":[1-9][0-9]{0,4}", port_text)):
            raise ValueError()
        port = parts.port
        if port is not None and not 1 <= port <= 65535:
            raise ValueError()
        authority = host + (f":{port}" if port is not None and port != (443 if parts.scheme == "https" else 80) else "")
        path = parts.path.rstrip("/")
        if re.search(r"%(?![0-9a-fA-F]{2})", path):
            raise ValueError()
        decoded = urllib.parse.unquote(path, errors="strict")
        if (any(ord(c) <= 32 or ord(c) == 127 for c in decoded) or
                "\\" in decoded or "//" in decoded or
                any(p in {".", ".."} for p in decoded.split("/")) or
                re.search(r"%(?:2f|5c)", path, re.I)):
            raise ValueError()
        result = urllib.parse.urlunsplit((parts.scheme, authority, path, "", ""))
        if len(result.encode("utf-8")) > MAX_BASE_URL_BYTES:
            raise ValueError()
        return result
    except (ValueError, UnicodeError):
        _fail("invalid_url", "a valid service base URL is required")


class IdentityStore:
    """Fresh pairing transactions share the ordinary devices/pairing_codes schema.

    Administrative methods are issue_pairing_code and revoke_device.
    They must never be exposed as unauthenticated routes.
    Every transition uses BEGIN IMMEDIATE, including credential validation
    that updates last_seen_at, so expiry/revocation cannot race activation.
    """

    def __init__(self, path: pathlib.Path | str, clock: Callable[[], int] | None = None):
        self.path = pathlib.Path(path)
        self.clock = clock or (lambda: int(time.time() * 1000))
        self.validate_existing(self.path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with self._connect() as db:
            # Changing a brand-new database to WAL can return SQLITE_BUSY
            # immediately despite busy_timeout, when constructors race. No
            # transaction has begun here, so only this pragma is safely retried.
            deadline = time.monotonic() + 15
            while True:
                try:
                    db.execute("PRAGMA journal_mode=WAL")
                    break
                except sqlite3.OperationalError as error:
                    if (getattr(error, "sqlite_errorcode", 0) & 0xff not in {sqlite3.SQLITE_BUSY, sqlite3.SQLITE_LOCKED}
                            or time.monotonic() >= deadline):
                        raise
                    time.sleep(0.01)
            db.executescript("""
                CREATE TABLE IF NOT EXISTS devices (
                    device_id TEXT PRIMARY KEY, token_sha256 TEXT NOT NULL UNIQUE,
                    created_at INTEGER NOT NULL, last_seen_at INTEGER, revoked_at INTEGER
                );
                CREATE TABLE IF NOT EXISTS pairing_codes (
                    code_sha256 TEXT PRIMARY KEY, created_at INTEGER NOT NULL,
                    expires_at INTEGER NOT NULL, attempts INTEGER NOT NULL DEFAULT 0,
                    used_at INTEGER
                );
                CREATE TABLE IF NOT EXISTS relay_identity (
                    singleton INTEGER PRIMARY KEY CHECK(singleton=1),
                    server_instance_uuid TEXT NOT NULL, schema_version INTEGER NOT NULL
                );
                CREATE TABLE IF NOT EXISTS device_identity_epochs (
                    device_id TEXT PRIMARY KEY, epoch INTEGER NOT NULL DEFAULT 0
                );
                CREATE TABLE IF NOT EXISTS device_enrollments (
                    enrollment_id TEXT PRIMARY KEY, device_id TEXT NOT NULL,
                    candidate_sha256 TEXT NOT NULL UNIQUE,
                    epoch INTEGER NOT NULL, phase TEXT NOT NULL,
                    created_at INTEGER NOT NULL, expires_at INTEGER NOT NULL,
                    activated_at INTEGER, finalized_at INTEGER
                );
                CREATE INDEX IF NOT EXISTS device_enrollments_device
                    ON device_enrollments(device_id, phase);
            """)
            db.execute("INSERT OR IGNORE INTO relay_identity VALUES (1, ?, 2)", (str(uuid.uuid4()),))
            row = db.execute("SELECT * FROM relay_identity WHERE singleton=1").fetchone()
            if row["schema_version"] != 2 or str(uuid.UUID(row["server_instance_uuid"])) != row["server_instance_uuid"]:
                raise ValueError("unsupported or damaged identity schema")
            self.server_instance_uuid = row["server_instance_uuid"]
        self.path.chmod(0o600)

    @staticmethod
    def validate_existing(path: pathlib.Path | str):
        """Never stamp unmarked historical credentials as a fresh installation.

        Empty tables and first-owner admin initialization may precede identity
        creation. Existing current-format identities remain valid on restart.
        This check does not migrate, reset or delete stored data.
        """
        path = pathlib.Path(path)
        if not path.exists():
            return
        db = sqlite3.connect(path.resolve().as_uri() + "?mode=ro", uri=True, timeout=15)
        try:
            tables = {row[0] for row in db.execute("SELECT name FROM sqlite_master WHERE type='table'")}
            if "relay_identity" in tables:
                row = db.execute("SELECT schema_version,server_instance_uuid FROM relay_identity WHERE singleton=1").fetchone()
                if row:
                    if row[0] != 2 or str(uuid.UUID(row[1])) != row[1]:
                        raise ValueError("unsupported or damaged identity schema")
                    return
            for table in ("devices", "pairing_codes", "device_enrollments", "device_identity_epochs"):
                if table in tables and db.execute("SELECT 1 FROM " + table + " LIMIT 1").fetchone():
                    raise ValueError("unmarked identity data requires a fresh database")
        finally:
            db.close()

    @contextmanager
    def _connect(self):
        db = sqlite3.connect(self.path, timeout=15)
        db.row_factory = sqlite3.Row
        db.execute("PRAGMA synchronous=FULL")
        try:
            with db:
                yield db
        finally:
            db.close()

    @contextmanager
    def _transaction(self):
        with self._connect() as db:
            db.execute("BEGIN IMMEDIATE")
            try:
                yield db
            except _CountedPairingRejection:
                # _consume_code is called before any successful pairing mutation.
                # Failed-code attempts remain committed even when pairing is rejected.
                db.commit()
                _fail("invalid_pairing_code", "pairing code is invalid, expired, or already used")

    def _now(self, now):
        return self.clock() if now is None else now

    @staticmethod
    def _epoch(db, device_id):
        db.execute("INSERT OR IGNORE INTO device_identity_epochs VALUES (?, 0)", (device_id,))
        return db.execute("SELECT epoch FROM device_identity_epochs WHERE device_id=?", (device_id,)).fetchone()[0]

    def info(self) -> dict:
        return {"service": "voice-memo-relay", "server_instance_uuid": self.server_instance_uuid,
                "protocol_version": PROTOCOL_VERSION,
                "capabilities": ["device-info", "enrollment-v2", "recoverable-enrollment-request", "server-audio-sha256", "task-cancel"],
                "limits": {"base_url_bytes": MAX_BASE_URL_BYTES, "candidate_ttl_seconds": 900,
                           "wav_bytes": 2_147_483_646, "stream_chunk_bytes": 65536},
                "audio_formats": {"wav": {"codec": "pcm_s16le", "sample_rate": 16000, "channels": 1},
                                  "streams": {"codec": "opus", "sample_rate": 16000, "channels": 1}}}

    def issue_pairing_code(self, ttl_seconds: int = 900, now: int | None = None) -> tuple[str, int]:
        if type(ttl_seconds) is not int or not 60 <= ttl_seconds <= 3600:
            raise ValueError("pairing code TTL must be 60..3600 seconds")
        now = self._now(now)
        for _ in range(8):
            code = "".join(secrets.choice(PAIRING_ALPHABET) for _ in range(10))
            try:
                with self._transaction() as db:
                    db.execute("INSERT INTO pairing_codes(code_sha256,created_at,expires_at) VALUES(?,?,?)",
                               (_hash(code), now, now + ttl_seconds * 1000))
                return code, now + ttl_seconds * 1000
            except sqlite3.IntegrityError:
                pass
        raise RuntimeError("could not allocate pairing code")

    @staticmethod
    def _consume_code(db, pairing_code, now):
        if not isinstance(pairing_code, str) or len(pairing_code) > 64:
            _fail("invalid_pairing_code", "valid one-time pairing code required")
        code = pairing_code.strip().upper().replace("-", "").replace(" ", "")
        if not PAIRING_CODE.fullmatch(code):
            _fail("invalid_pairing_code", "valid one-time pairing code required")
        digest = _hash(code)
        row = db.execute("SELECT * FROM pairing_codes WHERE code_sha256=?", (digest,)).fetchone()
        if not row or row["used_at"] is not None or row["expires_at"] <= now or row["attempts"] >= 5:
            if row and row["used_at"] is None:
                db.execute("UPDATE pairing_codes SET attempts=attempts+1 WHERE code_sha256=?", (digest,))
                raise _CountedPairingRejection()
            _fail("invalid_pairing_code", "pairing code is invalid, expired, or already used")
        db.execute("UPDATE pairing_codes SET used_at=?,attempts=attempts+1 WHERE code_sha256=?", (now, digest))


    def create_enrollment(self, device_id, pairing_code, enrollment_id, candidate_token, now=None):
        # The device persists these random values before its first request.
        # Replaying a lost response therefore never needs a token stored in plaintext.
        _device(device_id)
        try:
            valid_id = isinstance(enrollment_id, str) and str(uuid.UUID(enrollment_id)) == enrollment_id
        except (ValueError, AttributeError):
            valid_id = False
        if not valid_id or not isinstance(candidate_token, str) or not TOKEN.fullmatch(candidate_token):
            _fail("invalid_enrollment", "canonical enrollment ID and candidate credential required")
        now = self._now(now)
        with self._transaction() as db:
            existing = db.execute("SELECT * FROM device_enrollments WHERE enrollment_id=?", (enrollment_id,)).fetchone()
            if existing:
                row = self._enrollment(db, enrollment_id, candidate_token, now)
                if row["device_id"] != device_id:
                    _fail("enrollment_conflict", "request belongs to another device")
                return self._public_enrollment(db, row)
            db.execute("UPDATE device_enrollments SET phase='expired' WHERE device_id=? AND phase='created' AND expires_at<=?", (device_id, now))
            if db.execute("SELECT 1 FROM device_enrollments WHERE device_id=? AND phase IN ('created','activated')", (device_id,)).fetchone():
                _fail("enrollment_pending", "complete the existing pairing before creating another")
            if db.execute("SELECT 1 FROM device_enrollments WHERE candidate_sha256=?", (_hash(candidate_token),)).fetchone():
                _fail("enrollment_conflict", "candidate credential is already used")
            self._consume_code(db, pairing_code, now)
            epoch = self._epoch(db, device_id)
            db.execute("INSERT INTO device_enrollments(enrollment_id,device_id,candidate_sha256,epoch,phase,created_at,expires_at) VALUES(?,?,?,?,'created',?,?)",
                       (enrollment_id, device_id, _hash(candidate_token), epoch, now, now + CANDIDATE_TTL_MS))
            return self._public_enrollment(db, db.execute("SELECT * FROM device_enrollments WHERE enrollment_id=?", (enrollment_id,)).fetchone())

    def _public_enrollment(self, db, row):
        return {"enrollment_id": row["enrollment_id"], "device_id": row["device_id"],
                "server_instance_uuid": self.server_instance_uuid, "phase": row["phase"],
                "expires_at": row["expires_at"] if row["phase"] == "created" else None,
                "activated_at": row["activated_at"], "finalized_at": row["finalized_at"]}

    def _enrollment(self, db, identifier, token, now):
        if not isinstance(identifier, str) or len(identifier) != 36 or not isinstance(token, str) or not TOKEN.fullmatch(token):
            _fail("unauthorized", "valid enrollment credential required")
        row = db.execute("SELECT * FROM device_enrollments WHERE enrollment_id=?", (identifier,)).fetchone()
        if not row or not hmac.compare_digest(row["candidate_sha256"], _hash(token)):
            _fail("unauthorized", "valid enrollment credential required")
        if row["phase"] == "revoked" or row["epoch"] != self._epoch(db, row["device_id"]):
            _fail("enrollment_revoked", "enrollment was revoked")
        if row["phase"] == "expired" or (row["phase"] == "created" and now >= row["expires_at"]):
            _fail("enrollment_expired", "candidate expired before activation")
        if row["phase"] in {"activated", "finalized"}:
            active = db.execute("SELECT token_sha256,revoked_at FROM devices WHERE device_id=?", (row["device_id"],)).fetchone()
            if not active or active["revoked_at"] is not None or active["token_sha256"] != row["candidate_sha256"]:
                _fail("enrollment_superseded", "enrollment is no longer the active credential")
        return row

    def enrollment_status(self, enrollment_id: str, candidate_token: str, now: int | None = None) -> dict:
        with self._transaction() as db:
            return self._public_enrollment(db, self._enrollment(db, enrollment_id, candidate_token, self._now(now)))

    def activate_enrollment(self, enrollment_id: str, candidate_token: str, now: int | None = None) -> dict:
        now = self._now(now)
        with self._transaction() as db:
            row = self._enrollment(db, enrollment_id, candidate_token, now)
            if row["phase"] == "created":
                db.execute("INSERT INTO devices VALUES(?,?,?,?,NULL) ON CONFLICT(device_id) DO UPDATE SET "
                           "token_sha256=excluded.token_sha256,last_seen_at=excluded.last_seen_at,revoked_at=NULL",
                           (row["device_id"], row["candidate_sha256"], now, now))
                db.execute("UPDATE device_enrollments SET phase='activated',activated_at=? "
                           "WHERE enrollment_id=?", (now, enrollment_id))
            row = db.execute("SELECT * FROM device_enrollments WHERE enrollment_id=?", (enrollment_id,)).fetchone()
            return self._public_enrollment(db, row)

    def finalize_enrollment(self, enrollment_id: str, candidate_token: str, now: int | None = None) -> dict:
        now = self._now(now)
        with self._transaction() as db:
            row = self._enrollment(db, enrollment_id, candidate_token, now)
            if row["phase"] == "created":
                _fail("enrollment_not_activated", "activate the candidate before finalizing")
            if row["phase"] == "activated":
                db.execute("UPDATE device_enrollments SET phase='finalized',finalized_at=? "
                           "WHERE enrollment_id=?", (now, enrollment_id))
            row = db.execute("SELECT * FROM device_enrollments WHERE enrollment_id=?", (enrollment_id,)).fetchone()
            return self._public_enrollment(db, row)

    def _authenticated(self, db, device_id, token, now):
        if not isinstance(device_id, str) or not DEVICE_ID.fullmatch(device_id) or not isinstance(token, str) or not TOKEN.fullmatch(token):
            return False
        row = db.execute("SELECT token_sha256 FROM devices WHERE device_id=? AND revoked_at IS NULL", (device_id,)).fetchone()
        good = bool(row and hmac.compare_digest(row["token_sha256"], _hash(token)))
        if good:
            db.execute("UPDATE devices SET last_seen_at=? WHERE device_id=?", (now, device_id))
        return good

    def device_token_valid(self, device_id: str, token: str, now: int | None = None) -> bool:
        with self._transaction() as db:
            return self._authenticated(db, device_id, token, self._now(now))

    def device_info(self, device_id, token, now=None):
        with self._transaction() as db:
            if not self._authenticated(db, device_id, token, self._now(now)):
                _fail("unauthorized", "valid device credential required")
            return {"device_id": device_id, "server_instance_uuid": self.server_instance_uuid,
                    "protocol_version": PROTOCOL_VERSION}


    def list_devices(self, limit: int = 50, offset: int = 0) -> list[dict]:
        """Administrator-only paginated view, including candidates not yet activated."""
        if type(limit) is not int or not 1 <= limit <= 100 or type(offset) is not int or not 0 <= offset <= 1_000_000:
            raise ValueError("device pagination requires limit 1..100 and offset 0..1000000")
        with self._connect() as db:
            rows = db.execute("""
                SELECT ids.device_id, COALESCE(d.created_at,e.created_at) AS created_at,
                       d.last_seen_at, d.revoked_at, e.phase AS enrollment_phase,
                       e.expires_at AS enrollment_expires_at
                FROM (SELECT device_id FROM devices UNION SELECT device_id FROM device_enrollments) ids
                LEFT JOIN devices d ON ids.device_id=d.device_id
                LEFT JOIN device_enrollments e ON e.rowid=(
                    SELECT rowid FROM device_enrollments newest
                    WHERE newest.device_id=ids.device_id ORDER BY rowid DESC LIMIT 1)
                ORDER BY ids.device_id LIMIT ? OFFSET ?
            """, (limit, offset)).fetchall()
            result = []
            for row in rows:
                value = dict(row)
                if value["enrollment_phase"] == "created" and value["enrollment_expires_at"] <= self._now(None):
                    value["enrollment_phase"] = "expired"
                if value["enrollment_phase"] != "created":
                    value["enrollment_expires_at"] = None
                result.append(value)
            return result

    def _invalidate(self, db, device_id):
        self._epoch(db, device_id)
        db.execute("UPDATE device_identity_epochs SET epoch=epoch+1 WHERE device_id=?", (device_id,))
        db.execute("UPDATE device_enrollments SET phase='revoked' WHERE device_id=?", (device_id,))

    def revoke_device(self, device_id: str, now: int | None = None) -> bool:
        _device(device_id)
        with self._transaction() as db:
            existing = db.execute("SELECT 1 FROM devices WHERE device_id=? AND revoked_at IS NULL", (device_id,)).fetchone()
            pending = db.execute("SELECT 1 FROM device_enrollments WHERE device_id=? AND phase IN ('created','activated')",
                                 (device_id,)).fetchone()
            self._invalidate(db, device_id)
            db.execute("UPDATE devices SET revoked_at=? WHERE device_id=?", (self._now(now), device_id))
            return bool(existing or pending)
