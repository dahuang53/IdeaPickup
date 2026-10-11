"""One request per Unix connection, bounded messages and deadline."""
from __future__ import annotations
import json
import os
import secrets
import socket
import stat
import struct
import sys

from .core import BrokerError
from .protocol import MAX_FRAME, VERSION


class BrokerClient:
    def __init__(self, socket_path="/run/voice-memo/config.sock", *, timeout=10.0):
        self.socket_path = os.fspath(socket_path)
        self.timeout = timeout

    def call(self, method, **params):
        # Relay must never hand secrets to a user-controlled look-alike broker.
        if sys.platform != "linux" or not hasattr(socket, "SO_PEERCRED"):
            raise BrokerError("unsupported_platform")
        info = os.lstat(self.socket_path)
        if not stat.S_ISSOCK(info.st_mode) or info.st_uid != 0 or stat.S_IMODE(info.st_mode) != 0o660:
            raise BrokerError("unsafe_socket")
        identifier = secrets.token_hex(16)
        data = json.dumps({"version": VERSION, "id": identifier, "method": method, "params": params}, separators=(",", ":")).encode("utf-8") + b"\n"
        if len(data) > MAX_FRAME:
            raise BrokerError("request_too_large")
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.settimeout(self.timeout)
            connection.connect(self.socket_path)
            _pid, uid, _gid = struct.unpack("3i", connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize("3i")))
            if uid != 0:
                raise BrokerError("unauthorized_peer")
            connection.sendall(data)
            received = bytearray()
            while b"\n" not in received:
                part = connection.recv(min(8192, MAX_FRAME + 1 - len(received)))
                if not part:
                    raise BrokerError("incomplete_response")
                received.extend(part)
                if len(received) > MAX_FRAME:
                    raise BrokerError("response_too_large")
        try:
            response = json.loads(received)
        except (ValueError, UnicodeError, RecursionError):
            raise BrokerError("invalid_response") from None
        if not isinstance(response, dict) or response.get("version") != VERSION or response.get("id") != identifier or type(response.get("ok")) is not bool:
            raise BrokerError("invalid_response")
        if not response["ok"]:
            error = response.get("error", {})
            code = error.get("code") if isinstance(error, dict) else None
            if not isinstance(code, str) or not code or len(code) > 80 or not all(c.islower() or c == "_" for c in code):
                code = "broker_error"
            raise BrokerError(code)
        return response.get("result")

    def view(self):
        return self.call("view")

    def validate(self, patch):
        return self.call("validate", patch=patch)




    def save_activate(self, expected_revision, patch):
        return self.call("save_activate", expected_revision=expected_revision, patch=patch)

    def pin_active(self, task_id):
        return self.call("pin_active", task_id=task_id)

    def pin_existing(self, task_id, revision):
        return self.call("pin_existing", task_id=task_id, revision=revision)

    def read_snapshot(self, revision, task_id):
        return self.call("read_snapshot", revision=revision, task_id=task_id)

    def release(self, task_id):
        return self.call("release", task_id=task_id)

    def begin_maintenance(self):
        return self.call("begin_maintenance")

    def reconcile(self, generation, references):
        return self.call("reconcile", generation=generation, references=references)

    def end_maintenance(self, generation):
        return self.call("end_maintenance", generation=generation)

    def collect(self):
        return self.call("collect")
