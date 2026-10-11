#!/usr/bin/env python3
"""Pinned, offline SPDX inventory of one private Docker-save archive.

Install: .venv-relay/bin/python scripts/container_sbom.py install
Scan:    .venv-relay/bin/python scripts/container_sbom.py scan \
         --archive private/image.tar --sha256 <archive-sha256> --output-dir private/sbom/new-run

Only the install command downloads a fixed public tool asset. Scan uses a
read-only frozen copy, an explicit docker-archive source and macOS network/write
sandbox. It never calls Docker, a registry, or a package enrichment service.
SBOM inventory is not a license/privacy/publication approval or a layer scan.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import urllib.parse
import urllib.request
import uuid

ROOT = Path(__file__).resolve().parents[1]
PRIVATE = ROOT / "private"
MAX_DOWNLOAD = 64 * 1024 * 1024
MAX_BINARY = 128 * 1024 * 1024
FORMAT = "spdx-json@2.3"
CONFIG = {
    "check-for-app-update": False, "enrich": [], "scope": "squashed", "parallelism": 2,
    "license": {"content": "none"}, "cache": {"dir": "cache", "ttl": "0s"},
    "golang": {"search-local-mod-cache-licenses": False, "search-local-vendor-licenses": False,
               "search-remote-licenses": False, "use-packages-lib": False, "local-mod-cache-dir": "cache/go"},
    "java": {"use-network": False, "use-maven-local-repository": False, "resolve-transitive-dependencies": False,
             "maven-local-repository-dir": "cache/maven"},
    "javascript": {"search-remote-licenses": False}, "python": {"search-remote-licenses": False},
    "cpp": {"vcpkg-allow-git-clone": False}, "registry": {"auth": []},
}


class SBOMError(RuntimeError):
    pass


def require(value, reason):
    if not value:
        raise SBOMError(reason)


def sha256(path):
    value = hashlib.sha256()
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(descriptor, "rb") as stream:
        require(stat.S_ISREG(os.fstat(stream.fileno()).st_mode), "hash input must be a regular file")
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def private_path(value):
    path = Path(os.path.abspath(value))
    require(path.is_relative_to(PRIVATE) and path != PRIVATE, "path must be below project private/")
    require(not any(part.is_symlink() for part in (path, *path.parents) if part.is_relative_to(ROOT)), "symlink path refused")
    require(path.resolve().is_relative_to(PRIVATE.resolve()), "path escapes project private/")
    require(not any(ord(char) < 32 for char in str(path)), "invalid path encoding")
    return path


def regular_file(path):
    info = path.lstat()
    require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1, "a regular non-hardlinked file is required")
    return info


def write_json(path, value):
    temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    try:
        with temporary.open("x", encoding="utf-8") as stream:
            os.chmod(temporary, 0o600)
            json.dump(value, stream, sort_keys=True, indent=2); stream.write("\n")
            stream.flush(); os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def locked_tool():
    value = json.loads((ROOT / "scripts/container-tools.lock.json").read_text())["syft"]
    require(value["platform"] == "darwin_arm64" and re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", value["version"]), "unsupported Syft lock")
    expected = f"https://github.com/anchore/syft/releases/download/v{value['version']}/syft_{value['version']}_darwin_arm64.tar.gz"
    require(value["url"] == expected, "tool download URL does not match fixed official release")
    for key in ("archive_sha256", "binary_sha256", "license_sha256"):
        require(re.fullmatch(r"[a-f0-9]{64}", value[key]), "tool hash missing")
    return value


class OfficialRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, file, code, message, headers, url):
        target = urllib.parse.urlsplit(url)
        require(target.scheme == "https" and target.hostname in {"github.com", "release-assets.githubusercontent.com"}
                and target.port in (None, 443) and not target.username and not target.password and not target.fragment,
                "non-official tool redirect refused")
        return super().redirect_request(request, file, code, message, headers, url)


def download(url, destination):
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), OfficialRedirect())
    with opener.open(urllib.request.Request(url, headers={"User-Agent": "VoiceMemo-pinned-tool-download"}), timeout=60) as source:
        with destination.open("xb") as target:
            destination.chmod(0o600)
            size = 0
            while chunk := source.read(1024 * 1024):
                size += len(chunk)
                require(size <= MAX_DOWNLOAD, "tool archive exceeds download limit")
                target.write(chunk)
            target.flush(); os.fsync(target.fileno())


def tool_directory(lock):
    return private_path(PRIVATE / "tools" / "syft" / (lock["version"] + "-" + lock["platform"]))


def verify_tool(lock, directory):
    for name, key in (("syft", "binary_sha256"), ("LICENSE", "license_sha256"), ("release.tar.gz", "archive_sha256")):
        target = directory / name
        regular_file(target)
        require(sha256(target) == lock[key], "installed Syft artifact differs from lock")
    return directory / "syft"


def install():
    lock = locked_tool()
    directory = tool_directory(lock)
    if directory.exists():
        verify_tool(lock, directory)
        return {"status": "VERIFIED", "version": lock["version"], "binary_sha256": lock["binary_sha256"]}
    directory.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    staging = directory.with_name(directory.name + "-download-" + uuid.uuid4().hex)
    staging.mkdir(mode=0o700)
    try:
        package = staging / "release.tar.gz"
        download(lock["url"], package)
        require(sha256(package) == lock["archive_sha256"], "Syft download hash mismatch")
        seen = set()
        with tarfile.open(package, "r:gz") as archive:
            for item in archive:
                require(item.name in {"syft", "LICENSE", "README.md", "CHANGELOG.md"} and item.name not in seen
                        and item.isfile() and 0 < item.size <= MAX_BINARY, "unexpected tool archive entry")
                seen.add(item.name)
                with archive.extractfile(item) as source, (staging / item.name).open("xb") as target:
                    shutil.copyfileobj(source, target, 1024 * 1024)
                (staging / item.name).chmod(0o500 if item.name == "syft" else 0o400)
        require(seen == {"syft", "LICENSE", "README.md", "CHANGELOG.md"}, "tool archive incomplete")
        package.chmod(0o400)
        verify_tool(lock, staging)
        require(not directory.exists(), "concurrent tool installation refused")
        staging.rename(directory)
    finally:
        if staging.exists():
            shutil.rmtree(staging)
    return {"status": "VERIFIED", "version": lock["version"], "binary_sha256": lock["binary_sha256"],
            "signature_verified": False}


def docker_archive(path):
    """Validate one Docker-save image without extracting any archive member."""
    with tarfile.open(path, "r:") as archive:
        items = {}
        for member in archive:
            name = PurePosixPath(member.name)
            require(not name.is_absolute() and ".." not in name.parts and member.name not in items
                    and (member.isfile() or member.isdir()), "unsafe Docker archive member")
            require(len(items) < 100000, "Docker archive entry limit")
            items[member.name] = member
        def read_json(name):
            member = items.get(name)
            require(member and member.isfile() and 0 < member.size <= 4 * 1024 * 1024, "missing bounded Docker metadata")
            with archive.extractfile(member) as file:
                return json.load(file)
        manifest = read_json("manifest.json")
        require(isinstance(manifest, list) and len(manifest) == 1 and isinstance(manifest[0], dict), "exactly one Docker-save image required")
        entry = manifest[0]
        require(isinstance(entry.get("Config"), str) and isinstance(entry.get("Layers"), list) and entry["Layers"], "incomplete Docker manifest")
        for name in [entry["Config"], *entry["Layers"]]:
            require(isinstance(name, str) and name in items and items[name].isfile(), "Docker manifest references missing regular member")
        config = read_json(entry["Config"])
        require(config.get("os") == "linux" and config.get("architecture") in {"arm64", "amd64"}, "unsupported image platform")
        with archive.extractfile(items[entry["Config"]]) as file:
            image_id = "sha256:" + hashlib.sha256(file.read()).hexdigest()
        return {"image_id": image_id, "platform": "linux/" + config["architecture"], "layers": len(entry["Layers"])}


def environment(directory):
    # Do not inherit Syft profiles, proxy credentials, registry auth, Docker
    # endpoints, package-manager settings or the operator's HOME/configuration.
    return {"PATH": "/usr/bin:/bin", "HOME": str(directory / "home"), "TMPDIR": str(directory / "tmp"),
            "XDG_CONFIG_HOME": str(directory / "config"), "XDG_CACHE_HOME": str(directory / "cache"),
            "SYFT_CHECK_FOR_APP_UPDATE": "false", "LANG": "C", "LC_ALL": "C"}


def scan(archive, expected_sha256, output_dir):
    require(re.fullmatch(r"[a-f0-9]{64}", expected_sha256 or ""), "exact archive SHA256 required")
    require(sys.platform == "darwin" and platform.machine() == "arm64", "locked runner supports only darwin/arm64")
    sandbox = Path("/usr/bin/sandbox-exec")
    require(sandbox.is_file() and os.access(sandbox, os.X_OK), "macOS network/write sandbox is required")
    source, directory = private_path(archive), private_path(output_dir)
    regular_file(source)
    require(not directory.exists(), "SBOM output directory must be new")
    lock = locked_tool(); binary = verify_tool(lock, tool_directory(lock))
    directory.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    directory.mkdir(mode=0o700)
    report = {"kind": "docker-archive-sbom", "status": "RUNNING", "tool": "syft", "tool_version": lock["version"],
              "tool_binary_sha256": lock["binary_sha256"], "tool_archive_sha256": lock["archive_sha256"],
              "tool_lock_sha256": sha256(ROOT / "scripts/container-tools.lock.json"),
              "runner_sha256": sha256(Path(__file__)),
              "archive_sha256": expected_sha256, "format": FORMAT, "scope": "squashed",
              "network": "denied-by-macos-sandbox", "license_approval": False, "publication_ready": False,
              "signature_verified": False, "layer_history_scanned": False}
    report_path = directory / "report.json"
    write_json(report_path, report)
    try:
        frozen = directory / "image.tar"
        descriptor = os.open(source, os.O_RDONLY | os.O_NOFOLLOW)
        with os.fdopen(descriptor, "rb") as src, frozen.open("xb") as target:
            frozen.chmod(0o600); shutil.copyfileobj(src, target, 1024 * 1024)
            target.flush(); os.fsync(target.fileno())
        frozen.chmod(0o400)
        require(sha256(frozen) == expected_sha256, "archive does not match expected SHA256")
        report.update(docker_archive(frozen))
        for name in ("home", "tmp", "config", "cache"):
            (directory / name).mkdir(mode=0o700)
        write_json(directory / "syft.yaml", CONFIG)
        # Read access remains available to macOS runtime libraries; only this
        # run directory may be written. Network is denied even if a cataloger
        # attempts a connection despite the explicit offline configuration.
        quoted = json.dumps(str(directory))
        policy = ('(version 1)\n(allow default)\n(deny network*)\n(deny file-write*)\n'
                  '(allow file-write* (subpath ' + quoted + '))\n'
                  '(deny file-write* (literal ' + json.dumps(str(frozen)) + '))\n')
        (directory / "sandbox.sb").write_text(policy); (directory / "sandbox.sb").chmod(0o400)
        prefix = [str(sandbox), "-f", "sandbox.sb", str(binary)]
        def invoke(arguments, log):
            with (directory / log).open("xb") as errors:
                os.chmod(directory / log, 0o600)
                completed = subprocess.run(prefix + arguments, cwd=directory, env=environment(directory),
                    stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=errors, timeout=1800)
            require(completed.returncode == 0, "Syft command failed; inspect private log")
            return completed.stdout
        version = json.loads(invoke(["version", "-o", "json"], "version.log"))
        require(version.get("version") == lock["version"] and version.get("gitCommit") == lock["commit"]
                and version.get("platform") == "darwin/arm64", "Syft runtime version differs from lock")
        invoke(["scan", "docker-archive:image.tar", "--config", "syft.yaml", "--scope", "squashed",
                "--source-name", "voice-memo-relay", "--output", FORMAT + "=sbom.spdx.json",
                "--output", "syft-json=sbom.syft.json"], "scan.log")
        native_path = directory / "sbom.syft.json"
        regular_file(native_path); native_path.chmod(0o600)
        require(native_path.stat().st_size <= 128 * 1024 * 1024, "native SBOM output limit")
        native = json.loads(native_path.read_text())
        native_source = native.get("source", {})
        metadata = native_source.get("metadata", {})
        manifest_digest = metadata.get("manifestDigest", "")
        require(native_source.get("type") == "image" and native_source.get("name") == "voice-memo-relay"
                and metadata.get("imageID") == report["image_id"]
                and metadata.get("os") == "linux"
                and metadata.get("architecture") == report["platform"].split("/")[1]
                and re.fullmatch(r"sha256:[a-f0-9]{64}", manifest_digest), "native SBOM source differs from archive")
        require("sha256:" + hashlib.sha256(base64.b64decode(metadata.get("config", ""), validate=True)).hexdigest() == report["image_id"]
                and "sha256:" + hashlib.sha256(base64.b64decode(metadata.get("manifest", ""), validate=True)).hexdigest() == manifest_digest,
                "native SBOM raw source metadata digest mismatch")
        require(native.get("schema", {}).get("version") == version.get("schemaVersion")
                and isinstance(native.get("artifacts"), list), "native SBOM schema/inventory missing")
        output = directory / "sbom.spdx.json"
        regular_file(output); output.chmod(0o600)
        require(output.stat().st_size <= 128 * 1024 * 1024, "SBOM output limit")
        sbom = json.loads(output.read_text())
        require(sbom.get("spdxVersion") == "SPDX-2.3" and isinstance(sbom.get("packages"), list)
                and sbom["packages"] and isinstance(sbom.get("files", []), list), "missing SPDX package inventory")
        # Syft 1.54 emits the SPDX document relationship form, rather than the
        # optional documentDescribes shorthand. Its source checksum is the
        # manifest digest, not the Docker config/image ID. Native output from
        # the same scan binds both independently without rewriting metadata.
        described = [relation.get("relatedSpdxElement") for relation in sbom.get("relationships", [])
                     if isinstance(relation, dict) and relation.get("relationshipType") == "DESCRIBES"
                     and relation.get("spdxElementId") == sbom.get("SPDXID")]
        package_ids = [package.get("SPDXID") for package in sbom["packages"] if isinstance(package, dict)]
        require(len(described) == 1 and described[0] in package_ids
                and len(package_ids) == len(sbom["packages"]) and len(set(package_ids)) == len(package_ids),
                "missing SPDX source identity")
        source_packages = sum(identifier in described for identifier in package_ids)
        root_package = next(package for package in sbom["packages"] if package["SPDXID"] == described[0])
        require(root_package.get("primaryPackagePurpose") == "CONTAINER" and root_package.get("versionInfo") == native_source.get("version")
                and {"algorithm": "SHA256", "checksumValue": manifest_digest.split(":")[1]} in root_package.get("checksums", []),
                "SPDX source differs from Docker archive image")
        require(len(native["artifacts"]) == len(package_ids) - source_packages, "SPDX/native package counts differ")
        require(sha256(frozen) == expected_sha256 and sha256(source) == expected_sha256, "archive changed during SBOM scan")
        verify_tool(lock, tool_directory(lock))
        report.update(status="PASS_INVENTORY_ONLY", spdx_version=sbom["spdxVersion"], package_count=len(sbom["packages"]),
                      source_package_count=source_packages, software_package_count=len(package_ids) - source_packages,
                      syft_manifest_digest=manifest_digest, native_schema_version=native["schema"]["version"],
                      native_output_file=native_path.name, native_output_sha256=sha256(native_path),
                      file_count=len(sbom.get("files", [])), output_file=output.name, output_sha256=sha256(output),
                      config_sha256=sha256(directory / "syft.yaml"), sandbox_sha256=sha256(directory / "sandbox.sb"),
                      scan_log_sha256=sha256(directory / "scan.log"), tool_runtime=version)
    except BaseException as error:
        report.update(status="FAIL", error=type(error).__name__, reason=str(error) if isinstance(error, SBOMError) else "tool, archive, or output failure")
        raise
    finally:
        write_json(report_path, report)
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    actions = parser.add_subparsers(dest="action", required=True)
    actions.add_parser("install", allow_abbrev=False)
    inspect = actions.add_parser("scan", allow_abbrev=False)
    inspect.add_argument("--archive", required=True, type=Path)
    inspect.add_argument("--sha256", required=True)
    inspect.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        value = install() if args.action == "install" else scan(args.archive, args.sha256, args.output_dir)
        print(json.dumps(value, sort_keys=True)); return 0
    except (SBOMError, OSError, ValueError, tarfile.TarError, subprocess.SubprocessError) as error:
        print(str(error) if isinstance(error, SBOMError) else "SBOM operation failed; inspect private evidence", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
