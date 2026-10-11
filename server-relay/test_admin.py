from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from argon2 import PasswordHasher
from relay.admin import AdminError, AdminStore


class AdminTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.now = 1000
        self.path = Path(self.temp.name) / "state.sqlite3"
        self.hasher = PasswordHasher(time_cost=1, memory_cost=1024, parallelism=1)
        self.store = AdminStore(self.path, clock=lambda: self.now, hasher=self.hasher)
        self.password = "Synthetic-only-password!"

    def tearDown(self):
        self.temp.cleanup()

    def initialize(self):
        code = self.store.issue_bootstrap()
        self.store.initialize(code, self.password)
        return code

    def assertDenied(self, status, function, *args, **kwargs):
        with self.assertRaises(AdminError) as error:
            function(*args, **kwargs)
        self.assertEqual(error.exception.status, status)

    def test_bootstrap_single_use_hashes_only_and_closed_after_init(self):
        code = self.initialize()
        self.assertTrue(self.store.initialized())
        self.assertDenied(401, self.store.initialize, code, self.password)
        self.assertDenied(409, self.store.issue_bootstrap)
        raw = self.path.read_bytes()
        self.assertNotIn(code.encode(), raw)
        self.assertNotIn(self.password.encode(), raw)
        with self.store.connect() as db:
            row = db.execute("SELECT * FROM admin_account").fetchone()
        self.assertTrue(row["password_hash"].startswith("$argon2id$"))
        self.assertIsNone(row["bootstrap_hash"])

    def test_bootstrap_expiry_exact_boundary_and_replacement(self):
        old = self.store.issue_bootstrap()
        self.now += 900
        self.assertDenied(401, self.store.initialize, old, self.password)
        new = self.store.issue_bootstrap()
        self.assertDenied(401, self.store.initialize, old, self.password)
        self.store.initialize(new, self.password)

    def test_login_csrf_logout_and_restart(self):
        self.initialize()
        login = self.store.login(self.password)
        token = login["token"]
        restored = AdminStore(self.path, clock=lambda: self.now, hasher=self.hasher)
        self.assertTrue(restored.authenticate(token)["authenticated"])
        self.assertDenied(403, restored.authenticate, token, csrf="wrong", require_csrf=True)
        self.assertDenied(403, restored.authenticate, token, csrf="错", require_csrf=True)
        restored.authenticate(token, csrf=login["csrf"], require_csrf=True)
        self.assertNotIn(token.encode(), self.path.read_bytes())
        restored.logout(token)
        self.assertDenied(401, self.store.authenticate, token)

    def test_device_tokens_and_arbitrary_bearers_are_not_admin_sessions(self):
        self.initialize()
        for token in (None, "", "device-token" * 8, "x" * 64, "非ascii" * 10):
            self.assertDenied(401, self.store.authenticate, token)

    def test_idle_and_absolute_expiry(self):
        self.initialize()
        first = self.store.login(self.password)["token"]
        self.now += 1800
        self.assertDenied(401, self.store.authenticate, first)
        second = self.store.login(self.password)["token"]
        for _ in range(31):
            self.now += 900
            self.store.authenticate(second)
        self.now += 900
        self.assertDenied(401, self.store.authenticate, second)

    def test_local_reset_revokes_sessions_but_preserves_other_tables(self):
        self.initialize()
        token = self.store.login(self.password)["token"]
        with self.store.connect() as db:
            db.execute("CREATE TABLE preservation_test(value TEXT)")
            db.execute("INSERT INTO preservation_test VALUES('keep')")
        code = self.store.issue_bootstrap(reset=True)
        self.assertDenied(401, self.store.authenticate, token)
        self.assertFalse(self.store.initialized())
        self.store.initialize(code, "Different-synthetic-password")
        self.assertDenied(401, self.store.login, self.password)
        with self.store.connect() as db:
            self.assertEqual(db.execute("SELECT value FROM preservation_test").fetchone()[0], "keep")

    def test_failed_login_rate_is_persistent_and_expires(self):
        self.initialize()
        for _ in range(5):
            self.assertDenied(401, self.store.login, "wrong", peer="isolated-test-peer")
        restored = AdminStore(self.path, clock=lambda: self.now, hasher=self.hasher)
        self.assertDenied(429, restored.login, self.password, peer="isolated-test-peer")
        self.now += 300
        restored.login(self.password, peer="isolated-test-peer")

    def test_concurrent_initialization_consumes_code_once(self):
        code = self.store.issue_bootstrap()
        def initialize(index):
            try:
                self.store.initialize(code, self.password, peer=str(index))
                return "success"
            except AdminError as error:
                return error.code
        with ThreadPoolExecutor(max_workers=2) as pool:
            outcomes = list(pool.map(initialize, range(2)))
        self.assertEqual(outcomes.count("success"), 1)

    def test_invalid_password_does_not_consume_bootstrap(self):
        code = self.store.issue_bootstrap()
        for invalid in ("short", ["password"], "x" * 129, "x" * 12 + "\ud800"):
            self.assertDenied(400, self.store.initialize, code, invalid)
        self.now += 300
        self.store.initialize(code, self.password)

    def test_reset_racing_verification_does_not_mint_old_password_session(self):
        self.initialize()
        real_verify = self.hasher.verify
        def resetting_verify(encoded, password):
            result = real_verify(encoded, password)
            self.store.issue_bootstrap(reset=True)
            return result
        self.store.hasher = mock.Mock(wraps=self.hasher)
        self.store.hasher.verify.side_effect = resetting_verify
        self.assertDenied(401, self.store.login, self.password)


if __name__ == "__main__":
    unittest.main()
