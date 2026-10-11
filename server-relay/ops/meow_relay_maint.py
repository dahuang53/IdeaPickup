#!/usr/bin/env python3
"""Fixed legacy operations only. Code publication is deliberately not delegated."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import pathlib
import pwd
import re
import shutil
import sqlite3
import stat
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request


SERVICE = "meow-voice-memo-relay"
APP_DIR = pathlib.Path("/opt/meow-voice-memo-relay")
DB_PATH = pathlib.Path("/var/lib/meow-voice-memo-relay/state.sqlite3")
INCOMING = pathlib.Path("/home/meowmaint/incoming/relay-release.tar.gz")
BACKUPS = APP_DIR / "maintenance-backups"
HEALTH_URL = "http://127.0.0.1:8765/health"
ALLOWED_RELEASE_FILES = {
    "app.py",
    "stream_service.py",
    "test_app.py",
    "test_stream_service.py",
    "pyproject.toml",
}
REQUIRED_RELEASE_FILES = {
    "app.py",
    "stream_service.py",
    "test_app.py",
    "test_stream_service.py",
}
DEVICE_RE = re.compile(r"^[A-Za-z0-9_-]{1,64}$")
BACKUP_RE = re.compile(r"^\d{8}T\d{6}Z-[0-9a-f]{12}$")
TOKEN_RE = re.compile(r"(?i)(Bearer\s+)[A-Za-z0-9._~-]+")


class MaintenanceError(RuntimeError):
    pass


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def run(command: list[str], *, timeout: int = 60, capture: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        check=True,
        text=True,
        capture_output=capture,
        timeout=timeout,
        env={"PATH": "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", "LANG": "C.UTF-8"},
    )


def service_active() -> bool:
    result = subprocess.run(
        ["/usr/bin/systemctl", "is-active", "--quiet", SERVICE],
        check=False,
        env={"PATH": "/usr/sbin:/usr/bin:/sbin:/bin"},
    )
    return result.returncode == 0


def health_snapshot() -> dict[str, object]:
    try:
        with urllib.request.urlopen(HEALTH_URL, timeout=5) as response:
            body = json.load(response)
        allowed = ("status", "service", "model", "cardless_streams", "opus_available")
        return {key: body.get(key) for key in allowed if key in body}
    except Exception as error:  # status must remain useful while the service is down
        return {"status": "unavailable", "reason": type(error).__name__}


def wait_healthy(seconds: int = 20) -> dict[str, object]:
    deadline = time.monotonic() + seconds
    last: dict[str, object] = {}
    while time.monotonic() < deadline:
        last = health_snapshot()
        if service_active() and last.get("status") == "ok":
            return last
        time.sleep(1)
    raise MaintenanceError(f"service did not become healthy: {last}")


def status_payload() -> dict[str, object]:
    usage = shutil.disk_usage("/var/lib/meow-voice-memo-relay")
    files: dict[str, str | None] = {}
    for name in ("app.py", "stream_service.py"):
        path = APP_DIR / name
        files[name] = sha256(path) if path.is_file() else None
    return {
        "service_active": service_active(),
        "health": health_snapshot(),
        "code_sha256": files,
        "database_bytes": DB_PATH.stat().st_size if DB_PATH.is_file() else None,
        "disk_free_bytes": usage.free,
    }


def print_json(value: object) -> None:
    print(json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True))


def command_logs(lines: int) -> None:
    if not 1 <= lines <= 500:
        raise MaintenanceError("log line count must be between 1 and 500")
    result = run(
        ["/usr/bin/journalctl", "-u", SERVICE, "-n", str(lines), "--no-pager", "-o", "short-iso"],
        timeout=30,
    )
    print(TOKEN_RE.sub(r"\1<redacted>", result.stdout), end="")


def command_memo(device: str, recording: int) -> None:
    if not DEVICE_RE.fullmatch(device):
        raise MaintenanceError("invalid device id")
    if not 1 <= recording <= 0xFFFFFFFF:
        raise MaintenanceError("recording id is outside uint32")
    connection = sqlite3.connect(f"file:{DB_PATH}?mode=ro", uri=True, timeout=10)
    try:
        row = connection.execute(
            "select recording_id,duration_ms,status,error,updated_at,qwen_request_id,"
            "transcript_sha256 from memos where device_id=? and recording_id=?",
            (device, recording),
        ).fetchone()
    finally:
        connection.close()
    if row is None:
        raise MaintenanceError("recording not found")
    requests = [part for part in (row[5] or "").split(",") if part]
    print_json(
        {
            "recording_id": row[0],
            "duration_ms": row[1],
            "status": row[2],
            "error": row[3],
            "updated_at": row[4],
            "qwen_request_count": len(requests),
            "has_transcript_hash": bool(row[6]),
        }
    )


def validate_bundle(path: pathlib.Path, destination: pathlib.Path) -> str:
    info = path.lstat()
    owner = pwd.getpwnam("meowmaint")
    if not stat.S_ISREG(info.st_mode) or path.is_symlink() or info.st_uid != owner.pw_uid:
        raise MaintenanceError("release bundle must be a regular file owned by meowmaint")
    if info.st_mode & 0o077:
        raise MaintenanceError("release bundle must not be group/world accessible")
    if not 1 <= info.st_size <= 10 * 1024 * 1024:
        raise MaintenanceError("release bundle size is invalid")
    seen: set[str] = set()
    total = 0
    with tarfile.open(path, "r:gz") as archive:
        for member in archive.getmembers():
            if member.name not in ALLOWED_RELEASE_FILES or not member.isfile() or member.name in seen:
                raise MaintenanceError(f"unsafe or unexpected release member: {member.name}")
            if member.size < 0 or member.size > 5 * 1024 * 1024:
                raise MaintenanceError(f"release member has invalid size: {member.name}")
            total += member.size
            if total > 10 * 1024 * 1024:
                raise MaintenanceError("expanded release is too large")
            source = archive.extractfile(member)
            if source is None:
                raise MaintenanceError(f"cannot read release member: {member.name}")
            target = destination / member.name
            with target.open("xb") as output:
                shutil.copyfileobj(source, output)
            target.chmod(0o600)
            seen.add(member.name)
    missing = REQUIRED_RELEASE_FILES - seen
    if missing:
        raise MaintenanceError(f"release is missing: {', '.join(sorted(missing))}")
    return sha256(path)


def restart_and_check() -> dict[str, object]:
    run(["/usr/bin/systemctl", "restart", SERVICE], timeout=30)
    return wait_healthy()


def command_deploy() -> None:
    # Do not even extract, compile, import or inspect caller-controlled Python
    # through the privileged entry point. Publication uses the separate,
    # reviewed image workflow; an operator is not a release publisher.
    raise MaintenanceError(
        "legacy deployment disabled: use the separately authorized image release workflow"
    )


def command_list_backups() -> None:
    if not BACKUPS.is_dir():
        print_json([])
        return
    values = [path.name for path in BACKUPS.iterdir() if path.is_dir() and BACKUP_RE.fullmatch(path.name)]
    print_json(sorted(values, reverse=True))


def command_rollback(name: str) -> None:
    # An old artifact can restore the same unsafe code path as a new upload.
    raise MaintenanceError(
        "legacy code rollback disabled: use a schema-compatible reviewed image"
    )


def app_admin(arguments: list[str]) -> None:
    command = [
        "/usr/sbin/runuser",
        "-u",
        "meowrelay",
        "--",
        "/usr/bin/env",
        f"VOICE_MEMO_DB={DB_PATH}",
        "/usr/bin/python3",
        str(APP_DIR / "app.py"),
        *arguments,
    ]
    result = run(command, timeout=30)
    print(result.stdout, end="")


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(prog="meow-relay-maint")
    commands = result.add_subparsers(dest="command", required=True)
    commands.add_parser("status")
    logs = commands.add_parser("logs")
    logs.add_argument("lines", type=int, nargs="?", default=100)
    memo = commands.add_parser("memo")
    memo.add_argument("device")
    memo.add_argument("recording", type=int)
    commands.add_parser("deploy")
    commands.add_parser("restart")
    commands.add_parser("list-backups")
    rollback = commands.add_parser("rollback")
    rollback.add_argument("name")
    pairing = commands.add_parser("pairing-code")
    pairing.add_argument("ttl", type=int, nargs="?", default=900)
    revoke = commands.add_parser("revoke-device")
    revoke.add_argument("device")
    return result


def main() -> int:
    if os.geteuid() != 0:
        print("must run through the approved sudo rule", file=sys.stderr)
        return 1
    arguments = parser().parse_args()
    try:
        if arguments.command == "status":
            print_json(status_payload())
        elif arguments.command == "logs":
            command_logs(arguments.lines)
        elif arguments.command == "memo":
            command_memo(arguments.device, arguments.recording)
        elif arguments.command == "deploy":
            command_deploy()
        elif arguments.command == "restart":
            print_json({"status": "restarted", "health": restart_and_check()})
        elif arguments.command == "list-backups":
            command_list_backups()
        elif arguments.command == "rollback":
            command_rollback(arguments.name)
        elif arguments.command == "pairing-code":
            if not 60 <= arguments.ttl <= 3600:
                raise MaintenanceError("pairing TTL must be 60..3600 seconds")
            app_admin(["pairing-code", "--ttl", str(arguments.ttl)])
        elif arguments.command == "revoke-device":
            if not DEVICE_RE.fullmatch(arguments.device):
                raise MaintenanceError("invalid device id")
            app_admin(["revoke-device", arguments.device])
    except (MaintenanceError, subprocess.CalledProcessError, subprocess.TimeoutExpired, OSError, sqlite3.Error) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
