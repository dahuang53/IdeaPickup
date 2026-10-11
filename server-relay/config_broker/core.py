"""Small filesystem state machine for the root-only configuration broker.

The relay may retrieve secrets only with a durable task pin. Management views
never include them. Paths and environment variable names are not RPC inputs.
"""
from __future__ import annotations

import contextlib
import fcntl
import hashlib
import ipaddress
import json
import os
import re
import secrets
import stat
import threading
import unicodedata
from urllib.parse import urlsplit, urlunsplit


class BrokerError(Exception):
    def __init__(self, code: str):
        self.code = code
        super().__init__(code)


DEFAULTS = {
    "qwen_url": "https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation",
    "qwen_model": "qwen-audio-3.0-asr-flash",
    "qwen_api_key": "",
    "siyuan_url": "https://ld246.com/apis/siyuan/inbox/addCloudShorthand",
    "siyuan_token": "",
}
SECRET_FIELDS = frozenset(("qwen_api_key", "siyuan_token"))
REVISION = re.compile(r"[0-9a-f]{32}\Z")
TASK = re.compile(r"[A-Za-z0-9][A-Za-z0-9:_.-]{0,127}\Z")
MAX_PINS = 512
MAX_SNAPSHOTS = MAX_PINS + 2
MAX_FILE = 131072
STATE_KEYS = {"version", "active_revision", "snapshots", "pins", "maintenance", "generation", "reconciled"}


def _require(condition, code="invalid_request"):
    if not condition:
        raise BrokerError(code)


def root_ownership(info, mode):
    """Production policy: actual UID 0 ownership, exact mode, one hard link."""
    _require(info.st_uid == 0 and stat.S_IMODE(info.st_mode) == mode, "unsafe_permissions")
    if stat.S_ISREG(info.st_mode):
        _require(info.st_nlink == 1, "unsafe_permissions")


def _revision(value):
    _require(isinstance(value, str) and REVISION.fullmatch(value), "invalid_revision")
    return value


def _task(value):
    _require(isinstance(value, str) and TASK.fullmatch(value), "invalid_task")
    return value


def _string(value, limit):
    _require(isinstance(value, str), "invalid_config")
    try:
        encoded = value.encode("utf-8")
    except UnicodeError:
        raise BrokerError("invalid_config") from None
    _require(len(encoded) <= limit and all(unicodedata.category(c) != "Cc" for c in value), "invalid_config")
    return value


def _url(value, limit=512, allow_empty=False, public=False):
    _string(value, limit)
    if value == "" and allow_empty:
        return value
    try:
        parsed = urlsplit(value)
        host = parsed.hostname
        port = parsed.port
        _require(parsed.scheme == "https" and host and not parsed.username and not parsed.password,
                 "invalid_url")
        _require(not parsed.query and not parsed.fragment and "\\" not in value and not any(c.isspace() for c in value), "invalid_url")
        _require("@" not in parsed.netloc and "%" not in parsed.netloc and parsed.netloc.isascii(), "invalid_url")
        _require(port is None or 1 <= port <= 65535, "invalid_url")
        try:
            address = ipaddress.ip_address(host)
        except ValueError:
            labels = host.rstrip(".").split(".")
            _require((public or len(labels) >= 2) and all(re.fullmatch(r"[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?", label) for label in labels), "invalid_url")
            _require(public or (host.lower().rstrip(".") not in {"metadata.google.internal", "metadata.azure.internal"} and not host.lower().endswith((".localhost", ".local", ".internal"))), "invalid_url")
        else:
            _require(public or address.is_global, "invalid_url")
        path = parsed.path.rstrip("/")
        # A proxy path must not carry encoded control characters or dot traversal.
        from urllib.parse import unquote
        decoded = unquote(path)
        _require(not any(ord(c) < 32 or ord(c) == 127 for c in decoded) and "\\" not in decoded and all(p not in {".", ".."} for p in decoded.split("/")), "invalid_url")
        return urlunsplit(("https", parsed.netloc.lower(), path, "", ""))
    except (ValueError, UnicodeError):
        raise BrokerError("invalid_url") from None


def validate_config(config):
    _require(isinstance(config, dict) and set(config) == set(DEFAULTS), "invalid_config")
    result = dict(config)
    for name in ("qwen_url", "siyuan_url"):
        result[name] = _url(result[name])
        _require(result[name] in {DEFAULTS[name], DEFAULTS[name].replace(".com/", ".com:443/")}, "unsupported_endpoint")
    _string(result["qwen_model"], 80)
    _require(result["qwen_model"] == DEFAULTS["qwen_model"], "unsupported_model")
    for name in SECRET_FIELDS:
        _string(result[name], 4096)
    return result


