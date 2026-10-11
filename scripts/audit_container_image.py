#!/usr/bin/env python3
"""Offline, bounded Docker-save/OCI tar audit; never extracts, executes or networks.

This is a heuristic privacy gate, not an SBOM generator or publication approval.
Binary files remain REVIEW_REQUIRED even when printable-string scanning is clean.
Only one selected platform/image per archive is supported; extra OCI blobs fail.
"""
from __future__ import annotations
import argparse
import codecs
from dataclasses import asdict, dataclass
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import tarfile
import tempfile
import zlib

from export_public_source import scan_text

ROOT = Path(__file__).resolve().parents[1]
CHUNK = 65536
DIGEST = re.compile(r"sha256:[a-f0-9]{64}\Z")
MANIFESTS = {"application/vnd.oci.image.manifest.v1+json", "application/vnd.docker.distribution.manifest.v2+json"}
CONFIGS = {"application/vnd.oci.image.config.v1+json", "application/vnd.docker.container.image.v1+json"}
LAYERS = {"application/vnd.oci.image.layer.v1.tar": "tar", "application/vnd.oci.image.layer.v1.tar+gzip": "gzip",
          "application/vnd.oci.image.layer.nondistributable.v1.tar": "tar",
          "application/vnd.oci.image.layer.nondistributable.v1.tar+gzip": "gzip",
          "application/vnd.docker.image.rootfs.diff.tar": "tar", "application/vnd.docker.image.rootfs.diff.tar.gzip": "gzip",
          "application/vnd.docker.image.rootfs.foreign.diff.tar.gzip": "gzip"}


@dataclass(frozen=True)
class Limits:
    archive_bytes: int = 2 * 1024**3
    layer_bytes: int = 1024**3
    total_uncompressed_bytes: int = 4 * 1024**3
    file_bytes: int = 512 * 1024**2
    metadata_bytes: int = 8 * 1024**2
    outer_members: int = 10000
    layer_members: int = 200000
    findings: int = 50000
    text_run_bytes: int = 1024**2


class Problem(Exception):
    def __init__(self, code, kind="FAIL"):
        self.code, self.kind = code, kind
        super().__init__(code)


def need(value, code, kind="FAIL"):
    if not value:
        raise Problem(code, kind)


def sha(data):
    return "sha256:" + hashlib.sha256(data).hexdigest()


def location(label, name):
    # Even an entry name or link target can contain the secret being scanned.
    return label + "/name-sha256:" + hashlib.sha256(name.encode("utf-8", "surrogatepass")).hexdigest()


def member_name(name, *, directory=False):
    need(isinstance(name, str), "unsafe_archive_path")
    path = PurePosixPath(name)
    need(not path.is_absolute() and ".." not in path.parts and
         "\\" not in name and "\0" not in name and not re.match(r"^[A-Za-z]:", name), "unsafe_archive_path")
    need(bool(path.parts) or directory, "empty_archive_path")
    return path.as_posix()


