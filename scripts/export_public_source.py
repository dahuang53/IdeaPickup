#!/usr/bin/env python3
"""Create a reviewed source-only candidate without copying private Git history.

Scanning is heuristic; publication permission remains a separate owner decision.
This prepares only a local candidate. No upload,
Git push, history rewrite, secret value printing or automatic redaction occurs.
"""
from __future__ import annotations
import argparse
import gzip
import hashlib
import io
import ipaddress
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import tarfile
import tempfile
from urllib.parse import urlsplit
import zipfile

ROOT = Path(__file__).resolve().parents[1]
TEXT_SUFFIXES = {".c", ".h", ".cpp", ".cc", ".py", ".sh", ".md", ".txt", ".json", ".yaml", ".yml",
                 ".toml", ".lock", ".html", ".css", ".js", ".cjs", ".csv", ".example", ".conf"}
SPECIAL_NAMES = {"CMakeLists.txt", "Kconfig", "Kconfig.uac", "LICENSE", "UPSTREAM_COMMIT", "Dockerfile",
                 ".python-version", "relay-admin", "Kconfig.projbuild"}
TEXT_FILES = {"scripts/esptool-protected.cfg"}
ANIMATION_SHA256 = {
    "emotion_blink1_284_126.aaf": "aafdb404605114479346b98aba24880350905e4dfc2b7aa05a77bf11a2cfc843",
    "emotion_happy_284_126.aaf": "e7f38a2970a1b6ecfddb5b847b07a57503dfed05f6ed36c0311b2d750c5d487a",
    "emotion_blink_slow_284_126.aaf": "7ea7b322ee1dc859dbbc90e40eeb90dbfde036c5acbf6d794d90b7bcc546713a",
    "emotion_sleep_284_126.aaf": "920424e4fc1e265fb033482121997f979581747eb622dfff7ffcb933bf262cfd",
    "emotion_blink_fast_284_126.aaf": "65a9164db49f3cfbec1426031843cc2584be43c184772581e510590b79bada00",
    "emotion_blink_cardless_284_126.aaf": "e921695402b8162dcf27ffb2faca432000204cf9956fa88951d57449fe48da6e",
    "emotion_sad_284_126.aaf": "4352279e0e925b60a4f7ca4bb5935516f69b438112115e1231aa727e28c19488",
}
LICENSE_SHA256 = {
    'licenses/distlib-LICENSE.txt': '808e10c8a6ab8deb149ff9b3fb19f447a808094606d712a9ca57fead3552599d',
    'licenses/Python-3.11-LICENSE.txt': '3b2f81fe21d181c499c59a256c8e1968455d6689d269aa85373bfb6af41da3bf',

    "licenses/Apache-2.0.txt": "cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30",
    "licenses/xiaozhi-MIT.txt": "0a5a839033bfe18fe75d32b50d9d028912cf876f69ef59c2791aeb2971335d05",
    "LICENSE": "cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30",
    "main/assets/cat/LICENSE": "cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30",
    "assets/fonts/OFL.txt": "6a73f9541c2de74158c0e7cf6b0a58ef774f5a780bf191f2d7ec9cc53efe2bf2",
    "components/esp-wifi-connect/LICENSE": "35a84cad890590c2aa8421fa48db5d2fb7e5fc938f8f31ca00dc349eb5a3d5b7",
    "components/usb_device_uac/license.txt": "cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30",
}
REQUIRED_FILES = {"CMakeLists.txt", "main/CMakeLists.txt", "main/idf_component.yml", "LICENSE", "README.md",
                  "THIRD_PARTY_NOTICES.md", "CONTRIBUTING.md", "SECURITY.md", "CHANGELOG.md", "dependencies.lock",
                  "main/assets/usb_no_host.pcm", "docs/DEPLOYMENT.md", "docs/PUBLIC_RELEASE.md",
                  *TEXT_FILES, *LICENSE_SHA256}
OFFICIAL_HOSTS = {"github.com", "raw.githubusercontent.com", "api.github.com", "githubusercontent.com",
                  "espressif.com", "readthedocs.io", "lvgl.io", "python.org", "pythonhosted.org", "pypi.org",
                  "pypi.python.org", "astral.sh", "npmjs.org", "npmjs.com", "apache.org", "opensource.org",
                  "sil.org", "scripts.sil.org", "adobe.com", "unicode.org", "gnu.org", "w3.org", "ietf.org",
                  "json-schema.org", "openai.com", "dashscope.aliyuncs.com", "ld246.com", "nginx.org",
                  "docker.com", "docker.io", "debian.org", "freedesktop.org", "opus-codec.org", "ffmpeg.org",
                  "example.com", "example.org", "example.net", "localhost", "ghcr.io", "spotpear.com"}