def apply_patch(base, patch):
    _require(isinstance(patch, dict) and set(patch) <= set(DEFAULTS), "unknown_field")
    result = dict(base)
    for name, value in patch.items():
        if name in SECRET_FIELDS:
            _require(isinstance(value, dict), "invalid_secret_operation")
            if value == {"op": "clear"}:
                result[name] = ""
            elif set(value) == {"op", "value"} and value["op"] == "replace":
                _require(isinstance(value["value"], str) and bool(value["value"]), "invalid_secret_operation")
                result[name] = value["value"]
            else:
                raise BrokerError("invalid_secret_operation")
        else:
            result[name] = value
    result = validate_config(result)
    return result


def redact(config):
    return {**{key: value for key, value in config.items() if key not in SECRET_FIELDS},
            **{key + "_configured": bool(config[key]) for key in SECRET_FIELDS}}


def _encode(config):
    # This format is intentionally parsed as data, never sourced into a shell.
    return "".join(name.upper() + "=" + json.dumps(config[name], ensure_ascii=True) + "\n" for name in sorted(DEFAULTS)).encode("ascii")


def _decode(data):
    try:
        result = {}
        for line in data.decode("ascii").splitlines():
            name, value = line.split("=", 1)
            key = name.lower()
            _require(name == key.upper() and key in DEFAULTS and key not in result, "corrupt_snapshot")
            result[key] = json.loads(value)
        return validate_config(result)
    except (ValueError, UnicodeError, BrokerError):
        raise BrokerError("corrupt_snapshot") from None


