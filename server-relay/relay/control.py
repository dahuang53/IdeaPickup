"""Local fixed-operation control socket, isolated from the public HTTP surface."""
from __future__ import annotations

import contextlib
import json
import os
import re
from pathlib import Path
import socket
import stat
import struct
import sys
import threading


COMMANDS = frozenset(("status", "drain", "drain-status", "resume", "close"))
MAX_FRAME = 16384


class ControlError(Exception):
    pass


def peer_uid(connection):
    if sys.platform != "linux" or not hasattr(socket, "SO_PEERCRED"):
        raise ControlError("linux_peer_credentials_required")
    return struct.unpack("3i", connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))[1]


def receive(connection):
    data = bytearray()
    while b"\n" not in data:
        part = connection.recv(min(4096, MAX_FRAME + 1 - len(data)))
        if not part:
            raise ControlError("incomplete_request")
        data.extend(part)
        if len(data) > MAX_FRAME:
            raise ControlError("request_too_large")
    if data.count(b"\n") != 1 or not data.endswith(b"\n"):
        raise ControlError("invalid_frame")
    def unique(pairs):
        output = {}
        for key, value in pairs:
            if key in output:
                raise ControlError("duplicate_key")
            output[key] = value
        return output
    try:
        return json.loads(data, object_pairs_hook=unique)
    except (ValueError, RecursionError, UnicodeError):
        raise ControlError("invalid_json") from None


def dispatch(runtime, request, metadata):
    if (not isinstance(request, dict) or not {"version", "command"} <= set(request)
            or not set(request) <= {"version", "command", "params"}
            or type(request["version"]) is not int or request["version"] != 1
            or not isinstance(request["command"], str) or request["command"] not in COMMANDS):
        raise ControlError("invalid_command")
    command = request["command"]
    params = request.get("params", {})
    if not isinstance(params, dict):
        raise ControlError("invalid_parameters")
    if params:
        raise ControlError("invalid_parameters")
    if command == "status":
        return {**metadata, **runtime.readiness(), **runtime.drain_status(), "ok": True}
    if command == "drain":
        runtime.drain()
        return {"ok": True, "accepting_new": False, **runtime.drain_status()}
    if command == "drain-status":
        return {"ok": True, **runtime.drain_status()}
    if command == "resume":
        runtime.resume()
        return {"ok": True, "accepting_new": True}
    state = runtime.drain_status()
    if not state.get("draining") or not state.get("safe_to_stop"):
        raise ControlError("not_drained")
    runtime.close(timeout=30)
    return {"ok": True, "closed": True}


class ControlServer:
    def __init__(self, path: Path, runtime, metadata, *, get_peer_uid=peer_uid):
        self.path, self.runtime, self.metadata = Path(path), runtime, metadata
        self.get_peer_uid = get_peer_uid
        self.stop = threading.Event()
        self.path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        info = self.path.parent.lstat()
        if (not stat.S_ISDIR(info.st_mode) or info.st_uid != os.geteuid()
                or stat.S_IMODE(info.st_mode) != 0o700):
            raise ControlError("unsafe_control_directory")
        # Runtime's exclusive instance lock must already be held. An unexpected
        # regular file or symlink at this path is never removed as stale state.
        if self.path.exists() or self.path.is_symlink():
            old = self.path.lstat()
            if not stat.S_ISSOCK(old.st_mode) or old.st_uid != os.geteuid():
                raise ControlError("unsafe_control_socket")
            self.path.unlink()
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.listener.bind(str(self.path))
        os.chmod(self.path, 0o600)
        self.listener.listen(8)
        self.listener.settimeout(0.5)
        self.inode = self.path.lstat().st_ino
        self.thread = threading.Thread(target=self._serve, name="relay-local-control", daemon=True)
        self.thread.start()

    def _serve(self):
        while not self.stop.is_set():
            try:
                connection, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                if self.stop.is_set():
                    break
                continue
            with connection:
                connection.settimeout(35)
                try:
                    if self.get_peer_uid(connection) != os.geteuid():
                        raise ControlError("unauthorized_peer")
                    result = dispatch(self.runtime, receive(connection), self.metadata)
                except ControlError as error:
                    result = {"ok": False, "error": str(error)}
                except Exception:
                    # Runtime/provider exception text may contain private data.
                    result = {"ok": False, "error": "operation_failed"}
                with contextlib.suppress(OSError):
                    connection.sendall(json.dumps(result, separators=(",", ":")).encode() + b"\n")

    def close(self):
        self.stop.set()
        self.listener.close()
        self.thread.join(36)
        if self.thread.is_alive():
            raise ControlError("control_operation_still_running")
        with contextlib.suppress(FileNotFoundError):
            info = self.path.lstat()
            if stat.S_ISSOCK(info.st_mode) and info.st_ino == self.inode and info.st_uid == os.geteuid():
                self.path.unlink()


def call(path: Path, command: str, *, params=None, get_peer_uid=peer_uid):
    if command not in COMMANDS:
        raise ControlError("invalid_command")
    path = Path(path)
    info = path.lstat()
    parent = path.parent.lstat()
    if (not stat.S_ISSOCK(info.st_mode) or info.st_uid != os.geteuid() or stat.S_IMODE(info.st_mode) != 0o600
            or not stat.S_ISDIR(parent.st_mode) or parent.st_uid != os.geteuid() or stat.S_IMODE(parent.st_mode) != 0o700):
        raise ControlError("unsafe_control_socket")
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(35)
        connection.connect(str(path))
        if get_peer_uid(connection) != os.geteuid():
            raise ControlError("unauthorized_peer")
        request = {"version": 1, "command": command}
        if params is not None:
            request["params"] = params
        connection.sendall(json.dumps(request).encode() + b"\n")
        response = receive(connection)
    if not isinstance(response, dict) or type(response.get("ok")) is not bool:
        raise ControlError("invalid_response")
    return response
