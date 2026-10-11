"""Single-owner authentication. No device or provider credentials are accepted."""
from __future__ import annotations

import argparse
import hashlib
import hmac
import os
from pathlib import Path
import secrets
import sqlite3
import time

from argon2 import PasswordHasher
from argon2.exceptions import InvalidHashError, VerificationError


class AdminError(Exception):
    def __init__(self, status: int, code: str):
        self.status, self.code = status, code
        super().__init__(code)


class _Connection(sqlite3.Connection):
    def __exit__(self, *args):
        try:
            return super().__exit__(*args)
        finally:
            self.close()


class AdminStore:
    IDLE_SECONDS = 30 * 60
    MAX_SECONDS = 8 * 60 * 60
    BOOTSTRAP_SECONDS = 15 * 60

    def __init__(self, path: Path, *, clock=time.time, hasher=None):
        self.path, self.clock = Path(path), clock
        self.hasher = hasher or PasswordHasher()
        self.path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        with self.connect() as db:
            db.executescript("""
                CREATE TABLE IF NOT EXISTS admin_account (
                    singleton INTEGER PRIMARY KEY CHECK(singleton=1),
                    password_hash TEXT, bootstrap_hash TEXT,
                    bootstrap_until INTEGER NOT NULL DEFAULT 0
                );
                INSERT OR IGNORE INTO admin_account(singleton) VALUES(1);
                CREATE TABLE IF NOT EXISTS admin_sessions (
                    token_hash TEXT PRIMARY KEY, created_at INTEGER NOT NULL,
                    last_seen INTEGER NOT NULL
                );
                CREATE TABLE IF NOT EXISTS admin_rate_limits (
                    bucket TEXT PRIMARY KEY, since INTEGER NOT NULL,
                    attempts INTEGER NOT NULL
                );
            """)

    def connect(self):
        db = sqlite3.connect(self.path, timeout=15, factory=_Connection)
        db.row_factory = sqlite3.Row
        db.execute("PRAGMA synchronous=FULL")
        return db

    @staticmethod
    def digest(value):
        return hashlib.sha256(value.encode("utf-8")).hexdigest()

    @staticmethod
    def csrf(token):
        return hmac.new(token.encode("ascii"), b"voicememo-admin-csrf-v1", hashlib.sha256).hexdigest()

    @staticmethod
    def check_password(password):
        if not isinstance(password, str) or not 12 <= len(password) <= 128 or "\x00" in password:
            raise AdminError(400, "password_length_12_to_128")
        try:
            password.encode("utf-8")
        except UnicodeError:
            raise AdminError(400, "invalid_password_encoding") from None

    def initialized(self):
        with self.connect() as db:
            return bool(db.execute("SELECT password_hash FROM admin_account WHERE singleton=1").fetchone()[0])

    def issue_bootstrap(self, *, reset=False):
        """Deployment-host operation only. Never expose this as an HTTP endpoint."""
        code = secrets.token_urlsafe(32)
        now = int(self.clock())
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute("SELECT password_hash FROM admin_account WHERE singleton=1").fetchone()
            if row[0] and not reset:
                raise AdminError(409, "already_initialized")
            if reset:
                db.execute("DELETE FROM admin_sessions")
            db.execute("UPDATE admin_account SET password_hash=NULL, bootstrap_hash=?, bootstrap_until=? "
                       "WHERE singleton=1", (self.digest(code), now + self.BOOTSTRAP_SECONDS))
        return code

    def _attempt(self, peer):
        # Store bounded hashes, never raw IP/user supplied request data. Persist
        # the charge in its own transaction even if later authentication fails.
        now = int(self.clock())
        peer = str(peer)[:256]
        buckets = (("peer:" + self.digest(peer), 5), ("global", 60))
        limited = False
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            db.execute("DELETE FROM admin_rate_limits WHERE since <= ?", (now - 300,))
            for bucket, maximum in buckets:
                row = db.execute("SELECT attempts FROM admin_rate_limits WHERE bucket=?", (bucket,)).fetchone()
                if row and row[0] >= maximum:
                    limited = True
                else:
                    db.execute("INSERT INTO admin_rate_limits VALUES(?,?,1) ON CONFLICT(bucket) "
                               "DO UPDATE SET attempts=attempts+1", (bucket, now))
        if limited:
            raise AdminError(429, "authentication_rate_limited")

    def initialize(self, code, password, *, peer="local"):
        self._attempt(peer)
        self.check_password(password)
        if not isinstance(code, str) or len(code) > 128:
            raise AdminError(401, "initialization_denied")
        encoded = self.hasher.hash(password)
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute("SELECT * FROM admin_account WHERE singleton=1").fetchone()
            valid = (not row["password_hash"] and row["bootstrap_hash"]
                     and int(self.clock()) < row["bootstrap_until"]
                     and hmac.compare_digest(row["bootstrap_hash"], self.digest(code)))
            if not valid:
                raise AdminError(401, "initialization_denied")
            db.execute("UPDATE admin_account SET password_hash=?, bootstrap_hash=NULL, "
                       "bootstrap_until=0 WHERE singleton=1", (encoded,))

    def login(self, password, *, peer="local"):
        self._attempt(peer)
        if not isinstance(password, str) or len(password) > 128:
            raise AdminError(401, "login_denied")
        # Hash verification is outside a database write transaction. Compare the
        # exact credential version again before minting a session after a reset.
        with self.connect() as db:
            encoded = db.execute("SELECT password_hash FROM admin_account WHERE singleton=1").fetchone()[0]
        if not encoded:
            raise AdminError(401, "login_denied")
        try:
            self.hasher.verify(encoded, password)
        except (VerificationError, InvalidHashError, UnicodeError):
            raise AdminError(401, "login_denied") from None
        token = secrets.token_urlsafe(32)
        now = int(self.clock())
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            current = db.execute("SELECT password_hash FROM admin_account WHERE singleton=1").fetchone()[0]
            if current != encoded:
                raise AdminError(401, "login_denied")
            db.execute("DELETE FROM admin_sessions WHERE last_seen<=? OR created_at<=?",
                       (now - self.IDLE_SECONDS, now - self.MAX_SECONDS))
            # Browser sessions are bounded; oldest sessions are revoked first.
            db.execute("DELETE FROM admin_sessions WHERE token_hash IN "
                       "(SELECT token_hash FROM admin_sessions ORDER BY created_at DESC LIMIT -1 OFFSET 7)")
            db.execute("INSERT INTO admin_sessions VALUES(?,?,?)", (self.digest(token), now, now))
        return {"token": token, "csrf": self.csrf(token), "max_age": self.MAX_SECONDS}

    def authenticate(self, token, *, csrf=None, require_csrf=False):
        if not isinstance(token, str) or not 20 <= len(token) <= 128 or not token.isascii():
            raise AdminError(401, "login_required")
        if require_csrf and (not isinstance(csrf, str) or not csrf.isascii()
                             or not hmac.compare_digest(self.csrf(token), csrf)):
            raise AdminError(403, "csrf_denied")
        now = int(self.clock())
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute("SELECT * FROM admin_sessions WHERE token_hash=?", (self.digest(token),)).fetchone()
            if not row or now - row["last_seen"] >= self.IDLE_SECONDS or now - row["created_at"] >= self.MAX_SECONDS:
                raise AdminError(401, "login_required")
            db.execute("UPDATE admin_sessions SET last_seen=? WHERE token_hash=?", (now, self.digest(token)))
        return {"authenticated": True, "csrf": self.csrf(token)}

    def logout(self, token):
        if isinstance(token, str):
            with self.connect() as db:
                db.execute("DELETE FROM admin_sessions WHERE token_hash=?", (self.digest(token),))


def main():
    parser = argparse.ArgumentParser(description="Deployment-host admin recovery; no network operations")
    parser.add_argument("command", choices=("bootstrap", "reset"))
    args = parser.parse_args()
    database = os.environ.get("VOICE_MEMO_DB")
    if not database:
        parser.error("VOICE_MEMO_DB must identify the deployment data volume")
    store = AdminStore(Path(database))
    print(store.issue_bootstrap(reset=args.command == "reset"))


if __name__ == "__main__":
    main()
