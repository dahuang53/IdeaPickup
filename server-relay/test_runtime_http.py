"""Fresh-install HTTP integration with real ledger, broker, codec and provider substitutes."""
import hashlib
import errno
import json
import os
from pathlib import Path
import secrets
import sqlite3
import tempfile
import unittest
import uuid
from unittest import mock

from fastapi.testclient import TestClient
from app import RelayError
from config_broker.core import BrokerError
from relay.providers import ConfiguredService
from relay.runtime import Runtime
from relay.web import create_app
from stream_service import FORMAT, RETENTION_MS
from test_app import wav_bytes
from test_stream_service import synthetic_chunks
from test_support import LocalBroker


class RuntimeHTTPTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='route1-http-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'secrets').mkdir(mode=0o700)
        self.broker = LocalBroker(self.root / 'secrets')
        self.now = 1_000_000
        self.modes = {'asr': 'ok', 'delivery': 'ok'}
        self.asr, self.deliveries = [], []
        self.runtime = self.make_runtime()
        self.open_client()
        self.addCleanup(self.close_current)
        code = self.app.state.admins.issue_bootstrap()
        self.password = 'synthetic-password-123'
        self.assertEqual(self.client.post('/admin/api/session/initialize', json={'code': code, 'password': self.password}, headers=self.origin).status_code, 200)
        self.login()

    def make_runtime(self):
        def factory(store, config, progress=None):
            service = ConfiguredService(store, config, progress=progress)
            def asr(data, duration):
                self.asr.append((config['qwen_api_key'], duration))
                if self.modes['asr'] == 'fail':
                    raise RelayError(401, 'qwen_auth', 'synthetic rejection')
                return '合成音频用于验证新安装流程', 'synthetic-request'
            def delivery(title, content):
                if self.modes['delivery'] == 'uncertain':
                    raise RelayError(503, 'siyuan_uncertain', 'synthetic lost acknowledgement')
                if self.modes['delivery'] == 'fail':
                    raise RelayError(502, 'siyuan_rejected', 'synthetic rejection')
                self.deliveries.append((config['siyuan_token'], title))
            service._qwen_transcriber, service._siyuan_sender = asr, delivery
            return service
        return Runtime(self.root / 'state.db', self.broker, self.root / 'work', self.root / 'streams',
                       provider_factory=factory, clock=lambda: self.now, start_workers=False, min_free_bytes=0)

    def open_client(self, prefix=''):
        self.prefix = prefix
        self.origin = {'Origin': 'https://relay.example.org'}
        self.app = create_app(self.root / 'state.db', public_url='https://relay.example.org' + prefix, broker=self.broker, runtime=self.runtime)
        self.client = TestClient(self.app, base_url='https://relay.example.org')

    def close_current(self):
        self.client.close()
        self.runtime.close()
        self.broker.close()

    def login(self):
        response = self.client.post(self.prefix + '/admin/api/session/login', json={'password': self.password}, headers=self.origin)
        self.assertEqual(response.status_code, 200)
        self.admin_headers = {**self.origin, 'X-CSRF-Token': response.json()['csrf']}

    def config(self, qwen='qwen-A', siyuan='siyuan-A'):
        view = self.client.get(self.prefix + '/admin/api/config').json()
        result = self.client.post(self.prefix + '/admin/api/config', json={'expected_revision': view['active_revision'], 'patch': {
            'qwen_api_key': {'op': 'replace', 'value': qwen}, 'siyuan_token': {'op': 'replace', 'value': siyuan}}}, headers=self.admin_headers)
        self.assertEqual(result.status_code, 200, result.status_code)
        return result.json()

    def pair(self, device='future-device', token=None, finalize=True):
        code = self.client.post(self.prefix + '/admin/api/devices/pairing-code', json={}, headers=self.admin_headers).json()['pairing_code']
        request = {'device_id': device, 'pairing_code': code, 'enrollment_id': str(uuid.uuid4()), 'candidate_token': token or secrets.token_hex(32)}
        response = self.client.post(self.prefix + '/v1/enrollments', json=request)
        self.assertEqual(response.status_code, 201)
        self.assertNotIn(request['candidate_token'], response.text)
        if finalize:
            for action in ['activate', 'finalize']:
                self.assertEqual(self.client.post(self.prefix + '/v1/enrollments/' + request['enrollment_id'] + '/' + action,
                                                json={}, headers={'Authorization': 'Bearer ' + request['candidate_token']}).status_code, 200)
        return request

    def wav(self, request, recording=1, data=None):
        data = wav_bytes() if data is None else data
        return self.client.post(self.prefix + '/v1/memos', content=data, headers={
            'Authorization': 'Bearer ' + request['candidate_token'], 'X-Device-ID': request['device_id'],
            'X-Recording-ID': str(recording), 'X-Audio-SHA256': hashlib.sha256(data).hexdigest(), 'Content-Type': 'audio/wav'})

    def status(self, request, recording=1):
        return self.client.get(self.prefix + '/v1/memos/' + request['device_id'] + '/' + str(recording),
                               headers={'Authorization': 'Bearer ' + request['candidate_token']})

    def restart(self):
        self.client.close(); self.runtime.close(); self.broker.close()
        self.broker = LocalBroker(self.root / 'secrets')
        self.runtime = self.make_runtime()
        self.open_client(self.prefix); self.login()

    def stream(self, request, recording=0x80000001, finish=True):
        auth = {'Authorization': 'Bearer ' + request['candidate_token']}
        self.assertEqual(self.client.post(self.prefix + '/v1/streams', json=FORMAT, headers={**auth, 'X-Device-ID': request['device_id'], 'X-Recording-ID': str(recording)}).status_code, 201)
        path = self.prefix + '/v1/streams/' + request['device_id'] + '/' + str(recording)
        chunks = synthetic_chunks()
        for seq, chunk in enumerate(chunks):
            headers = {**auth, 'Content-Type': 'application/octet-stream', 'X-Chunk-SHA256': hashlib.sha256(chunk).hexdigest()}
            put = self.client.put(path + '/chunks/' + str(seq), content=chunk, headers=headers)
            self.assertEqual(put.status_code, 201)
            duplicate = self.client.put(path + '/chunks/' + str(seq), content=chunk, headers=headers)
            self.assertEqual(duplicate.status_code, 200)
            self.assertEqual(duplicate.json()['next_seq'], put.json()['next_seq'])
            self.assertEqual(duplicate.json()['received_samples'], put.json()['received_samples'])
        if finish:
            value = {'chunks': 2, 'samples': 32000, 'interrupted': False, 'reason': ''}
            self.assertEqual(self.client.post(path + '/finish', json=value, headers=auth).status_code, 202)
            self.assertEqual(self.client.post(path + '/finish', json=value, headers=auth).status_code, 202)
        return path, auth

    def test_empty_service_can_initialize_and_one_save_activates(self):
        self.assertEqual(self.client.get('/health/ready').json()['reason'], 'not_configured')
        page = self.client.get('/admin/').text
        self.assertIn('保存并生效', page)
        self.assertNotIn('activate-config', page)
        self.assertIn('name="qwen_url" type="url" readonly', page)
        view = self.config()
        self.assertTrue(self.client.get('/health/ready').json()['ready'])
        self.assertNotIn('qwen-A', json.dumps(view))
        self.assertNotIn('siyuan-A', json.dumps(view))
        self.assertNotIn('saved_revision', view)
        self.assertEqual(self.client.post('/admin/api/config/activate', json={}, headers=self.admin_headers).status_code, 404)

    def test_pairing_replays_lost_creation_activation_and_finalization_responses(self):
        request = self.pair(finalize=False)
        for _ in range(2):
            self.assertEqual(self.client.post('/v1/enrollments', json=request).status_code, 201)
        self.config()
        self.assertEqual(self.wav(request).status_code, 401)
        auth = {'Authorization': 'Bearer ' + request['candidate_token']}
        endpoint = '/v1/enrollments/' + request['enrollment_id']
        for action in ['activate', 'activate', 'finalize', 'finalize']:
            self.assertEqual(self.client.post(endpoint + '/' + action, json={}, headers=auth).status_code, 200)
        self.assertEqual(self.client.get(endpoint, headers=auth).json()['phase'], 'finalized')
        self.assertEqual(self.wav(request).status_code, 202)
        with self.runtime.store.connect() as db:
            self.assertEqual(db.execute('SELECT COUNT(*) FROM device_enrollments').fetchone()[0], 1)
            self.assertEqual(db.execute('SELECT attempts FROM pairing_codes').fetchone()[0], 1)

    def test_same_recording_id_isolated_across_future_device_models(self):
        self.config(); a = self.pair('future-model-a'); b = self.pair('other-model-b')
        self.assertEqual(self.wav(a).status_code, 202); self.assertEqual(self.wav(b).status_code, 202)
        self.runtime.run_pending_once(); self.runtime.run_pending_once()
        self.assertEqual(self.status(a).json()['status'], 'sent'); self.assertEqual(self.status(b).json()['status'], 'sent')
        self.assertEqual(len(self.deliveries), 2)
        unauthorized = self.client.get('/v1/memos/' + b['device_id'] + '/1', headers={'Authorization': 'Bearer ' + a['candidate_token']})
        self.assertEqual(unauthorized.status_code, 401)
        self.assertEqual(self.wav(a, data=wav_bytes(2)).status_code, 409)
        self.assertEqual(self.status(a).json()['audio_bytes'], len(wav_bytes()))
        self.assertEqual(self.client.get('/v1/memos/' + a['device_id'] + '/1/audio').status_code, 404)

    def test_configuration_change_never_changes_an_inflight_attempt(self):
        self.config(); device = self.pair(); self.assertEqual(self.wav(device).status_code, 202)
        self.config('qwen-B', 'siyuan-B'); self.runtime.run_pending_once()
        self.assertEqual(self.asr[-1][0], 'qwen-A'); self.assertEqual(self.deliveries[-1][0], 'siyuan-A')
        self.assertEqual(self.wav(device, recording=2).status_code, 202); self.runtime.run_pending_once()
        self.assertEqual(self.asr[-1][0], 'qwen-B'); self.assertEqual(self.deliveries[-1][0], 'siyuan-B')

    def test_failed_tasks_release_snapshots_and_do_not_block_key_rotations_or_restart(self):
        device = self.pair(); self.modes['asr'] = 'fail'
        for index in range(10):
            self.config('qwen-' + str(index))
            self.assertEqual(self.wav(device, recording=index+1).status_code, 202)
            self.runtime.run_pending_once()
            self.assertEqual(self.status(device, index+1).json()['status'], 'transcription_failed')
            self.assertEqual(self.broker.view()['pin_count'], 0)
            self.assertLessEqual(self.broker.view()['snapshot_count'], 1)
        self.restart()
        self.assertTrue(self.client.get('/health/ready').json()['ready'])
        self.assertEqual(self.broker.view()['pin_count'], 0)
        self.modes['asr'] = 'ok'; self.config('corrected-key')
        self.assertEqual(self.wav(device, recording=1).status_code, 202)
        self.runtime.run_pending_once(); self.assertEqual(self.status(device).json()['status'], 'sent')

    def _check_pending_release(self, response_lost, restart):
        for index, outcome in enumerate(('sent', 'transcription_failed', 'audio_expired'), 100):
            with self.subTest(outcome=outcome):
                self.modes['asr'] = 'fail' if outcome == 'transcription_failed' else 'ok'
                revision = self.config()['active_revision']
                device = self.pair(device='release-' + str(index))
                recording = index if outcome != 'audio_expired' else 0x80000000 + index
                if outcome == 'audio_expired':
                    self.stream(device, recording, finish=False)
                    self.now += RETENTION_MS + 1
                else:
                    self.assertEqual(self.wav(device, recording=recording).status_code, 202)
                original = self.broker.release
                calls = []
                def fail_once(pin):
                    calls.append(pin)
                    if len(calls) == 1:
                        if response_lost:
                            original(pin)
                        raise BrokerError('storage_failure')
                    return original(pin)
                with mock.patch.object(self.broker, 'release', side_effect=fail_once):
                    if outcome == 'audio_expired':
                        self.runtime.cleanup_once()
                    else:
                        self.assertTrue(self.runtime.run_pending_once())
                    job = self.runtime._job(device['device_id'], recording)
                    self.assertEqual(job['phase'], outcome)
                    self.assertEqual(job['revision'], revision)
                    self.assertIsNotNone(job['pin_id'])
                    self.assertIsNone(self.runtime._startup_error)
                    self.assertEqual(self.broker.view()['pin_count'], 0 if response_lost else 1)
                    self.config('qwen-B', 'siyuan-B')
                    old_secret = self.root / 'secrets' / ('revision-' + revision + '.env')
                    self.assertEqual(old_secret.exists(), not response_lost)
                    if restart:
                        self.restart()
                    else:
                        self.runtime.cleanup_once()
                job = self.runtime._job(device['device_id'], recording)
                self.assertIsNone(job['pin_id'])
                self.assertIsNone(job['revision'])
                self.assertEqual(self.broker.view()['pin_count'], 0)
                self.assertFalse(old_secret.exists())
                self.assertTrue(self.runtime.readiness()['ready'])
                self.assertFalse(self.runtime.run_pending_once())
                if outcome == 'audio_expired':
                    self.assertFalse(list(self.runtime.streams.directory.rglob('*.opus')))

    def test_release_failure_is_compensated_by_cleanup(self):
        self._check_pending_release(response_lost=False, restart=False)

    def test_release_response_loss_is_compensated_by_cleanup(self):
        self._check_pending_release(response_lost=True, restart=False)

    def test_restart_compensates_pending_release(self):
        self._check_pending_release(response_lost=False, restart=True)

    def test_restart_compensates_release_response_loss(self):
        self._check_pending_release(response_lost=True, restart=True)

    def test_release_ack_before_ledger_write_failure_recovers_without_old_snapshot(self):
        for index, restart in enumerate((False, True), 200):
            with self.subTest(restart=restart):
                revision = self.config()['active_revision']
                device = self.pair(device='ledger-' + str(index))
                self.assertEqual(self.wav(device, recording=index).status_code, 202)
                self.config('qwen-B', 'siyuan-B')
                original = self.runtime._update_job
                failed = []
                def fail_clear(*args, **fields):
                    if 'pin_id' in fields and fields['pin_id'] is None and not failed:
                        failed.append(True)
                        raise sqlite3.OperationalError('synthetic database busy')
                    return original(*args, **fields)
                with mock.patch.object(self.runtime, '_update_job', side_effect=fail_clear):
                    with self.assertRaises(sqlite3.OperationalError):
                        self.runtime.run_pending_once()
                self.assertEqual(self.status(device, index).json()['status'], 'sent')
                self.assertIsNotNone(self.runtime._job(device['device_id'], index)['pin_id'])
                self.assertEqual(self.broker.view()['pin_count'], 0)
                self.assertFalse((self.root / 'secrets' / ('revision-' + revision + '.env')).exists())
                if restart:
                    self.restart()
                else:
                    self.runtime.cleanup_once()
                self.assertIsNone(self.runtime._job(device['device_id'], index)['pin_id'])
                self.assertTrue(self.runtime.readiness()['ready'])
                self.assertFalse(self.runtime.run_pending_once())

    def test_startup_does_not_repin_stopped_attempt_when_release_is_still_unavailable(self):
        revision = self.config()['active_revision']
        device = self.pair()
        self.assertEqual(self.wav(device).status_code, 202)
        self.config('qwen-B', 'siyuan-B')
        release = self.broker.release
        def lose_reply(pin):
            release(pin)
            raise BrokerError('storage_failure')
        with mock.patch.object(self.broker, 'release', side_effect=lose_reply):
            self.assertTrue(self.runtime.run_pending_once())
        self.assertFalse((self.root / 'secrets' / ('revision-' + revision + '.env')).exists())
        with mock.patch('config_broker.core.ConfigBroker.release', side_effect=BrokerError('storage_failure')):
            self.restart()
            self.assertIsNotNone(self.runtime._job(device['device_id'], 1)['pin_id'])
            self.assertIsNone(self.runtime._startup_error)
            self.assertEqual(self.broker.view()['pin_count'], 0)
        self.runtime.cleanup_once()
        self.assertIsNone(self.runtime._job(device['device_id'], 1)['pin_id'])
        self.assertTrue(self.runtime.readiness()['ready'])
        self.assertFalse(self.runtime.run_pending_once())
        self.assertEqual(len(self.deliveries), 1)

    def test_unstarted_stream_keeps_release_reference_until_cleanup(self):
        revision = self.config()['active_revision']
        device = self.pair()
        recording = 0x80000001
        headers = {'Authorization': 'Bearer ' + device['candidate_token'],
                   'X-Device-ID': device['device_id'], 'X-Recording-ID': str(recording)}
        with mock.patch.object(self.runtime.streams, 'create', side_effect=sqlite3.OperationalError('synthetic busy')), \
                mock.patch.object(self.broker, 'release', side_effect=BrokerError('storage_failure')):
            with self.assertRaises(sqlite3.OperationalError):
                self.runtime.stream_create(headers, FORMAT)
        job = self.runtime._job(device['device_id'], recording)
        self.assertIsNotNone(job['pin_id'])
        self.assertEqual(job['phase'], 'awaiting_audio')
        self.config('qwen-B', 'siyuan-B')
        old_secret = self.root / 'secrets' / ('revision-' + revision + '.env')
        self.assertTrue(old_secret.exists())
        self.runtime.cleanup_once()
        self.assertIsNone(self.runtime._job(device['device_id'], recording))
        self.assertEqual(self.broker.view()['pin_count'], 0)
        self.assertFalse(old_secret.exists())
        self.assertTrue(self.runtime.readiness()['ready'])
        self.assertEqual(self.runtime.stream_create(headers, FORMAT)[1], 201)

    def test_broker_unlink_fault_collects_orphan_without_removing_active_or_inflight_secrets(self):
        for index, (removed_before_error, restart) in enumerate(((False, False), (True, False), (False, True), (True, True)), 400):
            with self.subTest(removed_before_error=removed_before_error, restart=restart):
                prior_pins = self.broker.view()['pin_count']
                old_revision = self.config('old-' + str(index))['active_revision']
                device = self.pair(device='unlink-' + str(index))
                self.assertEqual(self.wav(device, recording=index).status_code, 202)
                pinned_revision = self.config('inflight-' + str(index))['active_revision']
                self.stream(device, 0x80000000 + index, finish=False)
                active_revision = self.config('active-' + str(index))['active_revision']
                old_secret = self.root / 'secrets' / ('revision-' + old_revision + '.env')
                original, faults = os.unlink, []
                def fail_unlink(path, *args, **kwargs):
                    if path == old_secret.name and kwargs.get('dir_fd') == self.broker.core._dir and not faults:
                        faults.append(True)
                        if removed_before_error:
                            original(path, *args, **kwargs)
                        raise OSError('synthetic one-shot secret unlink failure')
                    return original(path, *args, **kwargs)
                with mock.patch('config_broker.core.os.unlink', side_effect=fail_unlink):
                    self.assertTrue(self.runtime.run_pending_once())
                self.assertEqual(faults, [True])
                self.assertIsNotNone(self.runtime._job(device['device_id'], index)['pin_id'])
                self.assertEqual(old_secret.exists(), not removed_before_error)
                if restart:
                    self.restart()
                else:
                    self.runtime.cleanup_once()
                self.assertIsNone(self.runtime._job(device['device_id'], index)['pin_id'])
                self.assertFalse(old_secret.exists())
                self.assertEqual(self.broker.view()['pin_count'], prior_pins + 1)
                self.assertEqual(self.broker.view()['active_revision'], active_revision)
                for revision in (pinned_revision, active_revision):
                    self.assertTrue((self.root / 'secrets' / ('revision-' + revision + '.env')).exists())
                other = self.runtime._job(device['device_id'], 0x80000000 + index)
                self.assertEqual(self.broker.read_snapshot(pinned_revision, other['pin_id'])['revision'], pinned_revision)
                self.assertTrue(self.runtime.readiness()['ready'])
                self.assertFalse(self.runtime.run_pending_once())

    def test_failure_before_send_keeps_text_and_retry_uses_current_config(self):
        for index, boundary in enumerate(('wav_phase', 'wav_sending', 'stream_sending'), 300):
            with self.subTest(boundary=boundary):
                revision = self.config()['active_revision']
                device = self.pair(device='pre-send-' + str(index))
                recording = index if boundary != 'stream_sending' else 0x80000000 + index
                if boundary == 'stream_sending':
                    self.stream(device, recording)
                else:
                    self.assertEqual(self.wav(device, recording=recording).status_code, 202)
                target = self.runtime if boundary == 'wav_phase' else (
                    self.runtime.streams.store if boundary == 'stream_sending' else self.runtime.store)
                name = '_update_job' if boundary == 'wav_phase' else 'update'
                original, faults = getattr(target, name), []
                def fail_before_send(*args, **fields):
                    if fields.get('phase' if boundary == 'wav_phase' else 'status') == 'sending' and not faults:
                        faults.append(True)
                        raise sqlite3.OperationalError('synthetic database busy')
                    return original(*args, **fields)
                before = len(self.deliveries), len(self.asr)
                with mock.patch.object(target, name, side_effect=fail_before_send):
                    self.assertTrue(self.runtime.run_pending_once())
                self.assertEqual(faults, [True])
                self.assertEqual(len(self.deliveries), before[0])
                item = next(x for x in self.runtime.list_jobs()['items'] if x['device_id'] == device['device_id'])
                self.assertEqual(item['status'], 'delivery_failed')
                self.assertEqual(item['reason'], 'processing_failed')
                self.assertTrue(item['can_retry'])
                self.assertTrue(self.runtime.store.get(device['device_id'], recording)['cloud_content'])
                self.assertEqual(self.broker.view()['pin_count'], 0)
                self.config('qwen-B', 'siyuan-B')
                self.assertFalse((self.root / 'secrets' / ('revision-' + revision + '.env')).exists())
                self.restart()
                self.assertFalse(self.runtime.run_pending_once())
                path = '/admin/api/jobs/' + device['device_id'] + '/' + str(recording) + '/retry'
                self.assertEqual(self.client.post(path, json={}, headers=self.admin_headers).status_code, 202)
                self.runtime.cleanup_once()
                self.assertTrue(self.runtime.run_pending_once())
                self.assertEqual(len(self.deliveries), before[0] + 1)
                self.assertEqual(self.deliveries[-1][0], 'siyuan-B')
                self.assertEqual(len(self.asr), before[1] + 1)
                memo = self.runtime.store.get(device['device_id'], recording)
                self.assertEqual(memo['status'], 'sent')
                self.assertIsNone(memo['cloud_content'])

    def test_bad_first_body_does_not_reserve_a_false_audio_identity(self):
        self.config(); device = self.pair(); data = wav_bytes()
        response = self.client.post('/v1/memos', content=data, headers={'Authorization': 'Bearer ' + device['candidate_token'],
            'X-Device-ID': device['device_id'], 'X-Recording-ID': '1', 'X-Audio-SHA256': 'f' * 64, 'Content-Type': 'audio/wav'})
        self.assertEqual(response.status_code, 400)
        self.assertEqual(self.broker.view()['pin_count'], 0)
        self.assertEqual(self.wav(device).status_code, 202)
        self.runtime.run_pending_once()
        self.assertEqual(self.status(device).json()['status'], 'sent')

    def test_incomplete_wav_rejected_before_identity_and_valid_same_id_can_retry(self):
        self.config(); device = self.pair()
        complete = wav_bytes()
        odd = bytearray(complete + b'\x00')
        odd[4:8] = (len(odd) - 8).to_bytes(4, 'little')
        odd[40:44] = (len(odd) - 44).to_bytes(4, 'little')
        for body in (complete[:-1], complete[:-100], bytes(odd)):
            with self.subTest(size=len(body)):
                response = self.wav(device, data=body)
                self.assertEqual(response.status_code, 400)
                self.assertEqual(response.json()['error'], 'invalid_wav')
                self.assertIsNone(self.runtime.store.get(device['device_id'], 1))
                self.assertEqual(self.broker.view()['pin_count'], 0)
                self.assertEqual(len(self.runtime._admissions), 0)
                self.assertEqual(list((self.root / 'work').glob('*.wav')), [])
                self.assertEqual(self.asr, [])
        self.assertEqual(self.wav(device, data=complete).status_code, 202)
        self.runtime.run_pending_once()
        self.assertEqual(self.status(device).json()['status'], 'sent')
        self.assertEqual(self.wav(device, data=wav_bytes(2)).status_code, 409)
        self.assertEqual(len(self.deliveries), 1)

    def test_receive_setup_and_queue_metadata_failures_release_idle_reservations(self):
        self.config(); device = self.pair()
        real_open, real_update = os.open, self.runtime._update_job
        for recording, stage in enumerate(('allocation', 'receiving', 'queued'), 1):
            with self.subTest(stage=stage):
                def fail_open(path, *args, **kwargs):
                    if stage == 'allocation' and Path(path).parent == self.root / 'work':
                        raise OSError(errno.ENOSPC, 'synthetic receive allocation failure')
                    return real_open(path, *args, **kwargs)
                failed = False
                def fail_update(*args, **kwargs):
                    nonlocal failed
                    if not failed and kwargs.get('phase') == stage:
                        failed = True
                        raise OSError(errno.ENOSPC, 'synthetic metadata failure')
                    return real_update(*args, **kwargs)
                with mock.patch('relay.runtime.os.open', side_effect=fail_open), mock.patch.object(self.runtime, '_update_job', side_effect=fail_update):
                    if stage == 'queued':
                        with self.assertRaises(OSError): self.wav(device, recording)
                    else:
                        self.assertEqual(self.wav(device, recording).status_code, 507)
                self.runtime.cleanup_once()
                job = self.runtime._job(device['device_id'], recording)
                self.assertIn(job['phase'], ('awaiting_audio', 'transcription_failed'))
                self.assertIsNone(job['revision'])
                self.assertIsNone(job['pin_id'])
                self.assertEqual(self.broker.view()['pin_count'], 0)
                self.assertEqual(len(self.runtime._admissions), 0)
                self.assertEqual(self.runtime._scheduled, set())
                self.assertEqual(list((self.root / 'work').glob('*.wav')), [])
                self.assertTrue(self.runtime.readiness()['ready'])
                if stage == 'queued':
                    # Accepted audio identity survives a failed handoff.
                    self.assertEqual(self.wav(device, recording, wav_bytes(2)).status_code, 409)
                self.assertEqual(self.wav(device, recording).status_code, 202)
                self.runtime.run_pending_once()
                self.assertEqual(self.status(device, recording).json()['status'], 'sent')
        self.assertEqual(len(self.deliveries), 3)

    def test_receive_abort_storage_uncertainty_stops_admission_until_restart(self):
        self.config(); device = self.pair()
        real_open, real_update = os.open, self.runtime._update_job
        def fail_open(path, *args, **kwargs):
            if Path(path).parent == self.root / 'work':
                raise OSError(errno.ENOSPC, 'synthetic receive failure')
            return real_open(path, *args, **kwargs)
        def fail_abort(*args, **kwargs):
            if kwargs.get('phase') == 'awaiting_audio':
                raise OSError(errno.ENOSPC, 'synthetic abort failure')
            return real_update(*args, **kwargs)
        with mock.patch('relay.runtime.os.open', side_effect=fail_open), mock.patch.object(self.runtime, '_update_job', side_effect=fail_abort):
            with self.assertRaises(OSError): self.wav(device)
        self.assertFalse(self.runtime.readiness()['ready'])
        self.assertEqual(self.runtime.readiness()['reason'], 'task_storage_failed')
        self.assertEqual(len(self.runtime._admissions), 0)
        self.assertIsNone(self.runtime.store.get(device['device_id'], 1))
        self.assertEqual(self.wav(device, 2).status_code, 503)
        self.restart()
        self.assertTrue(self.runtime.readiness()['ready'])
        self.assertEqual(self.broker.view()['pin_count'], 0)
        self.assertEqual(self.wav(device).status_code, 202)
        self.runtime.run_pending_once()
        self.assertEqual(self.status(device).json()['status'], 'sent')

    def test_failed_receive_file_deletion_is_retried_by_cleanup(self):
        self.config(); device = self.pair()
        real_unlink = Path.unlink
        def fail_unlink(path, *args, **kwargs):
            if path.parent == self.root / 'work':
                raise OSError(errno.EIO, 'synthetic temporary delete failure')
            return real_unlink(path, *args, **kwargs)
        with mock.patch.object(Path, 'unlink', autospec=True, side_effect=fail_unlink):
            self.assertEqual(self.wav(device, data=wav_bytes()[:-1]).status_code, 400)
        self.assertEqual(self.runtime._job(device['device_id'], 1)['phase'], 'awaiting_audio')
        self.assertEqual(self.broker.view()['pin_count'], 0)
        self.assertEqual(len(self.runtime._admissions), 0)
        self.assertEqual(len(list((self.root / 'work').glob('*.wav'))), 1)
        self.assertFalse(self.runtime.readiness()['ready'])
        self.assertEqual(self.runtime.readiness()['reason'], 'audio_cleanup_failed')
        self.now += RETENTION_MS + 1
        self.runtime.cleanup_once()
        self.assertEqual(list((self.root / 'work').glob('*.wav')), [])
        self.assertTrue(self.runtime.readiness()['ready'])
        self.assertEqual(self.wav(device).status_code, 202)
        self.runtime.run_pending_once()
        self.assertEqual(self.status(device).json()['status'], 'sent')

    def test_explicit_delivery_retry_uses_current_config_without_historical_confirmation(self):
        self.config(); device = self.pair(); self.modes['delivery'] = 'fail'
        self.wav(device); self.runtime.run_pending_once()
        self.assertEqual(self.status(device).json()['status'], 'delivery_failed')
        self.assertEqual(self.broker.view()['pin_count'], 0)
        current = self.config('qwen-B', 'siyuan-B'); self.modes['delivery'] = 'ok'
        path = '/admin/api/jobs/' + device['device_id'] + '/1/retry'
        self.assertEqual(self.client.post(path, json={'expected_revision': current['active_revision']}, headers=self.admin_headers).status_code, 202)
        self.runtime.cleanup_once()  # Must not collect a queued retry's pin.
        self.runtime.run_pending_once()
        self.assertEqual(len(self.asr), 1); self.assertEqual(self.deliveries[-1][0], 'siyuan-B')

    def test_uncertain_result_never_retries_until_that_entry_is_resolved(self):
        self.config(); device = self.pair(); self.modes['delivery'] = 'uncertain'
        self.wav(device); self.runtime.run_pending_once()
        self.assertEqual(self.status(device).json()['status'], 'uncertain')
        self.assertEqual(self.broker.view()['pin_count'], 0)
        path = '/admin/api/jobs/' + device['device_id'] + '/1'
        self.assertEqual(self.client.post(path + '/retry', json={}, headers=self.admin_headers).status_code, 409)
        self.restart(); self.assertEqual(len(self.deliveries), 0)
        self.assertEqual(self.client.post(path + '/reconcile', json={'present': True}, headers=self.admin_headers).status_code, 200)
        self.assertEqual(self.status(device).json()['status'], 'sent')
        row = self.runtime.store.get(device['device_id'], 1)
        self.assertIsNone(row['transcript']); self.assertIsNone(row['cloud_content'])

    def test_opus_duplicate_ack_finish_replay_restart_keeps_original_config_and_cleans_audio(self):
        self.config(); device = self.pair(); path, auth = self.stream(device)
        self.config('qwen-B', 'siyuan-B'); self.restart()
        self.runtime.run_pending_once()
        state = self.client.get(path, headers=auth)
        self.assertEqual(state.status_code, 200); self.assertEqual(state.json()['status'], 'sent')
        self.assertEqual(self.deliveries[-1][0], 'siyuan-A')
        self.assertEqual(sum(p.stat().st_size for p in self.runtime.streams.directory.rglob('*.opus')), 0)
        self.assertEqual(self.client.get(path + '/chunks/0', headers=auth).status_code, 405)
        self.assertEqual(self.broker.view()['pin_count'], 0)

    def test_accepted_stream_retry_restart_keeps_new_attempt_and_snapshot(self):
        device = self.pair()
        for index, failure in enumerate(('asr', 'delivery')):
            with self.subTest(failure=failure):
                self.config('qwen-A', 'siyuan-A')
                self.modes = {'asr':'ok', 'delivery':'ok'};self.modes[failure]='fail'
                path, auth = self.stream(device, recording=0x80000001+index)
                self.assertTrue(self.runtime.run_pending_once())
                self.assertEqual(self.broker.view()['pin_count'], 0)
                self.config('qwen-B', 'siyuan-B')
                self.modes = {'asr':'ok', 'delivery':'ok'}
                counts = len(self.asr), len(self.deliveries)
                retry = self.client.post(path+'/retry', json={}, headers=auth)
                self.assertEqual(retry.status_code, 202)
                self.assertEqual(self.broker.view()['pin_count'], 1)
                self.config('qwen-C', 'siyuan-C')
                self.restart()
                self.assertTrue(self.runtime.readiness()['ready'])
                self.assertEqual(self.broker.view()['pin_count'], 1)
                self.assertTrue(self.runtime.run_pending_once())
                self.assertEqual(self.client.get(path, headers=auth).json()['status'], 'sent')
                self.assertEqual(len(self.asr), counts[0]+int(failure=='asr'))
                if failure=='asr':self.assertEqual(self.asr[-1][0], 'qwen-B')
                self.assertEqual(len(self.deliveries), counts[1]+1)
                self.assertEqual(self.deliveries[-1][0], 'siyuan-B')
                self.assertEqual(self.broker.view()['pin_count'], 0)
                self.assertFalse(self.runtime.store.get(device['device_id'],0x80000001+index)['cloud_content'])
                self.assertEqual(sum(p.stat().st_size for p in self.runtime.streams.directory.rglob('*.opus')),0)

    def test_status_queries_do_not_renew_audio_and_expiry_is_enforced(self):
        self.config(); device = self.pair(); path, auth = self.stream(device, finish=False)
        expiry = self.runtime.streams._row(device['device_id'], 0x80000001)['expires_at']
        self.now += RETENTION_MS - 1000
        self.client.get(path, headers=auth)
        self.assertEqual(self.runtime.streams._row(device['device_id'], 0x80000001)['expires_at'], expiry)
        self.now += 2000; self.runtime.cleanup_once()
        self.assertEqual(self.client.get(path, headers=auth).json()['status'], 'audio_expired')
        self.assertEqual(self.client.post(path + '/retry', json={}, headers=auth).status_code, 410)
        self.assertEqual(sum(p.stat().st_size for p in self.runtime.streams.directory.rglob('*.opus')), 0)
        self.assertEqual(self.broker.view()['pin_count'], 0)

    def test_idle_failure_cleanup_does_not_keep_rewriting_history(self):
        self.config(); device = self.pair(); self.modes['asr'] = 'fail'
        self.wav(device); self.runtime.run_pending_once()
        before = self.runtime._job(device['device_id'], 1)['updated_at']
        self.now += 100000
        for _ in range(3): self.runtime.cleanup_once()
        self.assertEqual(self.runtime._job(device['device_id'], 1)['updated_at'], before)

    def test_administrator_and_device_permissions_are_separate_and_retired_routes_are_absent(self):
        self.config(); device = self.pair()
        other = TestClient(self.app, base_url='https://relay.example.org')
        self.addCleanup(other.close)
        auth = {'Authorization': 'Bearer ' + device['candidate_token']}
        self.assertEqual(other.get('/admin/api/config', headers=auth).status_code, 401)
        self.assertEqual(self.client.post('/admin/api/config', json={}, headers=self.origin).status_code, 403)
        for path in ['/v1/enroll', '/v1/migration-grants', '/admin/api/config/activate', '/admin/api/config/discard']:
            self.assertEqual(self.client.post(path, json={}, headers=self.admin_headers).status_code, 404)
        self.assertNotIn('migration', json.dumps(self.client.get('/v1/info').json()))
        bad = self.client.post('/admin/api/config', json={'expected_revision': self.broker.view()['active_revision'], 'patch': {'qwen_model': 'other'}}, headers=self.admin_headers)
        self.assertEqual(bad.status_code, 400)

    def test_subpath_login_assets_and_one_save(self):
        self.client.close(); self.open_client('/memo'); self.login()
        self.assertEqual(self.client.get('/memo/admin/').status_code, 200)
        self.assertEqual(self.client.get('/memo/admin/assets/admin.js').status_code, 200)
        self.config(); self.assertTrue(self.client.get('/memo/health/ready').json()['ready'])

    def test_broker_restart_recovers_automatically_without_user_history_steps(self):
        self.config(); device = self.pair(); self.stream(device, finish=False)
        self.broker.close(); self.broker = LocalBroker(self.root / 'secrets')
        self.runtime.broker = self.broker; self.app.state.broker = self.broker
        # Web's broker closure is rebound by rebuilding the application, as in a restart.
        self.client.close(); self.open_client(); self.login()
        self.assertTrue(self.client.get('/health/ready').json()['ready'])
        self.assertEqual(self.broker.view()['pin_count'], 1)


if __name__ == '__main__':
    unittest.main()
