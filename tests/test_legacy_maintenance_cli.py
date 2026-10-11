"""Legacy publish paths stop locally; SSH is always a synthetic executable."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


PROJECT = Path(__file__).resolve().parents[1]


class LegacyMaintenanceCliTests(unittest.TestCase):
    def setUp(self):
        temporary_root = PROJECT / 'private/route1-r01/maintenance-cli-tests'
        temporary_root.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.temporary = tempfile.TemporaryDirectory(dir=temporary_root)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / 'scripts').mkdir()
        self.wrapper = self.root / 'scripts/server_maint.sh'
        shutil.copyfile(PROJECT / 'scripts/server_maint.sh', self.wrapper)
        fake_bin = self.root / 'bin'
        fake_bin.mkdir()
        fake_ssh = fake_bin / 'ssh'
        fake_ssh.write_text('#!/bin/sh\n: > "$ROUTE1_SSH_MARKER"\nprintf "%s\\n" "$@" > "$ROUTE1_SSH_ARGUMENTS"\n')
        fake_ssh.chmod(0o700)
        self.marker = self.root / 'ssh-called'
        self.arguments = self.root / 'ssh-arguments'
        self.env = dict(os.environ, PATH=str(fake_bin) + os.pathsep + os.environ['PATH'],
                        VOICE_MEMO_MAINT_HOST='synthetic.invalid',
                        ROUTE1_SSH_MARKER=str(self.marker), ROUTE1_SSH_ARGUMENTS=str(self.arguments))

    def fingerprint(self):
        p = self.root / 'private/server-access/known_hosts'
        p.parent.mkdir(parents=True)
        p.write_text('synthetic test-only fingerprint\n')

    def invoke(self, *arguments):
        return subprocess.run(['/bin/bash', str(self.wrapper), *arguments], env=self.env,
                              capture_output=True, text=True, timeout=10)

    def assert_disabled(self, *arguments):
        result = self.invoke(*arguments)
        self.assertEqual(result.returncode, 1)
        self.assertIn('已停用', result.stderr)
        self.assertNotIn('缺少服务器主机指纹', result.stderr)
        self.assertFalse(self.marker.exists())

    def test_deploy_and_rollback_stop_before_fingerprint_lookup(self):
        self.assert_disabled('deploy')
        self.assert_disabled('rollback', 'test-only-backup')

    def test_existing_fingerprint_does_not_allow_publication(self):
        self.fingerprint()
        self.assert_disabled('deploy')
        self.assert_disabled('rollback', 'test-only-backup')

    def test_argument_separator_cannot_bypass_local_command_check(self):
        self.fingerprint()
        for arguments in [('--', 'deploy'), ('--', 'rollback', 'test-only-backup')]:
            result = self.invoke(*arguments)
            self.assertEqual(result.returncode, 2)
            self.assertFalse(self.marker.exists())

    def test_usage_excludes_retired_operations_without_a_fingerprint(self):
        result = self.invoke()
        self.assertEqual(result.returncode, 2)
        self.assertIn('status', result.stderr)
        self.assertNotIn('deploy', result.stderr)
        self.assertNotIn('rollback', result.stderr)
        self.assertFalse(self.marker.exists())

    def test_supported_status_still_uses_quoted_wrapper(self):
        self.fingerprint()
        result = self.invoke('status')
        self.assertEqual(result.returncode, 0)
        self.assertTrue(self.marker.exists())
        arguments = self.arguments.read_text()
        self.assertIn('meowmaint@synthetic.invalid', arguments)
        self.assertIn('sudo -n /usr/local/sbin/meow-relay-maint status', arguments)


if __name__ == '__main__':
    unittest.main()