class ConfigBroker:
    """All mutations are serialized using both a thread lock and flock.

    The optional ownership_checker exists for explicit isolated core tests on a
    non-root host. The production server never accepts or supplies an override.
    fault_hook is a deterministic test hook; production never supplies one.
    """
    def __init__(self, directory, *, ownership_checker=None, fault_hook=None):
        self._policy = ownership_checker or root_ownership
        self._fault = fault_hook or (lambda stage: None)
        self._thread_lock = threading.RLock()
        self._dir = os.open(os.fspath(directory), os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        self._lockfd = None
        self._closed = False
        try:
            self._policy(os.fstat(self._dir), 0o700)
            self._lockfd = os.open("store.lock", os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600, dir_fd=self._dir)
            self._check_file(self._lockfd)
            with self._locked():
                try:
                    os.stat("state.json", dir_fd=self._dir, follow_symlinks=False)
                except FileNotFoundError:
                    # Missing manifest with any snapshots is never fresh setup.
                    _require(not any(n.startswith("revision-") for n in os.listdir(self._dir)), "missing_manifest")
                    state = {"version": 2, "active_revision": None,
                             "snapshots": {}, "pins": {}, "maintenance": True,
                             "generation": secrets.token_hex(16), "reconciled": False}
                else:
                    # Missing referenced files must never be treated as setup.
                    state = self._load()
                state.update(maintenance=True, generation=secrets.token_hex(16), reconciled=False)
                self._write_state(state)
                self._remove_orphans(state)
        except BaseException:
            self.close()
            raise

    def close(self):
        if not self._closed:
            self._closed = True
            if self._lockfd is not None:
                os.close(self._lockfd)
            os.close(self._dir)

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()

    @contextlib.contextmanager
    def _locked(self):
        with self._thread_lock:
            fcntl.flock(self._lockfd, fcntl.LOCK_EX)
            try:
                yield
            finally:
                fcntl.flock(self._lockfd, fcntl.LOCK_UN)

    def _check_file(self, fd):
        info = os.fstat(fd)
        _require(stat.S_ISREG(info.st_mode), "unsafe_file")
        self._policy(info, 0o600)
        _require(info.st_size <= MAX_FILE, "corrupt_state")

    def _read(self, name):
        fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW, dir_fd=self._dir)
        try:
            self._check_file(fd)
            data = b""
            while True:
                block = os.read(fd, min(8192, MAX_FILE + 1 - len(data)))
                if not block:
                    break
                data += block
                _require(len(data) <= MAX_FILE, "corrupt_state")
            return data
        finally:
            os.close(fd)

    def _atomic(self, name, data, immutable=False):
        temporary = ".tmp-" + secrets.token_hex(16)
        fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600, dir_fd=self._dir)
        try:
            self._check_file(fd)
            view = memoryview(data)
            while view:
                count = os.write(fd, view)
                _require(count > 0, "storage_failure")
                view = view[count:]
            os.fsync(fd)
            self._fault("file_fsync:" + name)
        finally:
            os.close(fd)
        try:
            if immutable:
                # The lock and root-only directory exclude concurrent creation.
                # A single rename also avoids a two-hard-link crash window.
                try:
                    os.stat(name, dir_fd=self._dir, follow_symlinks=False)
                except FileNotFoundError:
                    pass
                else:
                    raise BrokerError("revision_collision")
                os.rename(temporary, name, src_dir_fd=self._dir, dst_dir_fd=self._dir)
            else:
                os.replace(temporary, name, src_dir_fd=self._dir, dst_dir_fd=self._dir)
            self._fault("rename:" + name)
            os.fsync(self._dir)
            self._fault("directory_fsync:" + name)
        finally:
            with contextlib.suppress(FileNotFoundError):
                os.unlink(temporary, dir_fd=self._dir)

    def _write_state(self, state):
        data = json.dumps(state, sort_keys=True, separators=(",", ":")).encode()
        _require(len(data) <= MAX_FILE, "state_limit")
        self._atomic("state.json", data)

    def _snapshot(self, state, revision):
        _revision(revision)
        _require(revision in state["snapshots"], "revision_not_found")
        data = self._read("revision-" + revision + ".env")
        _require(hashlib.sha256(data).hexdigest() == state["snapshots"][revision], "corrupt_snapshot")
        return _decode(data)

    def _load(self):
        raw = self._read("state.json")
        try:
            state = json.loads(raw)
            _require(isinstance(state, dict) and set(state) == STATE_KEYS and type(state["version"]) is int and state["version"] == 2, "corrupt_state")
            _require(isinstance(state["snapshots"], dict) and len(state["snapshots"]) <= MAX_SNAPSHOTS, "corrupt_state")
            _require(isinstance(state["pins"], dict) and len(state["pins"]) <= MAX_PINS, "corrupt_state")
            _require(type(state["maintenance"]) is bool and type(state["reconciled"]) is bool, "corrupt_state")
            _revision(state["generation"])
            for revision, digest in state["snapshots"].items():
                _revision(revision)
                _require(isinstance(digest, str) and re.fullmatch(r"[0-9a-f]{64}", digest), "corrupt_state")
                self._snapshot(state, revision)
            for key in ("active_revision",):
                _require(state[key] is None or state[key] in state["snapshots"], "corrupt_state")
            for task, revision in state["pins"].items():
                _task(task)
                _require(revision in state["snapshots"], "corrupt_state")
            candidates = set(state["snapshots"]) - set(state["pins"].values()) - {state["active_revision"]}
            # Unreferenced old versions can exist after a crash, but are collected.
            _require(len(candidates) <= MAX_SNAPSHOTS, "corrupt_state")
            return state
        except (ValueError, TypeError, KeyError, BrokerError):
            raise BrokerError("corrupt_state") from None

    def _remove_orphans(self, state):
        changed = False
        for name in os.listdir(self._dir):
            if re.fullmatch(r"\.tmp-[0-9a-f]{32}", name) or (re.fullmatch(r"revision-[0-9a-f]{32}\.env", name) and name[9:-4] not in state["snapshots"]):
                # Never traverse a caller-controlled symlink, even for cleanup.
                fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW, dir_fd=self._dir)
                try:
                    self._check_file(fd)
                finally:
                    os.close(fd)
                os.unlink(name, dir_fd=self._dir)
                changed = True
        if changed:
            os.fsync(self._dir)

    def _gc(self, state):
        if state["maintenance"] and not state["reconciled"]:
            return []
        retained = set(state["pins"].values()) | {state["active_revision"]}
        removed = sorted(set(state["snapshots"]) - retained)
        if removed:
            for revision in removed:
                del state["snapshots"][revision]
            # Manifest first: a crash leaves only an unreferenced orphan.
            self._write_state(state)
            for revision in removed:
                os.unlink("revision-" + revision + ".env", dir_fd=self._dir)
            os.fsync(self._dir)
        return removed

    def _view(self, state):
        revision = state["active_revision"]
        config = self._snapshot(state, revision) if revision else dict(DEFAULTS)
        redacted = redact(config)
        return {"active_revision": revision, "config": redacted,
                "active_config": redacted if revision else None,
                "maintenance": state["maintenance"], "snapshot_count": len(state["snapshots"]),
                "pin_count": len(state["pins"]),
                "saved_at": os.stat("revision-" + revision + ".env", dir_fd=self._dir, follow_symlinks=False).st_mtime if revision else None}

    def view(self):
        with self._locked():
            return self._view(self._load())

    def validate(self, patch):
        with self._locked():
            state = self._load()
            base = self._snapshot(state, state["active_revision"]) if state["active_revision"] else DEFAULTS
            return {"config": redact(apply_patch(base, patch))}




    def save_activate(self, expected_revision, patch):
        """One durable manifest publication saves and activates together."""
        with self._locked():
            state = self._load()
            _require(not state["maintenance"], "maintenance")
            _require(expected_revision == state["active_revision"], "revision_conflict")
            base = self._snapshot(state, state["active_revision"]) if state["active_revision"] else DEFAULTS
            config = apply_patch(base, patch)
            self._gc(state)
            self._remove_orphans(state)
            if state["active_revision"] and config == base:
                return self._view(state)
            _require(len(state["snapshots"]) < MAX_SNAPSHOTS, "snapshot_limit")
            revision = secrets.token_hex(16)
            data = _encode(config)
            self._atomic("revision-" + revision + ".env", data, immutable=True)
            state["snapshots"][revision] = hashlib.sha256(data).hexdigest()
            state["active_revision"] = revision
            self._write_state(state)
            self._gc(state)
            return self._view(state)

    def _pin(self, state, task_id, revision):
        _task(task_id)
        if task_id in state["pins"]:
            _require(state["pins"][task_id] == revision, "pin_conflict")
            return {"task_id": task_id, "revision": revision}
        _require(not state["maintenance"], "maintenance")
        _require(len(state["pins"]) < MAX_PINS, "pin_limit")
        self._snapshot(state, revision)
        state["pins"][task_id] = revision
        self._write_state(state)
        return {"task_id": task_id, "revision": revision}

    def pin_active(self, task_id):
        with self._locked():
            state = self._load()
            _task(task_id)
            if task_id in state["pins"]:
                return {"task_id": task_id, "revision": state["pins"][task_id]}
            _require(state["active_revision"] is not None, "not_configured")
            return self._pin(state, task_id, state["active_revision"])

    def pin_existing(self, task_id, revision):
        with self._locked():
            return self._pin(self._load(), task_id, _revision(revision))

    def read_snapshot(self, revision, task_id):
        with self._locked():
            state = self._load()
            _require(state["pins"].get(_task(task_id)) == _revision(revision), "pin_required")
            return {"revision": revision, "config": self._snapshot(state, revision)}

    def release(self, task_id):
        with self._locked():
            state = self._load()
            if state["pins"].pop(_task(task_id), None) is not None:
                self._write_state(state)
            self._gc(state)
            # GC publishes the manifest before unlinking. A retry must also
            # remove files orphaned by an earlier interrupted release.
            self._remove_orphans(state)
            os.fsync(self._dir)
            return {"released": True}

    def begin_maintenance(self):
        with self._locked():
            state = self._load()
            state.update(maintenance=True, generation=secrets.token_hex(16), reconciled=False)
            self._write_state(state)
            return {"generation": state["generation"]}

    def reconcile(self, generation, references):
        with self._locked():
            state = self._load()
            _require(state["maintenance"] and generation == state["generation"], "maintenance_conflict")
            _require(isinstance(references, dict) and len(references) <= MAX_PINS, "invalid_references")
            for task, revision in references.items():
                _task(task)
                self._snapshot(state, _revision(revision))
                _require(task not in state["pins"] or state["pins"][task] == revision, "pin_conflict")
            state["pins"] = dict(references)
            state["reconciled"] = True
            self._write_state(state)
            self._gc(state)
            self._remove_orphans(state)
            os.fsync(self._dir)
            return {"pin_count": len(state["pins"])}

    def end_maintenance(self, generation):
        with self._locked():
            state = self._load()
            _require(state["maintenance"] and generation == state["generation"] and state["reconciled"], "maintenance_conflict")
            state["maintenance"] = False
            self._write_state(state)
            return self._view(state)

    def collect(self):
        with self._locked():
            state = self._load()
            removed = self._gc(state)
            self._remove_orphans(state)
            return {"removed_revisions": removed}
