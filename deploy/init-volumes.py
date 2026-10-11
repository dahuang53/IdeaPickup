"""Prepare only four fixed, empty mounts; validate existing mounts untouched."""
import os
from pathlib import Path
import stat

MOUNTS = (
    ("/var/lib/voice-memo", 10001, 10001, 0o700),
    ("/var/lib/voice-memo-work", 10001, 10001, 0o700),
    ("/var/lib/voice-memo-secrets", 0, 0, 0o700),
    ("/run/voice-memo", 0, 10001, 0o750),
)


def initialize():
    if os.geteuid() != 0:
        raise RuntimeError("volume preparation requires actual root UID")
    os.umask(0o077)
    for name, uid, gid, mode in MOUNTS:
        path = Path(name)
        info = path.lstat()
        if not stat.S_ISDIR(info.st_mode) or path.is_symlink():
            raise RuntimeError("invalid mount type")
        if (info.st_uid, info.st_gid, stat.S_IMODE(info.st_mode)) == (uid, gid, mode):
            continue
        # Never recursively chown, chmod or repair existing deployment data.
        if any(path.iterdir()):
            raise RuntimeError("nonempty mount has unexpected ownership or mode")
        os.chmod(path, mode, follow_symlinks=False)
        os.chown(path, uid, gid, follow_symlinks=False)
        info = path.lstat()
        if (info.st_uid, info.st_gid, stat.S_IMODE(info.st_mode)) != (uid, gid, mode):
            raise RuntimeError("mount permissions did not persist")
    print("volume permissions verified")


if __name__ == "__main__":
    try:
        initialize()
    except (OSError, RuntimeError):
        raise SystemExit("volume initialization refused; inspect deployment mount ownership") from None