PATTERNS = {
    "private_key": re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH |DSA )?PRIVATE KEY-----"),
    "personal_absolute_path": re.compile(r"(?:/Users/|/home/)[A-Za-z0-9_.-]+/|[A-Za-z]:\\Users\\[^\\\s]+\\"),
    "personal_serial_device": re.compile(r"/dev/(?:cu|tty)\.(?:usbmodem[0-9]{3,}|usbserial[A-Za-z0-9_-]+)"),
    "credential_token": re.compile(r"\b(?:sk-[A-Za-z0-9_-]{20,}|gh[pousr]_[A-Za-z0-9]{20,}|AKIA[A-Z0-9]{16})\b"),
    "hardware_address": re.compile(r"(?<![A-Za-z0-9])(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}(?![A-Za-z0-9])"),
    "email_address": re.compile(r"\b[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}\b"),
}
SECRET_ASSIGNMENT = re.compile(r"(?i)(?:[a-z_]*api_key|[a-z_]*token|[a-z_]*password|wifi_ssid|[a-z_]*secret)\s*[\"']?\s*[:=]\s*[\"']([^\"'\s]{16,})[\"']")
URL_PATTERN = re.compile(r"https?://[^\s\"'<>`\\]+")
MAX_SOURCE_FILE = 16 * 1024 * 1024
# Exact reviewed synthetic literals, never path-wide exclusions. A new literal
# in these files is still scanned. Real private addresses are not generally OK.
REVIEWED_URL_FIXTURES = {
    "tests/test_network_config.c": {"10.0.0.1:8443/base", "exa", "-example.com",
        "example..com", "999.1.1.1", "01.1.1.1", "[:::1]", "[2001:db8]", "[::1]bad"},
    "server-relay/test_config_broker.py": {"192.168.1.2:8443/relay", "169.254.169.254/latest", "memo.local:8443"},
    "server-relay/test_identity.py": {"example.com\\\\path", "bad_host", "[fe80::1%en0]"},
    "components/esp-wifi-connect/assets/wifi_configuration.html": {"你的域名/可选路径"},
}
REVIEWED_URL_FIXTURES = {path: {"https" + "://" + value for value in values}
                         for path, values in REVIEWED_URL_FIXTURES.items()}
REVIEWED_URL_FIXTURES["deploy/nginx-container.conf"] = {"http" + "://relay:8765;"}
# Frozen approved visual reference: this fictional prototype value is normalized
# by the existing browser regression; it is never a runtime provider destination.
REVIEWED_URL_FIXTURES["docs/design/admin-approved/index.html"] = {"https" + "://api.b3logfile.com/siyuan/"}
REVIEWED_TOKEN_FIXTURES = {
    "tests/test_qwen_transcribe.py": {"sk" + "-test-secret-long-enough"},
    "server-relay/test_identity.py": {"credential" + "-should-not-be-logged"},
}


ROOT_FILES = {'.gitignore','.dockerignore','CMakeLists.txt','README.md','LICENSE','THIRD_PARTY_NOTICES.md','CONTRIBUTING.md','SECURITY.md','CHANGELOG.md','dependencies.lock','tools.lock.json','requirements-host.lock.txt','requirements-idf.lock.txt','partitions.csv','partitions.vocat16.csv','sdkconfig.defaults','sdkconfig.spotpear.defaults','sdkconfig.vocat16.defaults'}
SOURCE_ROOTS = ('main','components/esp-wifi-connect','components/usb_device_uac','assets/fonts','licenses','server-relay','scripts','tests','deploy','.github/workflows')
PUBLIC_DOCS = {'docs/DEPLOYMENT.md','docs/PUBLIC_RELEASE.md','docs/DEVICE_INSTALL.md','docs/FIRMWARE_STORAGE.md','docs/TESTING.md','docs/SUPPORT.md','docs/design/admin-approved/index.html'}
EXCLUDED = {'__pycache__','.venv','.venv-relay','node_modules','.DS_Store'}
EXCLUDED_FILES = {
 'scripts/acoustic_command_matrix.py','scripts/acoustic_usb_test.py','scripts/acoustic_voice_test.py','scripts/acoustic_wake_recall.py','scripts/check_device_pages.py','scripts/cloud_relay_sync.py','scripts/export_recordings.py','scripts/hardware_batch.py','scripts/inspect_cat_colors.py','scripts/qwen_transcribe.py','scripts/replay_voice_recording.py','scripts/reset_capture.py','scripts/siyuan_sync.py','scripts/verify_cardless_capture.py','scripts/verify_cardless_faults.py','scripts/verify_playback_pause.py','scripts/setup_sync.sh','scripts/watch_sync.command','scripts/watch_cloud_sync.command',
 'server-relay/ops/install_maintenance_access.sh','server-relay/ops/README.md','tests/test_qwen_transcribe.py','tests/test_cloud_relay_sync.py','tests/test_siyuan_sync.py','tests/display_color_probe.c','server-relay/meow-voice-memo-relay.service','server-relay/nginx-location.conf'}
