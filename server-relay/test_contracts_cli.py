import contextlib
import io
import json
from pathlib import Path
import sqlite3
import tempfile
import unittest

from relay.__main__ import main
from relay.admin import AdminStore
from relay.identity import IdentityStore
from relay.runtime import Runtime
from relay.web import create_app
from app import MemoStore
from config_broker.protocol import METHOD_PARAMS


class ContractsAndCliTests(unittest.TestCase):
    def test_http_contract_matches_actual_methods_and_paths(self):
        root = Path(__file__).resolve().parents[1]
        contract = json.loads((root / 'docs/contracts/device-api-v2.json').read_text())
        with tempfile.TemporaryDirectory() as directory:
            app = create_app(Path(directory)/'state.db', public_url='https://relay.example.org')
            actual = {(method, route.path) for route in app.routes if hasattr(route,'methods')
                      for method in route.methods if route.path.startswith(('/v1/','/admin/api/'))}
        expected = {(r['method'],r['path']) for r in contract['routes']}
        self.assertEqual(actual, expected)
        self.assertNotIn('save_snapshot', METHOD_PARAMS)
        self.assertIn('save_activate', METHOD_PARAMS)

    def test_local_bootstrap_ticket_can_initialize_but_is_not_an_http_endpoint(self):
        with tempfile.TemporaryDirectory() as directory:
            database = Path(directory)/'state.db'; stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                self.assertEqual(main(['bootstrap','--database',str(database)]),0)
            code = stdout.getvalue().strip()
            store = AdminStore(database); store.initialize(code,'synthetic-password-123')
            self.assertTrue(store.initialized())
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                self.assertEqual(main(['bootstrap','--database',str(database)]),1)
            self.assertNotIn(code, stderr.getvalue())

    def test_unmarked_historical_identity_is_rejected_without_schema_or_data_changes(self):
        for table in ('devices', 'pairing_codes', 'device_enrollments', 'device_identity_epochs'):
            with self.subTest(table=table), tempfile.TemporaryDirectory() as directory:
                root = Path(directory); database = root / 'state.db'
                with sqlite3.connect(database) as db:
                    if table == 'devices':
                        db.execute('CREATE TABLE devices(device_id TEXT PRIMARY KEY, token_sha256 TEXT NOT NULL UNIQUE, created_at INTEGER NOT NULL, last_seen_at INTEGER, revoked_at INTEGER)')
                        db.execute('INSERT INTO devices VALUES(?,?,1,NULL,NULL)', ('old-device', AdminStore.digest('a' * 64)))
                    else:
                        db.execute('CREATE TABLE ' + table + '(historical_fact TEXT)')
                        db.execute('INSERT INTO ' + table + ' VALUES(?)', ('synthetic historical data',))
                before = database.read_bytes()
                for constructor in (
                    lambda: IdentityStore(database), lambda: MemoStore(database),
                    lambda: create_app(database, public_url='https://relay.example.org'),
                    lambda: Runtime(database, None, root / 'work', root / 'streams', start_workers=False),
                ):
                    with self.assertRaisesRegex(ValueError, 'unmarked identity data'):
                        constructor()
                    self.assertEqual(database.read_bytes(), before)
                stdout = io.StringIO()
                with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(main(['bootstrap', '--database', str(database)]), 1)
                self.assertEqual(stdout.getvalue(), '')
                self.assertEqual(database.read_bytes(), before)

    def test_empty_identity_tables_allow_bootstrap_and_current_identity_survives_restart(self):
        with tempfile.TemporaryDirectory() as directory:
            database = Path(directory) / 'state.db'
            with sqlite3.connect(database) as db:
                db.execute('CREATE TABLE devices(device_id TEXT PRIMARY KEY, token_sha256 TEXT NOT NULL UNIQUE, created_at INTEGER NOT NULL, last_seen_at INTEGER, revoked_at INTEGER)')
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                self.assertEqual(main(['bootstrap', '--database', str(database)]), 0)
            app = create_app(database, public_url='https://relay.example.org')
            app.state.admins.initialize(stdout.getvalue().strip(), 'synthetic-password-123')
            store = MemoStore(database)
            from test_support import pair_device
            token = pair_device(store, 'fresh-device')
            instance = store.identity.server_instance_uuid
            reopened = create_app(database, public_url='https://relay.example.org')
            self.assertTrue(reopened.state.admins.initialized())
            self.assertEqual(reopened.state.identity.server_instance_uuid, instance)
            self.assertTrue(reopened.state.identity.device_token_valid('fresh-device', token))
            with sqlite3.connect(database) as db:
                db.execute('UPDATE relay_identity SET schema_version=1')
            before = database.read_bytes()
            with self.assertRaisesRegex(ValueError, 'unsupported or damaged identity schema'):
                create_app(database, public_url='https://relay.example.org')
            self.assertEqual(database.read_bytes(), before)

    def test_multiple_workers_and_removed_control_operations_reject_before_startup(self):
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(main(['serve','--workers','2'],env={}),1)
            for name in ['bind-legacy','complete-metadata-restore']:
                with self.assertRaises(SystemExit):main(['control',name],env={})


if __name__=='__main__':unittest.main()
