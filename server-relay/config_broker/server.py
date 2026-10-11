"""Linux-only authenticated Unix-socket service. No TCP, shell or network API."""
from __future__ import annotations
import argparse
import contextlib
import fcntl
import json
import os
import signal
import socket
import stat
import struct
import sys

from .core import BrokerError, ConfigBroker, root_ownership
from .protocol import MAX_FRAME, dispatch


def peer_uid(connection):
    if sys.platform != "linux" or not hasattr(socket, "SO_PEERCRED"):
        raise BrokerError("unsupported_platform")
    _pid, uid, _gid = struct.unpack("3i", connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize("3i")))
    return uid


def receive_request(connection):
    data = bytearray()
    while b"\n" not in data:
        block = connection.recv(min(8192, MAX_FRAME + 1 - len(data)))
        if not block:
            raise BrokerError("incomplete_request")
        data.extend(block)
        if len(data) > MAX_FRAME:
            raise BrokerError("request_too_large")
    # One frame per connection; reject trailing junk in the received frame.
    if data.count(b"\n") != 1 or data[-1:] != b"\n":
        raise BrokerError("invalid_request")
    try:
        def unique_keys(pairs):
            result = {}
            for key, value in pairs:
                if key in result:
                    raise BrokerError("duplicate_key")
                result[key] = value
            return result
        return json.loads(data, object_pairs_hook=unique_keys)
    except (ValueError, UnicodeError, RecursionError):
        raise BrokerError("invalid_request") from None


def handle_connection(connection, broker, allowed_uid):
    connection.settimeout(5.0)
    try:
        if peer_uid(connection) != allowed_uid:
            raise BrokerError("unauthorized_peer")
        response = dispatch(broker, receive_request(connection))
    except BrokerError as exc:
        response = {"version": 1, "id": None, "ok": False, "error": {"code": exc.code}}
    except (OSError, ValueError):
        response = {"version": 1, "id": None, "ok": False, "error": {"code": "transport_error"}}
    with contextlib.suppress(OSError):
        connection.sendall(json.dumps(response, separators=(",", ":")).encode("utf-8") + b"\n")


def serve(secret_directory, socket_directory, allowed_uid, socket_gid):
    if sys.platform != "linux" or not hasattr(socket, "SO_PEERCRED") or os.geteuid() != 0:
        raise BrokerError("unsupported_platform")
    if allowed_uid <= 0 or socket_gid <= 0:
        raise BrokerError("invalid_identity")
    # Paths are deployment arguments, never RPC parameters. Both mount roots
    # must already exist with their declared ownership; do not repair by chmod.
    socket_directory = os.path.abspath(socket_directory)
    socket_fd = os.open(socket_directory, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        info = os.fstat(socket_fd)
        if info.st_uid != 0 or info.st_gid != socket_gid or stat.S_IMODE(info.st_mode) != 0o750:
            raise BrokerError("unsafe_socket_directory")
        lock_fd = os.open("listener.lock", os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600, dir_fd=socket_fd)
        try:
            root_ownership(os.fstat(lock_fd), 0o600)
            fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with ConfigBroker(secret_directory) as broker:
                try:
                    stale = os.stat("config.sock", dir_fd=socket_fd, follow_symlinks=False)
                except FileNotFoundError:
                    pass
                else:
                    if not stat.S_ISSOCK(stale.st_mode) or stale.st_uid != 0:
                        raise BrokerError("unsafe_socket")
                    os.unlink("config.sock", dir_fd=socket_fd)
                # Bind through the already verified directory FD. This avoids
                # ancestor substitution and AF_UNIX path limits in CI roots;
                # clients use the regular mounted directory's config.sock.
                path = f"/proc/self/fd/{socket_fd}/config.sock"
                stopping = False
                def stop(_signum, _frame):
                    nonlocal stopping
                    stopping = True
                old_term = signal.signal(signal.SIGTERM, stop)
                old_int = signal.signal(signal.SIGINT, stop)
                try:
                    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
                        listener.bind(path)
                        os.chown("config.sock", 0, socket_gid, dir_fd=socket_fd, follow_symlinks=False)
                        os.chmod("config.sock", 0o660, dir_fd=socket_fd, follow_symlinks=False)
                        listener.listen(16)
                        listener.settimeout(1.0)
                        while not stopping:
                            try:
                                connection, _address = listener.accept()
                            except socket.timeout:
                                continue
                            with connection:
                                handle_connection(connection, broker, allowed_uid)
                finally:
                    signal.signal(signal.SIGTERM, old_term)
                    signal.signal(signal.SIGINT, old_int)
                    with contextlib.suppress(FileNotFoundError):
                        os.unlink("config.sock", dir_fd=socket_fd)
        finally:
            os.close(lock_fd)
    finally:
        os.close(socket_fd)


def main():
    parser = argparse.ArgumentParser(description="Restricted local configuration broker")
    parser.add_argument("--secrets-dir", default="/var/lib/voice-memo-secrets")
    parser.add_argument("--socket-dir", default="/run/voice-memo")
    parser.add_argument("--relay-uid", required=True, type=int)
    parser.add_argument("--socket-gid", required=True, type=int)
    args = parser.parse_args()
    try:
        serve(args.secrets_dir, args.socket_dir, args.relay_uid, args.socket_gid)
    except (OSError, BrokerError):
        # No exception text or filesystem/environment dump in container logs.
        print("config broker refused startup or storage operation", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