REQUIRED_FILES = {'README.md','LICENSE','THIRD_PARTY_NOTICES.md','CONTRIBUTING.md','SECURITY.md','CMakeLists.txt','main/CMakeLists.txt','scripts/source_identity.py','scripts/test_all.sh','scripts/firmware_bundle.py','scripts/relay_deploy.sh','server-relay/Dockerfile','deploy/compose.yaml','docs/DEPLOYMENT.md','docs/DEVICE_INSTALL.md','docs/SUPPORT.md','docs/design/admin-approved/index.html','main/assets/usb_no_host.pcm',*LICENSE_SHA256}


# Exact synthetic negative cases / placeholder text, not a path-wide exemption.
REVIEWED_URL_FIXTURES.setdefault('tests/test_network_config.c',set()).update({'https'+"://"+'01.2.3.4','https'+"://"+'192.168.1.20:9443/memo','https'+"://"+'256.2.3.4','https'+"://"+'[1:2:3]','https'+"://"+'x','https'+"://"+'x/','https'+"://"+'x/a#part','https'+"://"+'x/a%2fb','https'+"://"+'x/a/%2e%2e/b','https'+"://"+'x/a/../b','https'+"://"+'x/a//b','https'+"://"+'x/a?b=1','https'+"://"+'x/hello%00','https'+"://"+'x/hello%20','https'+"://"+'x/hello%5c','https'+"://"+'x:0','https'+"://"+'x:0443','https'+"://"+'x:65536'})
REVIEWED_URL_FIXTURES.setdefault('components/esp-wifi-connect/assets/wifi_configuration.html',set()).update({'https'+"://"+'你的服务器/路径'})
REVIEWED_URL_FIXTURES.setdefault('tests/deployment_http_client.py',set()).update({'http'+"://"+'relay:8765'})
REVIEWED_TOKEN_FIXTURES.setdefault('tests/test_firmware_pairing.py',set()).update({'synthetic-password-123'})
REVIEWED_TOKEN_FIXTURES.setdefault('server-relay/test_admin.py',set()).update({'Synthetic-only-password!'})
REVIEWED_TOKEN_FIXTURES.setdefault('server-relay/test_runtime_http.py',set()).update({'synthetic-password-123'})
REVIEWED_TOKEN_FIXTURES.setdefault('tests/test_network_config.c',set()).update({'0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef'})


REVIEWED_TOKEN_FIXTURES.setdefault('scripts/test_container_acceptance.py',set()).update({'Synthetic-password-2026'})
REVIEWED_TOKEN_FIXTURES.setdefault('tests/deployment_http_client.py',set()).update({'Synthetic-password-2026'})
REVIEWED_TOKEN_FIXTURES.setdefault('tests/relay_admin_fixture.py',set()).update({'synthetic-password-123'})

def finding(path, category, line=None):
    value = {"path": str(path), "category": category}
    if line is not None:
        value["line"] = line
    return value


def included(path):
    if any(part in EXCLUDED or part in {"private", ".git"} for part in path.parts):
        return False
    text = path.as_posix()
    if text in EXCLUDED_FILES:
        return False
    return text in ROOT_FILES or text in PUBLIC_DOCS or text.startswith("docs/contracts/") or any(text.startswith(root + "/") for root in SOURCE_ROOTS)


