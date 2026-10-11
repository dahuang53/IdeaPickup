"""Fixed deployment operations; never source environment files or restore a DB."""
from __future__ import annotations
import argparse
import contextlib
import fcntl
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
import uuid
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[1]
PRIVATE = ROOT / "private"
IMAGE = re.compile(r"(?:[A-Za-z0-9][A-Za-z0-9._:/-]*@)?sha256:[0-9a-f]{64}\Z")
KEYS = {"VOICE_MEMO_IMAGE", "VOICE_MEMO_PROJECT", "VOICE_MEMO_PUBLIC_URL", "VOICE_MEMO_PORT",
        "VOICE_MEMO_SUBNET", "VOICE_MEMO_GATEWAY", "VOICE_MEMO_RELAY_IP", "VOICE_MEMO_PROXY_IP",
        "VOICE_MEMO_PROXY", "VOICE_MEMO_TLS_DIR", "VOICE_MEMO_HTTPS_PORT", "VOICE_MEMO_HTTPS_BIND",
        "VOICE_MEMO_DRAIN_TIMEOUT"}



class DeploymentError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise DeploymentError(message)


def private_path(value):
    path = Path(value).expanduser().resolve()
    require(path.is_relative_to(PRIVATE.resolve()), "deployment inputs and state must stay under project private/")
    require("," not in str(path) and all(ord(c) >= 32 for c in str(path)), "deployment mount paths contain unsupported characters")
    return path


def read_env(path):
    values = {}
    for line in path.read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        require("=" in line, "environment entry must use KEY=value")
        key, value = line.split("=", 1)
        require(key in KEYS and key not in values, "unknown or repeated deployment environment field")
        require(value == value.strip() and all(ord(c) >= 32 and ord(c) != 127 for c in value), "invalid deployment field encoding")
        require(not any(c in value for c in "`$\"'"), "environment expansion and shell quoting are not supported")
        values[key] = value
    validate_values(values)
    return values


def validate_values(values):
    require(IMAGE.fullmatch(values.get("VOICE_MEMO_IMAGE", "")), "image must be an immutable sha256 ID or registry digest")
    require(re.fullmatch(r"[a-z][a-z0-9-]{0,39}", values.get("VOICE_MEMO_PROJECT", "voice-memo")), "invalid Compose project name")
    parsed = urlsplit(values.get("VOICE_MEMO_PUBLIC_URL", ""))
    require(parsed.scheme == "https" and parsed.hostname and not parsed.username and not parsed.password and not parsed.query and not parsed.fragment,
            "public URL must be HTTPS without credentials, query or fragment")
    require(len(values["VOICE_MEMO_PUBLIC_URL"].encode()) <= 240 and not parsed.path.endswith("/") and "\\" not in values["VOICE_MEMO_PUBLIC_URL"], "public URL must be normalized and at most 240 bytes")
    for field, default in (("VOICE_MEMO_PORT", "8765"), ("VOICE_MEMO_HTTPS_PORT", "8443")):
        require(values.get(field, default).isdigit() and 1024 <= int(values.get(field, default)) <= 65535, "invalid unprivileged port")
    subnet = ipaddress.ip_network(values.get("VOICE_MEMO_SUBNET", "172.30.86.0/24"))
    require(subnet.version == 4 and subnet.prefixlen == 24 and subnet.is_private, "dedicated private IPv4 /24 network required")
    addresses = [ipaddress.ip_address(values.get(key, default)) for key, default in (("VOICE_MEMO_GATEWAY", "172.30.86.1"), ("VOICE_MEMO_RELAY_IP", "172.30.86.2"), ("VOICE_MEMO_PROXY_IP", "172.30.86.3"))]
    require(len(set(addresses)) == 3 and all(address in subnet and address not in {subnet.network_address, subnet.broadcast_address} for address in addresses), "network addresses must be distinct usable members of subnet")
    require(values.get("VOICE_MEMO_PROXY", "0") in {"0", "1"}, "invalid proxy selection")
    timeout = values.get("VOICE_MEMO_DRAIN_TIMEOUT", "1800")
    require(timeout.isdigit() and 60 <= int(timeout) <= 7200, "drain timeout must be 60..7200 seconds")
    if values.get("VOICE_MEMO_PROXY", "0") == "1":
        tls_input = Path(values.get("VOICE_MEMO_TLS_DIR", ""))
        require(not tls_input.is_symlink(), "TLS directory cannot be a symlink")
        tls = private_path(tls_input)
        require(tls.is_dir() and not tls.is_symlink(), "missing dedicated TLS directory")
        require(set(path.name for path in tls.iterdir()) == {"certificate.pem", "private-key.pem"}, "TLS directory must contain only certificate.pem and private-key.pem")
        for filename in ("certificate.pem", "private-key.pem"):
            path = tls / filename
            info = path.lstat()
            require(path.is_file() and not path.is_symlink() and info.st_uid == 0 and info.st_mode & 0o777 == 0o600,
                    "TLS files must be root-owned regular files with mode 0600")
        values["VOICE_MEMO_TLS_DIR"] = str(tls)
        bind = ipaddress.ip_address(values.get("VOICE_MEMO_HTTPS_BIND", "127.0.0.1"))
        require(bind.version == 4, "proxy bind must be an explicit IPv4 address")


