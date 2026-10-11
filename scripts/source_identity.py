"""Build provenance for own Git checkouts and source archives, never ancestors."""
from pathlib import Path
import hashlib
import json
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def source_identity(root):
    root = Path(root).resolve()
    top = subprocess.run(['git', 'rev-parse', '--show-toplevel'], cwd=root,
                         capture_output=True, text=True)
    if top.returncode == 0 and Path(top.stdout.strip()).resolve() == root:
        head = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=root,
                              capture_output=True, text=True)
        if head.returncode == 0:
            return head.stdout.strip()
    # Only build sources/tooling contribute; editing support documentation does
    # not change the app version. Actual full inputs remain in build receipts.
    paths = []
    for directory in ('main', 'components'):
        paths.extend(p for p in (root / directory).rglob('*')
                     if p.is_file() and '__pycache__' not in p.parts)
    for pattern in ('CMakeLists.txt', 'sdkconfig*.defaults', 'partitions*.csv',
                    '*lock*.json', 'dependencies.lock', 'requirements-*.txt',
                    'scripts/env.sh', 'scripts/build*.sh', 'scripts/build_receipt.py',
                    'scripts/firmware_bundle.py', 'scripts/source_identity.py'):
        paths.extend(root.glob(pattern))
    records = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
               for p in sorted(set(paths)) if p.is_file()}
    if not records:
        raise ValueError('build source identity unavailable')
    return 'source-sha256:' + hashlib.sha256(json.dumps(records, sort_keys=True).encode()).hexdigest()


if __name__ == '__main__':
    identity = source_identity(ROOT)
    print('src-' + identity.split(':', 1)[1][:12] if identity.startswith('source-sha256:') else identity[:12])