def approved_url(url):
    try:
        # A C/JSON escaped closing quote leaves a trailing backslash in this
        # textual URL match. Remove that delimiter only, not internal slashes.
        host = urlsplit(url.rstrip(".,);}").rstrip("\\")).hostname
    except ValueError:
        return False
    if not host:
        return False
    try:
        address = ipaddress.ip_address(host)
        if address.version == 6 and address.ipv4_mapped:
            address = address.ipv4_mapped
        return (address.is_loopback or str(address) == "192.168.4.1"
                or any(address in ipaddress.ip_network(network) for network in
                       ("192.0.2.0/24", "198.51.100.0/24", "203.0.113.0/24", "2001:db8::/32")))
    except ValueError:
        pass
    return (host.endswith((".invalid", ".test", ".example")) or host in OFFICIAL_HOSTS
            or any(host.endswith("." + suffix) for suffix in OFFICIAL_HOSTS))


def scan_text(name, text):
    issues = []
    for number, line in enumerate(text.splitlines(), 1):
        for category, pattern in PATTERNS.items():
            for match in pattern.finditer(line):
                value = match.group()
                if category == "email_address" and (value.endswith(".invalid") or name in LICENSE_SHA256):
                    continue
                if category == "hardware_address" and value in {"01:02:03:04:05:06", "00:00:00:00:00:00", "ff:ff:ff:ff:ff:ff"} and (name.startswith("tests/") or name == "scripts/export_public_source.py"):
                    continue
                if category == "credential_token" and value in REVIEWED_TOKEN_FIXTURES.get(str(name), set()):
                    continue
                if category == "hardware_address" and (value in {"00:00:00:00:00:00", "ff:ff:ff:ff:ff:ff"}):
                    continue
                if category == "email_address" and value.rsplit("@", 1)[1].endswith(("example.com", "example.org", ".invalid", ".test", ".example")):
                    continue
                if category == "personal_absolute_path" and name in {"server-relay/ops/meow_relay_maint.py", "scripts/export_public_source.py"} and value == "/home/meowmaint/":
                    continue
                issues.append(finding(name, category, number))
        for match in SECRET_ASSIGNMENT.finditer(line):
            value = match.group(1)
            if value not in REVIEWED_TOKEN_FIXTURES.get(str(name), set()):
                issues.append(finding(name, "literal_credential_or_network_secret", number))
        for match in URL_PATTERN.finditer(line):
            if name in LICENSE_SHA256:  # Exact immutable upstream legal text, not runtime destinations.
                continue
            if match.group() not in REVIEWED_URL_FIXTURES.get(str(name), set()) and not approved_url(match.group()):
                issues.append(finding(name, "unreviewed_network_destination", number))
    return issues


def safe_member(name):
    path = PurePosixPath(name)
    return (bool(name) and not path.is_absolute() and
            all(part not in {'', '.', '..'} for part in path.parts) and '\\' not in name)


def read_regular(root, name):
    """Open every path component without following links, then bound the read."""
    if not safe_member(name):
        raise ValueError("unsafe source path")
    directory = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        parts = PurePosixPath(name).parts
        for part in parts[:-1]:
            next_directory = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=directory)
            os.close(directory)
            directory = next_directory
        descriptor = os.open(parts[-1], os.O_RDONLY | os.O_NOFOLLOW, dir_fd=directory)
        try:
            info = os.fstat(descriptor)
            if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_size > MAX_SOURCE_FILE:
                raise ValueError("source is not a bounded unlinked regular file")
            with os.fdopen(descriptor, "rb", closefd=False) as stream:
                content = stream.read(MAX_SOURCE_FILE + 1)
            if len(content) > MAX_SOURCE_FILE:
                raise ValueError("source changed during read")
            return info, content
        finally:
            os.close(descriptor)
    finally:
        os.close(directory)




