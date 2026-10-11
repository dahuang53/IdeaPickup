"""Atomic one-action saves, pinned in-flight snapshots and bounded secret retention."""
import json
from pathlib import Path
import tempfile
import unittest

from config_broker.core import BrokerError, ConfigBroker, MAX_FILE
from config_broker.protocol import dispatch
from test_support import ready_broker, test_ownership


class BrokerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='route1-broker-')
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)
        self.broker = ready_broker(self.path)
        self.addCleanup(lambda: self.broker.close())

    def save(self, qwen='key-A', siyuan='token-A'):
        return self.broker.save_activate(self.broker.view()['active_revision'], {
            'qwen_api_key': {'op': 'replace', 'value': qwen}, 'siyuan_token': {'op': 'replace', 'value': siyuan}})

    def test_one_save_publishes_active_config_and_never_echoes_secrets(self):
        view = self.save()
        self.assertTrue(view['active_revision'])
        self.assertNotIn('key-A', json.dumps(view)); self.assertNotIn('token-A', json.dumps(view))
        self.assertNotIn('saved_revision', view)
        pin = self.broker.pin_active('test-job')
        self.assertEqual(self.broker.read_snapshot(pin['revision'], 'test-job')['config']['qwen_api_key'], 'key-A')
        self.broker.release('test-job')

    def test_inflight_snapshot_remains_immutable_and_release_removes_old_secret_file(self):
        old = self.save()['active_revision']; self.broker.pin_existing('job-A', old)
        self.save('key-B', 'token-B')
        self.assertEqual(self.broker.read_snapshot(old, 'job-A')['config']['siyuan_token'], 'token-A')
        self.assertTrue((self.path / ('revision-' + old + '.env')).exists())
        self.broker.release('job-A')
        self.assertFalse((self.path / ('revision-' + old + '.env')).exists())
        self.assertEqual(self.broker.view()['snapshot_count'], 1)

    def test_many_actual_inflight_revisions_do_not_create_an_arbitrary_four_version_gate(self):
        pins = []
        for index in range(8):
            revision = self.save('key-' + str(index))['active_revision']
            task = 'job-' + str(index); self.broker.pin_existing(task, revision); pins.append(task)
        self.assertEqual(self.broker.view()['pin_count'], 8)
        for task in pins:self.broker.release(task)
        self.assertEqual(self.broker.view()['snapshot_count'], 1)

    def test_stale_revision_and_unknown_or_unsupported_configuration_never_mutate_state(self):
        view = self.save(); previous = view['active_revision']
        self.save('key-B')
        with self.assertRaises(BrokerError): self.broker.save_activate(previous, {})
        for patch in [{'other': 'x'}, {'qwen_url': 'http://localhost/'}, {'qwen_model': 'unsupported'}, {'qwen_api_key': 'plain'}]:
            with self.assertRaises(BrokerError): self.broker.validate(patch)
        self.assertEqual(self.broker.view()['snapshot_count'], 1)

    def test_current_root_policy_and_no_path_or_split_save_rpc(self):
        if __import__('os').geteuid() != 0:
            with self.assertRaises(BrokerError): ConfigBroker(self.path)
        for method, params in [('save_snapshot', {'expected_revision': None, 'patch': {}}), ('activate', {}), ('read_snapshot', {'filename': '/etc/passwd'})]:
            result = dispatch(self.broker.core, {'version': 1, 'id': 'test', 'method': method, 'params': params})
            self.assertFalse(result['ok'])

    def test_read_requires_a_durable_pin(self):
        revision = self.save()['active_revision']
        with self.assertRaises(BrokerError): self.broker.read_snapshot(revision, 'unowned')

    def test_state_budget_failure_occurs_before_replacing_the_durable_manifest(self):
        state = self.broker.core._load(); original = (self.path / 'state.json').read_bytes()
        state['pins'] = {'x' * (MAX_FILE + 1): 'invalid'}
        with self.assertRaises(BrokerError): self.broker.core._write_state(state)
        self.assertEqual((self.path / 'state.json').read_bytes(), original)


class AtomicSaveCrashTests(unittest.TestCase):
    def test_each_save_boundary_recovers_an_entire_old_or_new_configuration(self):
        boundaries = ['file_fsync:revision-', 'rename:revision-', 'directory_fsync:revision-',
                      'file_fsync:state.json', 'rename:state.json', 'directory_fsync:state.json']
        for boundary in boundaries:
            with self.subTest(boundary=boundary), tempfile.TemporaryDirectory(prefix='route1-save-') as directory:
                p = Path(directory); broker = ready_broker(p)
                broker.save_activate(None, {'qwen_api_key': {'op': 'replace', 'value': 'key-A'}, 'siyuan_token': {'op': 'replace', 'value': 'token-A'}})
                triggered = []
                def fault(stage):
                    if not triggered and stage.startswith(boundary):
                        triggered.append(stage); raise OSError('synthetic interruption')
                broker.core._fault = fault
                with self.assertRaises(OSError):
                    broker.save_activate(broker.view()['active_revision'], {'qwen_api_key': {'op': 'replace', 'value': 'key-B'}, 'siyuan_token': {'op': 'replace', 'value': 'token-B'}})
                self.assertTrue(triggered); broker.close()
                recovered = ready_broker(p)
                try:
                    pin = recovered.pin_active('readback')
                    config = recovered.read_snapshot(pin['revision'], 'readback')['config']
                    self.assertIn((config['qwen_api_key'], config['siyuan_token']), [('key-A', 'token-A'), ('key-B', 'token-B')])
                    recovered.release('readback')
                    self.assertEqual(recovered.view()['snapshot_count'], 1)
                    self.assertFalse(any(f.name.startswith('.tmp-') for f in p.iterdir()))
                finally: recovered.close()


if __name__ == '__main__':unittest.main()
