import contextlib
import io
import os
import pathlib
import sqlite3
import sys
import tarfile
import tempfile
import types
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from ops import meow_relay_maint as maint


class MaintenanceAccessTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def bundle(self, members):
        path = self.root / "release.tar.gz"
        with tarfile.open(path, "w:gz") as archive:
            for name, value, kind in members:
                info = tarfile.TarInfo(name)
                if kind == "file":
                    data = value.encode()
                    info.size = len(data)
                    archive.addfile(info, io.BytesIO(data))
                elif kind == "symlink":
                    info.type = tarfile.SYMTYPE
                    info.linkname = value
                    archive.addfile(info)
        path.chmod(0o600)
        return path

    def owner(self):
        return mock.patch.object(
            maint.pwd,
            "getpwnam",
            return_value=types.SimpleNamespace(pw_uid=os.getuid()),
        )

    def test_safe_release_extracts_only_expected_files(self):
        members = [(name, f"contents of {name}", "file") for name in maint.REQUIRED_RELEASE_FILES]
        bundle = self.bundle(members)
        destination = self.root / "out"
        destination.mkdir()
        with self.owner():
            digest = maint.validate_bundle(bundle, destination)
        self.assertEqual(digest, maint.sha256(bundle))
        self.assertEqual(set(path.name for path in destination.iterdir()), maint.REQUIRED_RELEASE_FILES)

    def test_traversal_symlink_unknown_and_duplicate_are_rejected(self):
        required = [(name, "x", "file") for name in maint.REQUIRED_RELEASE_FILES]
        cases = [
            required + [("../app.py", "x", "file")],
            required + [("pyproject.toml", "app.py", "symlink")],
            required + [("unexpected.env", "secret", "file")],
            required + [("app.py", "duplicate", "file")],
        ]
        for index, members in enumerate(cases):
            with self.subTest(index=index):
                bundle = self.bundle(members)
                destination = self.root / f"out-{index}"
                destination.mkdir()
                with self.owner(), self.assertRaises(maint.MaintenanceError):
                    maint.validate_bundle(bundle, destination)
                bundle.unlink()

    def test_group_readable_bundle_is_rejected(self):
        members = [(name, "x", "file") for name in maint.REQUIRED_RELEASE_FILES]
        bundle = self.bundle(members)
        bundle.chmod(0o640)
        destination = self.root / "out"
        destination.mkdir()
        with self.owner(), self.assertRaises(maint.MaintenanceError):
            maint.validate_bundle(bundle, destination)

    def test_legacy_deploy_rejects_before_any_uploaded_code_or_root_subprocess(self):
        # Even an allowlisted Python test file may contain arbitrary code.
        marker = self.root / "executed"
        payload = f"import pathlib; pathlib.Path({str(marker)!r}).write_text('executed')"
        members = [(name, payload, "file") for name in maint.REQUIRED_RELEASE_FILES]
        bundle = self.bundle(members)
        with mock.patch.object(maint, "INCOMING", bundle), \
                mock.patch.object(maint, "run") as run, \
                mock.patch.object(maint, "validate_bundle") as extract:
            with self.assertRaisesRegex(maint.MaintenanceError, "deployment disabled"):
                maint.command_deploy()
            run.assert_not_called()
            extract.assert_not_called()
        self.assertFalse(marker.exists())
        self.assertTrue(bundle.exists())

    def test_legacy_rollback_cannot_restore_unreviewed_uploaded_code(self):
        with mock.patch.object(maint, "run") as run:
            with self.assertRaisesRegex(maint.MaintenanceError, "rollback disabled"):
                maint.command_rollback("20261003T000000Z-abcdef123456")
            run.assert_not_called()

    def test_memo_query_never_outputs_transcript_or_request_ids(self):
        database = self.root / "state.sqlite3"
        connection = sqlite3.connect(database)
        connection.execute(
            "create table memos(device_id text,recording_id integer,duration_ms integer,"
            "status text,error text,updated_at integer,qwen_request_id text,"
            "transcript_sha256 text,transcript text)"
        )
        connection.execute(
            "insert into memos values(?,?,?,?,?,?,?,?,?)",
            ("device-1", 7, 1234, "sent", None, 42, "request-a,request-b", "hash", "PRIVATE WORDS"),
        )
        connection.commit()
        connection.close()
        output = io.StringIO()
        with mock.patch.object(maint, "DB_PATH", database), contextlib.redirect_stdout(output):
            maint.command_memo("device-1", 7)
        value = output.getvalue()
        self.assertNotIn("PRIVATE WORDS", value)
        self.assertNotIn("request-a", value)
        self.assertIn('"qwen_request_count": 2', value)


if __name__ == "__main__":
    unittest.main()