def candidate(root):
    root = Path(root).resolve()
    records, issues = [], []
    for directory, folders, filenames in os.walk(root, followlinks=False):
        relative = Path(directory).relative_to(root)
        # Reach the single approved reference without traversing other design drafts.
        folders[:] = sorted(
            f for f in folders
            if f not in EXCLUDED
            and f not in {'private','.git','.tools','artifacts','evidence','managed_components'}
            and (f != 'design' or (relative/f).as_posix() == 'docs/design')
            and (relative.as_posix() != 'docs/design' or f == 'admin-approved')
            and relative.as_posix() != 'docs/design/admin-approved'
            and not f.startswith('build'))
        for f in list(folders):
            if (Path(directory)/f).is_symlink():
                if included(relative/f/'placeholder.py'): issues.append(finding(relative/f,'symlink_forbidden'))
                folders.remove(f)
        for f in sorted(filenames):
            path=relative/f; name=path.as_posix()
            if not included(path): continue
            try:
                info,data=read_regular(root,name)
            except (OSError,ValueError):
                issues.append(finding(name,'source_changed_or_unsafe'));continue
            digest=hashlib.sha256(data).hexdigest()
            records.append({'path':name,'sha256':digest,'size':len(data),'executable':bool(info.st_mode&0o111)})
            if name in LICENSE_SHA256 and digest != LICENSE_SHA256[name]: issues.append(finding(name,'upstream_license_changed'))
            if path.suffix in {'.aaf','.pcm'}:
                expected=ANIMATION_SHA256.get(path.name) if path.parent.as_posix()=='main/assets/cat' else '367fbdbaa3b98b5d66aadb806c5678bb80c8a8723cc6b1cbdbc544d6effddba3' if name=='main/assets/usb_no_host.pcm' else None
                if digest != expected: issues.append(finding(name,'unrecognized_asset'))
                continue
            if path.suffix not in TEXT_SUFFIXES and path.name not in SPECIAL_NAMES and name not in ROOT_FILES and name not in TEXT_FILES: issues.append(finding(name,'file_type_not_allowlisted'));continue
            try: text=data.decode('utf-8')
            except UnicodeError: issues.append(finding(name,'non_utf8_source'));continue
            if '\0' in text: issues.append(finding(name,'binary_in_text_path'));continue
            issues.extend(scan_text(name,text))
    records.sort(key=lambda item:item['path'])
    for missing in sorted(REQUIRED_FILES-{x['path'] for x in records}): issues.append(finding(missing,'required_public_source_missing'))
    return {'version':1,'tree_sha256':hashlib.sha256(json.dumps(records,sort_keys=True,separators=(',',':')).encode()).hexdigest(),'files':records,'findings':issues,'source_only':True,'includes_git_history':False,'publication_authorized':False}


def export(root,destination,report):
    root,destination=Path(root).resolve(),Path(destination).resolve()
    if report['findings']: raise ValueError('source findings must be resolved before candidate export')
    if not destination.is_relative_to(root/'private') or destination.exists(): raise ValueError('candidate must be a new project private directory')
    destination.parent.mkdir(mode=0o700,parents=True,exist_ok=True)
    temporary=Path(tempfile.mkdtemp(prefix='.source-candidate-',dir=destination.parent))
    try:
        for item in report['files']:
            info,data=read_regular(root,item['path'])
            if hashlib.sha256(data).hexdigest()!=item['sha256'] or bool(info.st_mode&0o111)!=item['executable']: raise ValueError('source changed after scan')
            target=temporary/item['path'];target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(data);target.chmod(0o755 if item['executable'] else 0o644)
        (temporary/'PUBLIC_SOURCE_MANIFEST.json').write_text(json.dumps({k:v for k,v in report.items() if k!='findings'},indent=2)+'\n')
        os.rename(temporary,destination)
    except BaseException:
        shutil.rmtree(temporary);raise


def main():
    p=argparse.ArgumentParser(description='Prepare source-only private candidate; never publish or grant approval')
    p.add_argument('--root',type=Path,default=ROOT);p.add_argument('--output',type=Path);p.add_argument('--report',type=Path,required=True);a=p.parse_args()
    try:
        root=a.root.resolve();report=a.report.resolve()
        if not report.is_relative_to(root/'private'): raise ValueError('scan report must remain in project private')
        value=candidate(root);report.parent.mkdir(mode=0o700,parents=True,exist_ok=True);report.write_text(json.dumps(value,indent=2)+'\n');report.chmod(0o600)
        if a.output: export(root,a.output,value)
        print(json.dumps({'tree_sha256':value['tree_sha256'],'files':len(value['files']),'findings':value['findings'],'publication_authorized':False},indent=2));return int(bool(value['findings']))
    except (ValueError,OSError):
        print('Source export failed; inspect the protected report',file=__import__('sys').stderr);return 2


if __name__=='__main__': raise SystemExit(main())