def open_without_links(path):
    """Open every host path component without following filesystem links."""
    path = Path(os.path.abspath(path))
    directory = os.open(path.anchor, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        for component in path.parts[1:-1]:
            following = os.open(component, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=directory)
            os.close(directory)
            directory = following
        return os.open(path.name, os.O_RDONLY | os.O_NOFOLLOW, dir_fd=directory)
    finally:
        os.close(directory)


def valid_digest(value):
    return isinstance(value, str) and DIGEST.fullmatch(value) is not None


def json_load(data):
    def unique(pairs):
        value = {}
        for key, item in pairs:
            need(key not in value, "duplicate_json_key")
            value[key] = item
        return value
    try:
        return json.loads(data, object_pairs_hook=unique,
                          parse_constant=lambda _: (_ for _ in ()).throw(Problem("nonfinite_json")))
    except (ValueError, UnicodeError, RecursionError):
        raise Problem("invalid_metadata_json") from None


class HashReader:
    """Short-read streaming adapter with a hard byte limit and bounded tail."""
    def __init__(self, source, maximum, charge=None):
        self.source, self.maximum, self.charge = source, maximum, charge
        self.bytes = 0
        self.digest = hashlib.sha256()
        self.tail = b""

    def read(self, size=CHUNK):
        need(size >= 0, "unbounded_reader_request", "INCOMPLETE")
        data = self.source.read(min(size, CHUNK, max(1, self.maximum - self.bytes + 1)))
        self.bytes += len(data)
        need(self.bytes <= self.maximum, "layer_byte_limit", "INCOMPLETE")
        if self.charge:
            self.charge(len(data))
        self.digest.update(data)
        self.tail = (self.tail + data)[-2 * CHUNK:]
        return data

    def since(self, offset):
        need(self.bytes - len(self.tail) <= offset <= self.bytes, "tar_trailer_not_observable", "INCOMPLETE")
        return self.tail[offset - (self.bytes - len(self.tail)):]


class SingleGzipReader:
    """Bounded gzip decoder with an explicit supported envelope.

    zlib validates the stream CRC/size. The filename field is read under a
    metadata budget and scanned before decompression. Other optional metadata
    and concatenated members are unsupported, never silently consumed.
    """
    def __init__(self, source, metadata_limit=0, scan_header=None):
        self.source, self.done = source, False
        header = b""
        while len(header) < 10:
            block = source.read(10 - len(header))
            need(bool(block), "truncated_gzip_header")
            header += block
        need(header[:3] == b"\x1f\x8b\x08", "invalid_gzip_header")
        need(header[3] & ~8 == 0, "gzip_optional_header_unsupported", "INCOMPLETE")
        self.pending = header
        if header[3] & 8:
            need(scan_header is not None, "gzip_header_scanner_required", "INCOMPLETE")
            name = bytearray()
            while True:
                # The budget includes the fixed header and final NUL. No
                # header read or unterminated filename allocation is unbounded.
                remaining = metadata_limit - 10 - len(name)
                need(remaining > 0, "gzip_header_byte_limit", "INCOMPLETE")
                block = source.read(min(CHUNK, remaining))
                need(bool(block), "truncated_gzip_filename")
                end = block.find(b"\0")
                if end >= 0:
                    name.extend(block[:end])
                    self.pending = header + bytes(name) + block[end:]
                    scan_header("filename", bytes(name))
                    break
                name.extend(block)
        self.decoder = zlib.decompressobj(wbits=31)

    def read(self, size=CHUNK):
        need(0 < size <= CHUNK, "unbounded_gzip_request", "INCOMPLETE")
        if self.done: return b""
        output = bytearray()
        while len(output) < size:
            if not self.pending:
                self.pending = self.source.read(CHUNK)
                need(bool(self.pending), "truncated_gzip_stream")
            output.extend(self.decoder.decompress(self.pending, size - len(output)))
            self.pending = self.decoder.unconsumed_tail
            if self.decoder.eof:
                need(not self.decoder.unused_data and not self.source.read(CHUNK),
                     "gzip_extra_member_or_trailer_unsupported", "INCOMPLETE")
                self.done = True
                break
        return bytes(output)


class Audit:
    def __init__(self, platform, expected_config=None, expected_manifest=None, limits=None):
        need(platform in {"linux/amd64", "linux/arm64"}, "unsupported_platform")
        need(expected_config is not None or expected_manifest is not None, "expected_identity_required")
        need(all(value is None or valid_digest(value) for value in (expected_config, expected_manifest)), "invalid_expected_identity")
        self.platform, self.expected_config, self.expected_manifest = platform, expected_config, expected_manifest
        self.limits = limits or Limits()
        need(all(type(value) is int and value > 0 for value in asdict(self.limits).values()), "invalid_budget")
        self.used = set()
        self.issues = set()
        self.total_bytes = 0
        self.total_members = 0
        self.report = {"version": 1, "status": "INCOMPLETE", "complete": False,
                       "platform": platform, "expected_config": expected_config, "expected_manifest": expected_manifest,
                       "budgets": asdict(self.limits), "findings": [], "problems": [], "layers": [],
                       "counts": {"regular_files": 0, "links": 0, "binary_files": 0, "whiteouts": 0, "special_members": 0},
                       "privacy_cleared": False, "publication_ready": False, "sbom_generated": False,
                       "scope": "selected immutable image metadata and every historical layer; no extraction/link following; heuristic text and binary-string checks; binary/manual rights/privacy review remains"}

    def finding(self, label, category, line=None):
        key = (label, category, line)
        if key in self.issues:
            return
        need(len(self.issues) < self.limits.findings, "finding_budget_exhausted", "INCOMPLETE")
        self.issues.add(key)
        item = {"location": label, "category": category}
        if line is not None:
            item["line"] = line
        self.report["findings"].append(item)

    def text(self, label, text, line=1):
        # The reused source heuristics include unbounded token patterns. Bound
        # each regex input as well as memory; unusually long lines still get
        # overlapping string checks, but always require manual review because
        # a match spanning more than the overlap cannot be ruled out.
        for offset, value in enumerate(text.splitlines()):
            need(len(value.encode("utf-8")) <= self.limits.text_run_bytes, "text_run_limit", "INCOMPLETE")
            if len(value) > 4096:
                self.finding(label, "long_text_requires_review", line + offset)
            for start in range(0, max(1, len(value)), 3584):
                for item in scan_text(label, value[start:start + 4096]):
                    self.finding(label, item["category"], line + offset)

    def tar_info(self, label, maximum):
        """Bound extended headers before stdlib tarfile allocates their payload.

        Scan raw extensions too: a later PAX field can overwrite an earlier
        field, just as a later layer can overwrite a file. Never skip that data.
        Sparse formats require a separate decoder and are deliberately refused.
        """
        audit, count, depth = self, 0, 0
        class FirstRead:
            def __init__(self, source, size, location):
                self.source, self.size, self.location, self.first = source, size, location, True
            def __getattr__(self, name): return getattr(self.source, name)
            def read(self, size=-1):
                value = self.source.read(size)
                if self.first:
                    self.first = False
                    need(len(value) >= self.size, "truncated_extended_header")
                    raw = value[:self.size]
                    text = raw.decode("utf-8", "replace")
                    if "\ufffd" in text: audit.finding(self.location, "binary_metadata_requires_review")
                    audit.text(self.location, text.replace("\0", "\n"))
                return value
        class BoundedTarInfo(tarfile.TarInfo):
            def _proc_member(self, archive):
                nonlocal count, depth
                count += 1
                need(count <= maximum, "physical_tar_header_limit", "INCOMPLETE")
                depth += 1
                need(depth <= 16, "extended_header_depth_limit", "INCOMPLETE")
                need(self.type != tarfile.GNUTYPE_SPARSE, "sparse_member_unsupported", "INCOMPLETE")
                previous = archive.fileobj
                try:
                    if self.type in (tarfile.XHDTYPE, tarfile.XGLTYPE, tarfile.SOLARIS_XHDTYPE,
                                     tarfile.GNUTYPE_LONGNAME, tarfile.GNUTYPE_LONGLINK):
                        need(0 <= self.size <= audit.limits.metadata_bytes, "extended_header_byte_limit", "INCOMPLETE")
                        archive.fileobj = FirstRead(previous, self.size, label + "/extended-header:" + str(count))
                    return super()._proc_member(archive)
                finally:
                    archive.fileobj = previous
                    depth -= 1
            def _proc_gnusparse_00(self, *args): raise Problem("sparse_member_unsupported", "INCOMPLETE")
            def _proc_gnusparse_01(self, *args): raise Problem("sparse_member_unsupported", "INCOMPLETE")
            def _proc_gnusparse_10(self, *args): raise Problem("sparse_member_unsupported", "INCOMPLETE")
        return BoundedTarInfo

    def content(self, label, source, size):
        need(size <= self.limits.file_bytes, "file_byte_limit", "INCOMPLETE")
        decoder = codecs.getincrementaldecoder("utf-8")("replace")
        pending, line, binary, total = "", 1, False, 0
        digest = hashlib.sha256()
        while True:
            block = source.read(CHUNK)
            total += len(block)
            need(total <= size, "member_size_changed")
            digest.update(block)
            text = decoder.decode(block, final=not block)
            binary |= re.search(r"[\x00-\x08\x0b\x0c\x0e-\x1f\x7f\ufffd]", text) is not None
            # Control bytes delimit binary printable runs. Text whitespace and
            # UTF-8 are retained; a very long run is explicitly incomplete.
            text = re.sub(r"[\x00-\x08\x0b\x0c\x0e-\x1f\x7f\ufffd]", "\n", text)
            pieces = (pending + text).split("\n")
            pending = pieces.pop()
            for value in pieces:
                need(len(value.encode("utf-8")) <= self.limits.text_run_bytes, "text_run_limit", "INCOMPLETE")
                self.text(label, value, line)
                line += 1
            need(len(pending.encode("utf-8")) <= self.limits.text_run_bytes, "text_run_limit", "INCOMPLETE")
            if not block:
                break
        need(total == size, "truncated_member")
        if pending:
            self.text(label, pending, line)
        if binary:
            self.report["counts"]["binary_files"] += 1
            self.finding(label, "binary_artifact_requires_review")
        return digest.hexdigest()

    def metadata(self, name, *, expected=None, size=None):
        need(name in self.members and self.members[name].isfile(), "missing_metadata_blob")
        member = self.members[name]
        need(member.size <= self.limits.metadata_bytes, "metadata_byte_limit", "INCOMPLETE")
        if size is not None:
            need(type(size) is int and size >= 0 and member.size == size, "descriptor_size_mismatch")
        with self.archive.extractfile(member) as source:
            raw = source.read(self.limits.metadata_bytes + 1)
        need(len(raw) == member.size, "truncated_metadata")
        digest = sha(raw)
        if expected:
            need(digest == expected, "metadata_digest_mismatch")
        self.used.add(name)
        value = json_load(raw)
        self.text(location("metadata", name), json.dumps(value, ensure_ascii=False, indent=2))
        return value, digest

    def blob(self, descriptor, accepted):
        need(isinstance(descriptor, dict) and descriptor.get("mediaType") in accepted, "unsupported_descriptor_type", "INCOMPLETE")
        need(valid_digest(descriptor.get("digest")) and type(descriptor.get("size")) is int and descriptor["size"] >= 0,
             "invalid_descriptor")
        name = "blobs/sha256/" + descriptor["digest"][7:]
        need(name in self.members and self.members[name].isfile(), "missing_referenced_blob")
        need(self.members[name].size == descriptor["size"], "descriptor_size_mismatch")
        return name

    def platform_check(self, value):
        need(isinstance(value, dict) and value.get("os") == "linux" and value.get("architecture") == self.platform.split("/")[1],
             "platform_mismatch")

    def graph(self):
        if "index.json" in self.members or "oci-layout" in self.members:
            need("index.json" in self.members and "oci-layout" in self.members, "incomplete_oci_layout")
            layout, _ = self.metadata("oci-layout")
            need(layout == {"imageLayoutVersion": "1.0.0"}, "unsupported_oci_layout", "INCOMPLETE")
            index, index_digest = self.metadata("index.json")
            need(isinstance(index, dict) and type(index.get("schemaVersion")) is int and index["schemaVersion"] == 2,
                 "invalid_oci_index")
            descriptors = index.get("manifests")
            need(isinstance(descriptors, list) and len(descriptors) == 1, "single_image_archive_required", "INCOMPLETE")
            selected = descriptors[0]
            name = self.blob(selected, MANIFESTS)
            if "platform" in selected:
                self.platform_check(selected["platform"])
            manifest, manifest_digest = self.metadata(name, expected=selected["digest"], size=selected["size"])
            need(isinstance(manifest, dict) and type(manifest.get("schemaVersion")) is int and manifest["schemaVersion"] == 2 and
                 manifest.get("mediaType", selected["mediaType"]) == selected["mediaType"], "invalid_image_manifest")
            if self.expected_manifest:
                need(manifest_digest == self.expected_manifest, "selected_manifest_mismatch")
            config_desc = manifest.get("config")
            config_name = self.blob(config_desc, CONFIGS)
            config, config_digest = self.metadata(config_name, expected=config_desc["digest"], size=config_desc["size"])
            layers = manifest.get("layers")
            need(isinstance(layers, list), "invalid_layer_list")
            need(len(layers) <= self.limits.outer_members, "layer_reference_limit", "INCOMPLETE")
            paths = [self.blob(item, LAYERS) for item in layers]
            specs = [(name, item["digest"], LAYERS[item["mediaType"]]) for name, item in zip(paths, layers)]
            self.report.update(format="oci", index_digest=index_digest, manifest_digest=manifest_digest)
            if "manifest.json" in self.members:
                compatibility, _ = self.metadata("manifest.json")
                need(isinstance(compatibility, list) and len(compatibility) == 1 and isinstance(compatibility[0], dict) and
                     compatibility[0].get("Config") == config_name and compatibility[0].get("Layers") == paths,
                     "docker_oci_manifest_disagreement")
                self.report["docker_save_manifest_consistent"] = True
        else:
            need("manifest.json" in self.members, "unsupported_archive_format", "INCOMPLETE")
            need(self.expected_manifest is None, "classic_archive_has_no_oci_manifest", "INCOMPLETE")
            manifests, _ = self.metadata("manifest.json")
            need(isinstance(manifests, list) and len(manifests) == 1 and isinstance(manifests[0], dict),
                 "single_image_archive_required", "INCOMPLETE")
            selected = manifests[0]
            need(isinstance(selected.get("Config"), str) and isinstance(selected.get("Layers"), list), "invalid_docker_manifest")
            need(len(selected["Layers"]) <= self.limits.outer_members, "layer_reference_limit", "INCOMPLETE")
            config_name = member_name(selected["Config"])
            config, config_digest = self.metadata(config_name)
            specs = []
            for name in selected["Layers"]:
                need(isinstance(name, str), "invalid_layer_path")
                name = member_name(name)
                need(name in self.members and self.members[name].isfile(), "missing_referenced_layer")
                # Classic Docker-save layers are uncompressed tar by contract.
                specs.append((name, None, "tar"))
                parent = PurePosixPath(name).parent
                for leaf in ("VERSION", "json"):
                    ancillary = (parent / leaf).as_posix()
                    if ancillary in self.members:
                        if leaf == "json": self.metadata(ancillary)
                        else:
                            item = self.members[ancillary]
                            need(item.isfile() and item.size <= self.limits.metadata_bytes, "invalid_legacy_layer_metadata")
                            with self.archive.extractfile(item) as source:
                                self.content(location("legacy-metadata", ancillary), source, item.size)
                            self.used.add(ancillary)
            if "repositories" in self.members: self.metadata("repositories")
            self.report["format"] = "docker-save"
        self.platform_check(config)
        if self.expected_config:
            need(config_digest == self.expected_config, "selected_config_mismatch")
        rootfs = config.get("rootfs")
        need(isinstance(rootfs, dict) and rootfs.get("type") == "layers" and isinstance(rootfs.get("diff_ids"), list) and
             len(rootfs["diff_ids"]) == len(specs) and all(valid_digest(item) for item in rootfs["diff_ids"]), "invalid_rootfs_diff_ids")
        configuration = config.get("config") or {}
        need(isinstance(configuration, dict), "invalid_image_config")
        env = configuration.get("Env") or []
        need(isinstance(env, list) and all(isinstance(value, str) for value in env), "invalid_image_environment")
        for index, value in enumerate(env):
            key, separator, content = value.partition("=")
            if separator and content and re.search(r"(?i)(?:api_key|token|password|secret|wifi_ssid)$", key):
                self.finding("config/env-entry:" + str(index), "image_environment_credential_requires_review")
        self.report.update(config_digest=config_digest, layer_count=len(specs))
        return [(name, compressed, kind, diff) for (name, compressed, kind), diff in zip(specs, rootfs["diff_ids"])]

    def charge(self, count):
        self.total_bytes += count
        need(self.total_bytes <= self.limits.total_uncompressed_bytes, "total_uncompressed_limit", "INCOMPLETE")

    def layer(self, index, name, expected_compressed, compression, expected_diff):
        item = self.members[name]
        need(item.size <= self.limits.archive_bytes, "compressed_layer_limit", "INCOMPLETE")
        self.used.add(name)
        record = {"index": index, "compression": compression, "compressed_bytes": item.size,
                  "expected_digest": expected_compressed, "expected_diff_id": expected_diff, "complete": False,
                  "entries": 0, "member_inventory_sha256": None}
        self.report["layers"].append(record)
        inventory = hashlib.sha256()
        with self.archive.extractfile(item) as source:
            compressed = HashReader(source, self.limits.archive_bytes)
            def scan_header(field, raw):
                record.setdefault("gzip_metadata", []).append({"field": field, "bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()})
                label = "layer:" + str(index) + "/gzip-" + field
                text = raw.decode("utf-8", "replace")
                if re.search(r"[\x00-\x08\x0b\x0c\x0e-\x1f\x7f\ufffd]", text):
                    self.finding(label, "binary_metadata_requires_review")
                self.text(label, text)
            decoded = SingleGzipReader(compressed, self.limits.metadata_bytes, scan_header) if compression == "gzip" else compressed
            stream = HashReader(decoded, self.limits.layer_bytes, self.charge)
            with tarfile.open(fileobj=stream, mode="r|", bufsize=CHUNK,
                              tarinfo=self.tar_info("layer:" + str(index), self.limits.layer_members)) as layer:
                for member in layer:
                    self.total_members += 1
                    need(self.total_members <= self.limits.layer_members, "layer_member_limit", "INCOMPLETE")
                    path = member_name(member.name, directory=member.isdir())
                    need(not member.sparse, "sparse_member_unsupported", "INCOMPLETE")
                    label = location("layer:" + str(index), path)
                    metadata = {"name": member.name, "linkname": member.linkname, "uname": member.uname,
                                "gname": member.gname, "pax": member.pax_headers}
                    text = json.dumps(metadata, ensure_ascii=False, sort_keys=True)
                    need(len(text.encode()) <= self.limits.metadata_bytes, "member_metadata_limit", "INCOMPLETE")
                    self.text(label + "/metadata", text)
                    entry = {"path_sha256": hashlib.sha256(path.encode()).hexdigest(), "size": member.size,
                             "type": member.type.hex(), "metadata_sha256": hashlib.sha256(text.encode()).hexdigest()}
                    if member.isfile():
                        self.report["counts"]["regular_files"] += 1
                        with layer.extractfile(member) as content:
                            entry["sha256"] = self.content(label, content, member.size)
                        if PurePosixPath(path).name.startswith(".wh."):
                            self.report["counts"]["whiteouts"] += 1
                    elif member.issym() or member.islnk():
                        self.report["counts"]["links"] += 1 # Metadata only. Never extract/read a target.
                        need(member.size == 0, "link_payload_unsupported", "INCOMPLETE")
                    elif not member.isdir():
                        self.report["counts"]["special_members"] += 1
                        self.finding(label, "special_member_requires_review")
                        need(member.size == 0, "special_payload_unsupported", "INCOMPLETE")
                    inventory.update(json.dumps(entry, sort_keys=True, separators=(",", ":")).encode() + b"\n")
                    record["entries"] += 1
                end = layer.offset
            # Stream mode can prefetch beyond the last entry. Check that
            # buffered and remaining trailer bytes contain only tar padding.
            need(not stream.since(end).strip(b"\0"), "nonzero_layer_trailer")
            while block := stream.read(CHUNK): need(not block.strip(b"\0"), "nonzero_layer_trailer")
            while compressed.read(CHUNK): pass
            record.update(digest="sha256:" + compressed.digest.hexdigest(), diff_id="sha256:" + stream.digest.hexdigest(),
                          uncompressed_bytes=stream.bytes, member_inventory_sha256=inventory.hexdigest())
            need(compressed.bytes == item.size, "compressed_size_mismatch")
            if expected_compressed: need(record["digest"] == expected_compressed, "layer_digest_mismatch")
            need(record["diff_id"] == expected_diff, "layer_diff_id_mismatch")
            record["complete"] = True

    def run(self, path):
        fd = None
        try:
            path = Path(path)
            need(not path.is_symlink(), "archive_symlink_forbidden")
            fd = open_without_links(path)
            before = os.fstat(fd)
            need(stat.S_ISREG(before.st_mode) and before.st_nlink == 1, "archive_not_unlinked_regular_file")
            need(before.st_size <= self.limits.archive_bytes, "archive_byte_limit", "INCOMPLETE")
            with os.fdopen(fd, "rb") as file:
                fd = None
                digest = hashlib.sha256()
                while block := file.read(CHUNK): digest.update(block)
                self.report.update(archive_sha256=digest.hexdigest(), archive_bytes=before.st_size)
                file.seek(0)
                with tarfile.open(fileobj=file, mode="r:", tarinfo=self.tar_info("archive", self.limits.outer_members)) as archive:
                    self.archive, self.members = archive, {}
                    outer_count = 0
                    for item in archive:
                        outer_count += 1
                        need(outer_count <= self.limits.outer_members, "outer_member_limit", "INCOMPLETE")
                        name = member_name(item.name, directory=item.isdir())
                        need(name not in self.members, "duplicate_outer_member")
                        need(item.isfile() or item.isdir(), "outer_link_or_special_member")
                        self.text(location("archive-header", name), json.dumps(
                            {"name": item.name, "uname": item.uname, "gname": item.gname, "pax": item.pax_headers},
                            ensure_ascii=False, sort_keys=True))
                        self.members[name] = item
                    outer_end = archive.offset
                    file.seek(outer_end)
                    while block := file.read(CHUNK): need(not block.strip(b"\0"), "nonzero_archive_trailer")
                    specs = self.graph()
                    for index, spec in enumerate(specs): self.layer(index, *spec)
                    need(not {name for name, item in self.members.items() if item.isfile()} - self.used,
                         "unreferenced_archive_content")
                    self.report["outer_members"] = outer_count
                after = os.fstat(file.fileno())
                need((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns) ==
                     (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns), "archive_changed_during_read")
            self.report["complete"] = True
        except Problem as error:
            self.report["problems"].append({"kind": error.kind, "code": error.code})
        except (OSError, ValueError, EOFError, tarfile.TarError, RecursionError, zlib.error):
            self.report["problems"].append({"kind": "FAIL", "code": "archive_or_compression_parse_failed"})
        finally:
            if fd is not None: os.close(fd)
        self.report["scanned_uncompressed_bytes"] = self.total_bytes
        self.report["status"] = ("FAIL" if any(item["kind"] == "FAIL" for item in self.report["problems"]) else
                                 "INCOMPLETE" if not self.report["complete"] else
                                 "REVIEW_REQUIRED" if self.report["findings"] else "PASS_SCOPED")
        return self.report


def write_report(path, report):
    path = Path(path).absolute()
    private = ROOT / "private"
    need(not path.is_symlink() and path.resolve().is_relative_to(private.resolve()), "report_must_remain_private")
    for parent in path.parents:
        if parent == ROOT: break
        need(not parent.is_symlink(), "report_parent_symlink_forbidden")
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".image-audit-", dir=path.parent)
    try:
        os.fchmod(fd, 0o600)
        with os.fdopen(fd, "w") as stream:
            json.dump(report, stream, ensure_ascii=False, indent=2); stream.write("\n")
            stream.flush(); os.fsync(stream.fileno())
        os.replace(temporary, path)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try: os.fsync(directory)
        finally: os.close(directory)
    finally:
        if os.path.exists(temporary): os.unlink(temporary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--platform", choices=("linux/amd64", "linux/arm64"), required=True)
    parser.add_argument("--expected-config")
    parser.add_argument("--expected-manifest")
    parser.add_argument("--report", type=Path, required=True)
    for name, default in asdict(Limits()).items():
        parser.add_argument("--max-" + name.replace("_", "-"), dest=name, type=int, default=default)
    args = parser.parse_args()
    try:
        report = Audit(args.platform, args.expected_config, args.expected_manifest,
                       Limits(**{key: getattr(args, key) for key in asdict(Limits())})).run(args.archive)
        report["scanner_sha256"] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
        report["text_scanner_sha256"] = hashlib.sha256((ROOT / "scripts/export_public_source.py").read_bytes()).hexdigest()
        write_report(args.report, report)
        print(json.dumps({"status": report["status"], "complete": report["complete"], "findings": len(report["findings"]),
                          "problem_codes": [item["code"] for item in report["problems"]],
                          "archive_sha256": report.get("archive_sha256"), "publication_ready": False}))
        return {"PASS_SCOPED": 0, "REVIEW_REQUIRED": 1, "INCOMPLETE": 2, "FAIL": 2}[report["status"]]
    except Problem as error:
        print(json.dumps({"status": error.kind, "problem_codes": [error.code]})); return 2
    except OSError:
        print(json.dumps({"status": "FAIL", "problem_codes": ["report_io_failed"]})); return 2


if __name__ == "__main__":
    raise SystemExit(main())
