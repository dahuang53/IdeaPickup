"""Read current DB and committed WAL only after the deployment writer stops.
The real mounted files stay read-only; existing image readers inspect a private
volatile copy, deleted on exit. No legacy/PASS input or database restoration.
"""
import json,sys,sqlite3,shutil,tempfile,os
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"server-relay"))
from relay.jobs import STORAGE_MANIFEST
from relay.runtime import Runtime
from relay.identity import IdentityStore

def check(path):
    path=Path(path)
    if not path.exists():
        return True
    with tempfile.TemporaryDirectory(prefix="current-schema-") as folder:
        snapshot=Path(folder)/"relay.db"
        shutil.copyfile(path,snapshot);os.chmod(snapshot,0o600)
        wal=path.with_name(path.name+"-wal")
        if wal.exists():
            copied=snapshot.with_name(snapshot.name+"-wal")
            shutil.copyfile(wal,copied);os.chmod(copied,0o600)
        # Do not copy stale SHM. SQLite builds its WAL index on this disposable
        # writable directory, so committed rows are seen without touching source.
        IdentityStore.validate_existing(snapshot)
        runtime=Runtime.__new__(Runtime);runtime.path=snapshot
        return runtime._startup_schema()["compatible"]

if __name__ == "__main__":
    try:
        compatible=check(Path("/var/lib/voice-memo/relay.db"))
    except (OSError,ValueError,RuntimeError,sqlite3.Error):
        compatible=False
    print(json.dumps({"compatible":compatible,"storage_manifest":STORAGE_MANIFEST}))
    raise SystemExit(0 if compatible else 1)