def run(command, *, env=None, json_output=False):
    result = subprocess.run(command, cwd=ROOT, env=env, check=False, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        # Never print docker inspect/env dumps, arbitrary provider content or logs.
        raise DeploymentError("operation failed: " + " ".join(command[:3]) + " (inspect protected local runtime diagnostics)")
    if json_output:
        try:
            return json.loads(result.stdout)
        except ValueError:
            raise DeploymentError("operation returned invalid JSON") from None
    return result.stdout.strip()


class Deployment:
    def __init__(self, values, env_path):
        self.values, self.env_path = dict(values), env_path
        self.project = values.get("VOICE_MEMO_PROJECT", "voice-memo")

    def compose(self, *arguments, values=None, json_output=False):
        configuration = values or self.values
        # Only validated deployment metadata, never plaintext provider/upload keys.
        environment = {key: value for key, value in os.environ.items() if not key.startswith("VOICE_MEMO_")}
        environment.update(configuration)
        command = ["docker", "compose", "--project-directory", str(ROOT / "deploy"), "--env-file", "/dev/null", "-p", self.project,
                   "-f", str(ROOT / "deploy" / "compose.yaml")]
        if configuration.get("VOICE_MEMO_PROXY", "0") == "1":
            command += ["-f", str(ROOT / "deploy" / "compose.proxy.yaml")]
        return run(command + list(arguments), env=environment, json_output=json_output)

    def image_check(self, image, pull=False):
        require(IMAGE.fullmatch(image), "image digest required")
        if pull and "@sha256:" in image:
            run(["docker", "pull", image])
        result = run(["docker", "image", "inspect", image], json_output=True)
        require(len(result) == 1, "image identity ambiguous")
        item = result[0]
        require(item.get("Os") == "linux" and item.get("Architecture") in {"amd64", "arm64"}, "unsupported image platform")
        if image.startswith("sha256:"):
            require(item["Id"] == image, "local image digest mismatch")
        else:
            digest = image.rsplit("@", 1)[1]
            require(any(entry.endswith("@" + digest) for entry in item.get("RepoDigests", [])), "registry manifest digest mismatch")
        require(item.get("Config", {}).get("User") == "10001:10001", "runtime image must default to non-root UID 10001")
        return item["Id"]

    def validate(self):
        info = run(["docker", "info", "--format", "{{json .}}"], json_output=True)
        require(info.get("OSType") == "linux", "Linux container engine required")
        options = " ".join(info.get("SecurityOptions", []))
        require("rootless" not in options and "userns" not in options, "rootless and user namespace mappings are not qualified")
        self.compose("config", "--quiet")
        self.image_check(self.values["VOICE_MEMO_IMAGE"])
        return {"validated": True, "project": self.project, "image": self.values["VOICE_MEMO_IMAGE"],
                "live_permissions_tested": False, "engine_os": "linux"}

    def require_running_image(self, image):
        expected = self.image_check(image)
        for service in ("relay", "config-broker"):
            identifier = self.compose("ps", "--all", "-q", service)
            if identifier:
                require("\n" not in identifier, "multiple service containers are unsupported")
                actual = run(["docker", "inspect", "--format", "{{.Image}}", identifier])
                running = run(["docker", "inspect", "--format", "{{json .State.Running}}", identifier], json_output=True)
                require(not running or actual == expected, "existing running image differs; use the normal upgrade operation")

    def start(self):
        self.validate()
        # 'start' cannot silently switch an existing deployment's image.
        self.require_running_image(self.values["VOICE_MEMO_IMAGE"])
        if self.running():
            state = self.control("status")
            if state.get("reason") == "closed" and state.get("safe_to_stop") is True:
                self.compose("stop", "relay", "config-broker")
                self.compose("up", "-d", "--force-recreate", "--wait", "--wait-timeout", "120")
            else:
                self.compose("up", "-d", "--wait", "--wait-timeout", "120")
        else:
            self.compose("up", "-d", "--wait", "--wait-timeout", "120")
        return self.control("status")

    def control(self, operation):
        return self.compose("exec", "-T", "relay", "python", "-m", "relay", "control", operation, "--json", json_output=True)

    def storage_check(self, image):
        # The writer has been drained/closed/stopped. Use the image's existing
        # current-format reader against the current DB, not a PASS/qualification file.
        return run(["docker", "run", "--rm", "--network", "none", "--read-only", "--user", "10001:10001",
                    "--cap-drop", "ALL", "--security-opt", "no-new-privileges:true",
                    "--tmpfs", "/var/lib/voice-memo-work:rw,noexec,nosuid,mode=0700,uid=10001,gid=10001",
                    "--mount", f"type=volume,src={self.project}_data,dst=/var/lib/voice-memo,readonly",
                    "--entrypoint", "python", image, "/opt/voice-memo/deploy/storage-check.py"], json_output=True)

    def drain(self):
        response = self.control("drain")
        require(response.get("ok") and response.get("accepting_new") is False, "service did not enter drain mode")
        deadline = time.monotonic() + int(self.values.get("VOICE_MEMO_DRAIN_TIMEOUT", "1800"))
        while True:
            state = self.control("drain-status")
            if state.get("ok") and state.get("draining") and state.get("safe_to_stop") is True:
                require(all(state.get(name) == 0 for name in ("active_sessions", "active_jobs", "active_requests")), "inconsistent drain status")
                return state
            if time.monotonic() >= deadline:
                self.control("resume")
                raise DeploymentError("drain timed out; upgrade refused and existing service resumed")
            time.sleep(2)

    def commit_image(self, image):
        updated = dict(self.values, VOICE_MEMO_IMAGE=image)
        atomic_json_or_env(self.env_path, updated, env_format=True)
        self.values = updated

    def stop_cleanly(self):
        self.drain()
        response = self.control("close")
        require(response.get("ok") and response.get("closed"), "service close not confirmed; update stopped")
        self.compose("stop", "relay", "config-broker")

    def running(self, values=None):
        identifier = self.compose("ps", "--all", "-q", "relay", values=values)
        if not identifier:
            return False
        state = run(["docker", "inspect", "--format", "{{json .State}}", identifier], json_output=True)
        # Restarting/PID 0 has no live application to drain. Stop its restart
        # loop before any fallback reader opens the volume; never mistake it
        # for an accepting worker merely because Docker marks Running true.
        return state.get("Running") is True and not state.get("Restarting") and state.get("Pid", 0) > 0

    def verify_service(self, before):
        after = self.control("status")
        require(after.get("instance_uuid") == before.get("instance_uuid") and bool(after.get("instance_uuid")), "instance identity changed")
        require(after.get("cleanup_ok") is True and after.get("worker_available") is True, "service startup recovery incomplete")
        require(after.get("opus_available") is True and after.get("maintenance") is False,
                "candidate codec or maintenance state unavailable")
        if before.get("configured"):
            require(after.get("configured") is True, "configuration not available after update")
        if after.get("configured"):
            require(after.get("ready") is True and after.get("reason") is None,
                    "configured service cannot accept recordings")
        else:
            require(after.get("ready") is False and after.get("reason") == "not_configured",
                    "unconfigured service has another recovery error")
        return after

    def switch(self, image):
        self.validate()
        self.image_check(image, pull=True)
        old = self.values["VOICE_MEMO_IMAGE"]
        self.require_running_image(old)
        require(image != old, "target image already selected")
        before = self.control("status")
        self.stop_cleanly()
        original_values = dict(self.values)
        selected = dict(self.values, VOICE_MEMO_IMAGE=image)
        try:
            require(self.storage_check(image).get("compatible") is True, "candidate cannot read current data")
            self.commit_image(image)
            self.compose("up", "-d", "--wait", "--wait-timeout", "120", values=selected)
            self.verify_service(before)
        except Exception:
            self.values = selected
            if self.running(selected):
                # New work may have been admitted or the status response lost.
                # Never force-stop it just because candidate verification failed.
                self.stop_cleanly()
            else:
                self.compose("stop", "relay", "config-broker", values=selected)
            require(self.storage_check(old).get("compatible") is True,
                    "old code cannot read current data; remain stopped, do not replace the database")
            self.commit_image(old)
            self.compose("up", "-d", "--wait", "--wait-timeout", "120", values=original_values)
            self.values = original_values
            self.verify_service(before)
            raise DeploymentError("update failed; previous image restored with current data") from None
        return {"ok": True, "image": image, "previous_image": old, "database_restored": False}


def atomic_json_or_env(path, value, *, env_format=False):
    data = "".join(key + "=" + entry + "\n" for key, entry in sorted(value.items())) if env_format else json.dumps(value, indent=2) + "\n"
    fd, temporary = tempfile.mkstemp(prefix=".deploy-", dir=path.parent)
    try:
        os.fchmod(fd, 0o600)
        with os.fdopen(fd, "w") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        with contextlib.suppress(FileNotFoundError):
            os.unlink(temporary)




def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--env-file", default=str(PRIVATE / "deploy" / "relay.env"))
    parser.add_argument("operation", choices=("validate", "start", "status", "upgrade"))
    parser.add_argument("image", nargs="?")
    args = parser.parse_args()
    try:
        env_path = private_path(args.env_file)
        deployment = Deployment(read_env(env_path), env_path)
        lock_path = env_path.parent / ".deployment.lock"
        with lock_path.open("a+") as lock:
            os.chmod(lock_path, 0o600)
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            if args.operation == "validate":
                result = deployment.validate()
            elif args.operation == "start":
                result = deployment.start()
            elif args.operation == "status":
                result = deployment.control("status")
            else:
                require(args.image, "upgrade requires a target image digest")
                result = deployment.switch(args.image)
            print(json.dumps(result, sort_keys=True))
    except (DeploymentError, OSError, ValueError) as error:
        message = str(error) if isinstance(error, DeploymentError) else "deployment input, lock, or tool failure"
        print(message, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
