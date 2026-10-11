#!/usr/bin/env python3
"""Isolated R09 Compose/TLS/protocol cases; no device or real provider calls.

Read-only synthetic callbacks are mounted over the shipped runtime entrypoint.
Production CLI, HTTP, codecs, stores, broker and deployment operations still run.
All raw requests, credentials and evidence stay in the generated private run.
"""
from pathlib import Path
import argparse
import concurrent.futures
from contextlib import closing
import hashlib
import http.client
from http.cookies import SimpleCookie
import io
import json
import os
import secrets
import socket
import ssl
import subprocess
import sys
import time
import uuid
import wave

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'deploy'))
from manage import Deployment, atomic_json_or_env, run

DEVICE = 'container-synthetic-device'
FORMAT = {'format': 'opus', 'sample_rate': 16000, 'channels': 1, 'frame_ms': 20, 'bitrate': 24000}


def require(value, message):
    if not value:
        raise AssertionError(message)


def port():
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        return listener.getsockname()[1]


def poll(callback, timeout=60):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            value = callback()
            if value:
                return value
        except (ConnectionError, TimeoutError, http.client.HTTPException, ssl.SSLEOFError):
            pass
        time.sleep(.1)
    raise AssertionError('bounded condition did not converge')


def wav():
    out = io.BytesIO()
    with wave.open(out, 'wb') as audio:
        audio.setparams((1, 2, 16000, 0, 'NONE', 'not compressed'))
        audio.writeframes(b'\0' * 32000)
    return out.getvalue()


class TestDeployment(Deployment):
    def __init__(self, values, env_path, overlay):
        super().__init__(values, env_path)
        self.overlay = overlay

    def compose(self, *arguments, values=None, json_output=False):
        environment = {k: v for k, v in os.environ.items() if not k.startswith('VOICE_MEMO_')}
        environment.update(values or self.values)
        return run(['docker', 'compose', '--project-directory', str(ROOT / 'deploy'),
                    '--env-file', '/dev/null', '-p', self.project,
                    '-f', str(ROOT / 'deploy/compose.yaml'), '-f', str(self.overlay),
                    *arguments], env=environment, json_output=json_output)


class Lab:
    def __init__(self, directory, image, platform, previous=None):
        self.directory, self.image, self.platform, self.previous = directory, image, platform, previous
        self.project = 'r09-' + uuid.uuid4().hex[:8] + '-' + platform.split('/')[1]
        self.tls_port, self.http_port, self.probe_port = port(), port(), port()
        require(len({self.tls_port, self.http_port, self.probe_port}) == 3, 'port collision')
        self.prefix = ''
        self.origin = 'https://localhost:' + str(self.tls_port)
        self.overlay = directory / 'compose-test.json'
        self.cookie = self.csrf = self.token = ''
        self.cases = {}
        self.values = {'VOICE_MEMO_IMAGE': previous or image, 'VOICE_MEMO_PROJECT': self.project,
                       'VOICE_MEMO_PUBLIC_URL': self.origin, 'VOICE_MEMO_PORT': str(self.http_port),
                       'VOICE_MEMO_PROXY': '0', 'VOICE_MEMO_SUBNET': '172.29.139.0/24',
                       'VOICE_MEMO_GATEWAY': '172.29.139.1', 'VOICE_MEMO_RELAY_IP': '172.29.139.2',
                       'VOICE_MEMO_PROXY_IP': '172.29.139.3'}
        self.env_path = directory / 'relay.env'
        self.deployment = TestDeployment(self.values, self.env_path, self.overlay)

    def record(self, name, value):
        self.cases[name] = value
        atomic_json_or_env(self.directory / 'results.json', self.cases)

    def certs(self):
        folder = self.directory / 'tls-source'
        folder.mkdir(mode=0o700)
        def openssl(*args):
            r = subprocess.run(['openssl', *args], cwd=folder, capture_output=True)
            require(r.returncode == 0, 'test certificate generation failed')
        openssl('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '2',
                '-subj', '/CN=VoiceMemo isolated CA', '-keyout', 'ca.key', '-out', 'ca.pem')
        openssl('req', '-newkey', 'rsa:2048', '-nodes', '-subj', '/CN=localhost',
                '-keyout', 'private-key.pem', '-out', 'server.csr')
        for name, san in [('certificate.pem', 'DNS:localhost,IP:127.0.0.1'), ('wrong.pem', 'DNS:wrong.invalid')]:
            (folder / 'extensions.cnf').write_text('subjectAltName=' + san + '\nextendedKeyUsage=serverAuth\n')
            openssl('x509', '-req', '-in', 'server.csr', '-CA', 'ca.pem', '-CAkey', 'ca.key',
                    '-CAcreateserial', '-days', '2', '-extfile', 'extensions.cnf', '-out', name)
        for path in folder.iterdir():
            path.chmod(0o600)
        self.context = ssl.create_default_context(cafile=str(folder / 'ca.pem'))
        self.install_cert('certificate.pem')

    def install_cert(self, filename):
        # Root-owned 0600 test files are created on the actual Linux named volume;
        # no host chown/sudo or global certificate trust changes are attempted.
        volume = self.project + '_tls-test'
        run(['docker', 'volume', 'create', volume])
        code = "from pathlib import Path; import os; p=Path('/tls'); p.chmod(0o700); "
        code += "[(p/n).write_bytes(Path('/input/'+s).read_bytes()) for n,s in [('certificate.pem','" + filename + "'),('private-key.pem','private-key.pem')]]; "
        code += "[(p/n).chmod(0o600) for n in ('certificate.pem','private-key.pem')]"
        run(['docker', 'run', '--rm', '--platform', self.platform, '--network', 'none', '--read-only',
             '--user', '0:0', '--cap-drop', 'ALL', '--security-opt', 'no-new-privileges:true',
             '--mount', 'type=volume,src=' + volume + ',dst=/tls',
             '--mount', 'type=bind,src=' + str(self.directory / 'tls-source') + ',dst=/input,readonly',
             '--entrypoint', 'python', self.image, '-c', code])

    def write_compose(self, *, fixture=True):
        self.values['VOICE_MEMO_PUBLIC_URL'] = self.origin + self.prefix
        atomic_json_or_env(self.env_path, self.values, env_format=True)
        proxy = json.loads((ROOT / 'deploy/compose.proxy.yaml').read_text())['services']['proxy']
        proxy.update(platform=self.platform, restart='no', ports=[f'127.0.0.1:{self.tls_port}:8443'],
                     volumes=[str(ROOT / 'deploy/nginx-container.conf') + ':/etc/nginx/voice-memo.conf:ro',
                              'tls-test:/etc/voice-memo-tls:ro'])
        relay = {'platform': self.platform, 'restart': 'no'}
        if fixture:
            relay.update(entrypoint=['python', '/fixture.py'], command=[],
                         environment={'VOICE_MEMO_FIXTURE_ISOLATED': '1'},
                         volumes=[str(ROOT / 'tests/container_fixture.py') + ':/fixture.py:ro'],
                         ports=[f'127.0.0.1:{self.probe_port}:8766'])
        overlay = {'services': {'relay': relay, 'config-broker': {'platform': self.platform},
                               'volume-init': {'platform': self.platform}, 'proxy': proxy},
                   'volumes': {'tls-test': {}}}
        atomic_json_or_env(self.overlay, overlay)
        self.deployment.values = dict(self.values)

    def request(self, method, path, body=None, *, headers=None, expect=(200,), context=None):
        h = {'Origin': self.origin}
        if self.cookie:
            h['Cookie'], h['X-CSRF-Token'] = self.cookie, self.csrf
        if self.token:
            h['X-Device-ID'], h['Authorization'] = DEVICE, 'Bearer ' + self.token
        if isinstance(body, dict):
            body = json.dumps(body).encode(); h['Content-Type'] = 'application/json'
        h.update(headers or {})
        with closing(http.client.HTTPSConnection('localhost', self.tls_port, context=context or self.context, timeout=10)) as c:
            c.request(method, self.prefix + path, body=body, headers=h)
            r = c.getresponse(); payload = r.read(); response_headers = {k.lower(): v for k, v in r.getheaders()}
            require(r.status in expect, f'{method} {path}: {r.status} {payload[:200]!r}')
            value = json.loads(payload) if 'application/json' in r.getheader('Content-Type', '') else payload
            return r.status, value, response_headers

    def probe(self, path, body=None):
        with closing(http.client.HTTPConnection('127.0.0.1', self.probe_port, timeout=10)) as c:
            c.request('POST' if body is not None else 'GET', path,
                      json.dumps(body).encode() if body is not None else None,
                      {'Content-Type': 'application/json'})
            r = c.getresponse(); data = r.read(); require(r.status in (200, 204), 'fixture probe failed')
            return json.loads(data) if data else None

    def inspect(self, number):
        return self.probe('/inspect/' + str(number))

    def terminal(self, number, status='sent'):
        def done():
            s = self.inspect(number)
            return s if s['memo'] and s['memo']['status'] == status and not s['stream_files'] else None
        return poll(done)

    def login(self):
        self.cookie = self.csrf = ''
        _, session, h = self.request('POST', '/admin/api/session/login', {'password': 'Synthetic-password-2026'})
        cookies = SimpleCookie(h['set-cookie']); self.cookie = '; '.join(k + '=' + v.value for k, v in cookies.items())
        self.csrf = session['csrf']
        require('Secure' in h['set-cookie'] and 'HttpOnly' in h['set-cookie'], 'cookie restrictions absent')

    def configure(self, label):
        view = self.request('GET', '/admin/api/config')[1]
        patch = {k: {'op': 'replace', 'value': 'synthetic-' + label + '-' + k} for k in ('qwen_api_key', 'siyuan_token')}
        saved = self.request('POST', '/admin/api/config', {'expected_revision': view['active_revision'], 'patch': patch})[1]
        require('synthetic-' not in json.dumps(saved), 'configuration echoed secrets')
        return saved['active_revision']

    def initialize(self):
        d = self.deployment
        state = d.start(); require(state['reason'] == 'not_configured', 'empty service readiness inaccurate')
        self.request('GET', '/admin/'); self.request('GET', '/admin/assets/admin.css')
        require(not self.request('GET', '/admin/api/session')[1]['initialized'], 'not an empty install')
        code = d.compose('exec', '-T', 'relay', 'relay-admin', 'bootstrap').strip()
        self.request('POST', '/admin/api/session/initialize', {'code': code, 'password': 'Synthetic-password-2026'})
        self.login(); self.revision = self.configure('A')
        pair = self.request('POST', '/admin/api/devices/pairing-code', {})[1]['pairing_code']
        self.token = secrets.token_hex(32); transaction = str(uuid.uuid4())
        enrollment = {'device_id': DEVICE, 'pairing_code': pair, 'enrollment_id': transaction, 'candidate_token': self.token}
        first = self.request('POST', '/v1/enrollments', enrollment, expect=(201,))[1]
        replay = self.request('POST', '/v1/enrollments', enrollment, expect=(201,))[1]
        require(first == replay, 'enrollment replay changed transaction state')
        for operation in ('activate', 'finalize'):
            self.request('POST', '/v1/enrollments/' + transaction + '/' + operation, {})
        require(self.request('GET', '/v1/device-info')[1]['device_id'] == DEVICE, 'pairing did not persist')
        self.record('empty_install', {'status': 'PASS', 'initial_readiness': 'not_configured', 'cli_admin_http_config_pairing': True})

    def upload_wav(self, number):
        data = wav()
        return self.request('POST', '/v1/memos', data, headers={'X-Recording-ID': str(number),
                            'X-Audio-SHA256': hashlib.sha256(data).hexdigest(), 'Content-Type': 'audio/wav'}, expect=(202,))

    def open_stream(self, number):
        self.request('POST', '/v1/streams', FORMAT, headers={'X-Recording-ID': str(number)}, expect=(201,))

    def chunk(self):
        with closing(http.client.HTTPConnection('127.0.0.1', self.probe_port, timeout=10)) as c:
            c.request('GET', '/chunk'); r = c.getresponse(); require(r.status == 200, 'codec fixture unavailable')
            return r.read()

    def put(self, number, data, *, expect=(201,)):
        return self.request('PUT', f'/v1/streams/{DEVICE}/{number}/chunks/0', data,
                            headers={'X-Chunk-SHA256': hashlib.sha256(data).hexdigest(), 'Content-Type': 'application/octet-stream'}, expect=expect)

    def finish(self, number, *, expect=(202,)):
        return self.request('POST', f'/v1/streams/{DEVICE}/{number}/finish',
                            {'chunks': 1, 'samples': 80000, 'interrupted': False, 'reason': ''}, expect=expect)

    def restart(self, force=False):
        self.deployment.compose('up', '-d', *(['--force-recreate'] if force else []), '--wait', '--wait-timeout', '120')
        poll(lambda: self.request('GET', '/v1/device-info'))
        require(self.request('GET', '/admin/api/session')[1]['authenticated'], 'restart lost administrator session')

    def exercise(self):
        self.deployment.image_check(self.image)
        if self.previous:
            self.deployment.image_check(self.previous)
        self.certs(); self.write_compose(); self.initialize()
        self.upload_wav(1); self.terminal(1)
        before = self.deployment.control('status')['instance_uuid']
        self.deployment.compose('restart', 'relay', 'config-broker'); self.restart()
        require(self.deployment.control('status')['instance_uuid'] == before, 'restart changed instance')
        self.restart(force=True)
        require(self.request('GET', '/v1/memos/' + DEVICE + '/1')[1]['status'] == 'sent', 'rebuild lost success')
        require(self.request('GET', '/admin/api/config')[1]['active_revision'] == self.revision, 'rebuild lost config')
        self.record('restart_recreate', {'status': 'PASS', 'config_device_instance_success_preserved': True})
        if self.previous:
            result = self.deployment.switch(self.image)
            require(result['previous_image'] == self.previous and result['database_restored'] is False, 'wrong update')
            self.values['VOICE_MEMO_IMAGE'] = self.image
            require(self.request('GET', '/admin/api/session')[1]['authenticated'], 'update lost administrator session')
            require(self.request('GET', '/v1/memos/' + DEVICE + '/1')[1]['status'] == 'sent', 'update lost success')
            require(self.request('GET', '/admin/api/config')[1]['active_revision'] == self.revision, 'update lost config')
            self.record('compatible_code_update', dict(status='PASS', **result))
        for prefix in ('', '/memo'):
            self.prefix = prefix; self.write_compose(); self.restart(force=True)
            self.request('GET', '/admin/', headers={'X-Forwarded-Proto': 'http', 'X-Forwarded-For': 'untrusted.invalid'})
            _, _, redirect = self.request('GET', '/admin', expect=(307,))
            require(redirect['location'] == prefix + '/admin/', 'prefix redirect mismatch')
            self.upload_wav(2 if not prefix else 3); s = self.terminal(2 if not prefix else 3)
            require(sum(e['kind'] == 'delivery' for e in s['events']) == 1 and not s['memo']['cloud_content'], 'duplicate/content retained')
        for context in (ssl.create_default_context(),):
            try:
                self.request('GET', '/v1/info', context=context)
            except ssl.SSLCertVerificationError:
                pass
            else:
                raise AssertionError('untrusted CA accepted')
        self.install_cert('wrong.pem'); self.deployment.compose('restart', 'proxy')
        def rejected():
            try:
                self.request('GET', '/v1/info')
            except ssl.SSLCertVerificationError:
                return True
            except ssl.SSLEOFError:
                # Proxy restart can close an early handshake. Only an actual
                # certificate-verification error qualifies the negative case.
                return False
            return False
        poll(rejected)
        self.install_cert('certificate.pem'); self.deployment.compose('restart', 'proxy')
        poll(lambda: self.request('GET', '/v1/info'))
        self.record('tls', {'status': 'PASS', 'root_subpath_upload': True, 'unknown_ca_and_wrong_san_rejected': True,
                            'trust': 'explicit isolated CA; no global trust changes'})
        self.protocol_faults()
        self.permissions()
        self.retention()
        self.disk_fault()
        self.request('GET', '/v1/device-info', headers={'Authorization': 'Bearer ' + '0' * 64}, expect=(401,))
        self.request('POST', '/admin/api/devices/pairing-code', {}, headers={'Origin': 'https://wrong.invalid'}, expect=(403,))
        self.request('POST', f'/admin/api/devices/{DEVICE}/revoke', {})
        self.request('GET', '/v1/device-info', expect=(401,))
        self.record('http_authority', {'status': 'PASS', 'invalid_and_revoked_token_denied': True, 'cross_origin_write_denied': True})

    def protocol_faults(self):
        data = self.chunk(); self.open_stream(10)
        self.probe('/arm', {'phase': 'chunk_ack', 'recording': 10})
        with concurrent.futures.ThreadPoolExecutor() as pool:
            future = pool.submit(self.put, 10, data)
            poll(lambda: any(e['kind'] == 'gate:chunk_ack' for e in self.inspect(10)['events']))
            self.deployment.compose('kill', '-s', 'SIGKILL', 'relay')
            try:
                future.result()
            except (ConnectionError, TimeoutError, http.client.HTTPException, AssertionError):
                pass
            else:
                raise AssertionError('armed ACK did not lose its response')
        self.restart(); state = self.inspect(10)
        atomic_json_or_env(self.directory / 'opus-durable-prefix.json', state)
        digest = hashlib.sha256(data).hexdigest()
        require(state['stream']['next_seq'] == 1 and state['stream']['received_samples'] == 80000 and
                len(state['chunks']) == 1 and state['chunks'][0]['seq'] == 0 and
                state['chunks'][0]['sha256'] == state['chunks'][0]['file_sha256'] == digest, 'durable ACK lost')
        self.put(10, data, expect=(200,))
        changed = bytearray(data); changed[-1] ^= 1
        self.put(10, bytes(changed), expect=(409,))
        self.probe('/arm', {'phase': 'finish_ack', 'recording': 10})
        with concurrent.futures.ThreadPoolExecutor() as pool:
            future = pool.submit(self.finish, 10)
            poll(lambda: any(e['kind'] == 'gate:finish_ack' for e in self.inspect(10)['events']))
            self.deployment.compose('kill', '-s', 'SIGKILL', 'relay')
            try:
                future.result()
            except (ConnectionError, TimeoutError, http.client.HTTPException, AssertionError):
                pass
            else:
                raise AssertionError('armed finish did not lose its response')
        self.restart(); self.finish(10, expect=(200, 202)); sent = self.terminal(10)
        atomic_json_or_env(self.directory / 'opus-sent.json', sent)
        require(sum(e['kind'] == 'delivery' for e in sent['events']) == 1, 'finish replay duplicated delivery')
        self.record('opus_lost_ack_finish', {'status': 'PASS', 'actual_sigkill': True, 'prefix_hash_samples_preserved': True,
                                           'duplicate_chunk_conflict_and_single_delivery': True})
        for phase, number in [('transcribing', 11), ('transcribed', 12), ('sending', 13)]:
            self.probe('/arm', {'phase': phase, 'recording': number}); self.upload_wav(number)
            poll(lambda: any(e['kind'] == 'gate:' + phase for e in self.inspect(number)['events']))
            atomic_json_or_env(self.directory / f'wav-{phase}-before.json', self.inspect(number))
            self.deployment.compose('kill', '-s', 'SIGKILL', 'relay'); self.restart()
            expected = 'uncertain' if phase == 'sending' else 'sent' if phase == 'transcribed' else 'transcription_failed'
            poll(lambda: self.inspect(number)['memo']['status'] == expected)
            state = self.inspect(number)
            atomic_json_or_env(self.directory / f'wav-{phase}-recovered.json', state)
            if expected == 'uncertain':
                self.request('POST', f'/v1/memos/{DEVICE}/{number}/retry', {}, expect=(409,))
                require(sum(e['kind'] == 'delivery' for e in state['events']) == 1, 'uncertain duplicated')
                self.request('POST', f'/admin/api/jobs/{DEVICE}/{number}/reconcile', {'present': True})
            elif expected == 'transcription_failed':
                self.upload_wav(number); self.terminal(number)
            completed = self.terminal(number)
            require(sum(e['kind'] == 'delivery' for e in completed['events']) == 1 and
                    not completed['memo']['cloud_content'] and not completed['memo']['transcript'], 'completed WAV content/send mismatch')
            if phase == 'transcribed':
                require(sum(e['kind'] == 'asr' for e in completed['events']) == 1, 'durable text retranscribed')
            atomic_json_or_env(self.directory / f'wav-{phase}-completed.json', completed)
            self.record('wav_kill_' + phase, {'status': 'PASS', 'recovered_status': expected})

    def permissions(self):
        d = self.deployment
        identifier = d.compose('ps', '-q', 'relay')
        info = run(['docker', 'inspect', identifier], json_output=True)[0]
        atomic_json_or_env(self.directory / 'relay-inspect.json', info)
        require(info['Config']['User'] == '10001:10001' and info['HostConfig']['ReadonlyRootfs'], 'relay restrictions missing')
        require(not any(m['Destination'] == '/var/lib/voice-memo-secrets' for m in info['Mounts']), 'secret volume leaked')
        code = "import socket,json; s=socket.socket(socket.AF_UNIX); s.connect('/run/voice-memo/config.sock'); r=json.loads(s.recv(1024)); assert r['error']['code']=='unauthorized_peer'; print('peer rejected')"
        require(d.compose('exec', '-T', '--user', '10002:10001', 'relay', 'python', '-c', code) == 'peer rejected', 'UID gate absent')
        code = "from config_broker import BrokerClient; c=BrokerClient(); assert c.view()['active_revision']; print('allowed peer')"
        require(d.compose('exec', '-T', 'relay', 'python', '-c', code) == 'allowed peer', 'real relay UID denied')
        stats = d.compose('exec', '-T', 'config-broker', 'python', '-c',
                          "import os,stat,json; from pathlib import Path; p=Path('/var/lib/voice-memo-secrets'); assert p.stat().st_uid==0 and stat.S_IMODE(p.stat().st_mode)==0o700; assert all(x.stat().st_uid==0 and stat.S_IMODE(x.stat().st_mode)==0o600 for x in p.iterdir() if x.is_file()); print('root-only secrets')")
        require(stats == 'root-only secrets', 'secrets permissions incorrect')
        self.record('permissions', {'status': 'PASS', 'linux_so_peercred_allow_10001_deny_10002': True,
                                    'root_0700_0600_secrets_no_relay_mount': True, 'environment': 'Docker Desktop native Linux VM named volumes'})

    def retention(self):
        data = self.chunk()
        for number, mode in [(20, 'reject'), (21, 'asr_fail')]:
            self.probe('/provider-mode', {'recording': number, 'mode': mode})
            self.open_stream(number); self.put(number, data); self.finish(number)
        reject = self.terminal(20, 'transcription_rejected')
        require(not any(e['kind'] == 'delivery' for e in reject['events']), 'rejection delivered')
        poll(lambda: self.inspect(21)['memo'] and self.inspect(21)['memo']['status'] == 'transcription_failed')
        expiry = self.inspect(21)['stream']['expires_at']
        for _ in range(3):
            self.request('GET', f'/v1/streams/{DEVICE}/21')
            require(self.inspect(21)['stream']['expires_at'] == expiry, 'query extended retention')
        self.open_stream(22); self.put(22, data)
        self.open_stream(23); self.put(23, data)
        self.probe('/cleanup-fault', {'recording': 23, 'enabled': True})
        self.probe('/clock', {'advance_ms': 86400000 + 1000})
        for number in (21, 22):
            poll(lambda n=number: self.inspect(n)['stream']['phase'] == 'audio_expired' and not self.inspect(n)['stream_files'])
        fault = poll(lambda: self.inspect(23) if not self.inspect(23)['cleanup_ok'] else None)
        require(fault['stream_files'], 'failure did not retain audio')
        self.request('GET', '/health/ready', expect=(503,))
        self.deployment.compose('kill', '-s', 'SIGKILL', 'relay'); self.restart()
        poll(lambda: self.inspect(23)['stream']['phase'] == 'audio_expired' and not self.inspect(23)['stream_files'])
        self.request('POST', f'/v1/streams/{DEVICE}/23/retry', {}, expect=(410,))
        self.record('retention_cleanup', {'status': 'PASS', 'success_rejection_cleanup': True,
                                         'failed_interrupted_expire_status_queries_do_not_extend': True,
                                         'removal_fault_not_ready_then_startup_compensates': True,
                                         'clock': 'explicit persistent offset, not 24h wall-clock test'})

    def cleanup(self):
        require(self.project.startswith('r09-'), 'refuse unrelated cleanup')
        self.deployment.compose('down', '--remove-orphans')
        for suffix in ('data', 'work', 'secrets', 'broker-socket', 'tls-test'):
            name = self.project + '_' + suffix
            run(['docker', 'volume', 'rm', name])

    def disk_fault(self):
        # A separate bounded tmpfs is the *actual* WAV work filesystem here.
        # Persistent current DB/streams stay on their ordinary named volume.
        overlay = json.loads(self.overlay.read_text())
        overlay['services']['relay']['environment'].update(VOICE_MEMO_WORK_DIR='/fault-work', TMPDIR='/fault-work')
        overlay['services']['relay']['tmpfs'] = ['/fault-work:rw,noexec,nosuid,size=96m,uid=10001,gid=10001,mode=0700']
        atomic_json_or_env(self.overlay, overlay)
        self.restart(force=True)
        d = self.deployment
        code = "from pathlib import Path; p=Path('/fault-work/filler'); f=p.open('wb'); [f.write(b'x'*1048576) for _ in range(40)]; f.close(); print('allocated 40 MiB')"
        d.compose('exec', '-T', 'relay', 'python', '-c', code)
        data = wav()
        response = self.request('POST', '/v1/memos', data, headers={'X-Recording-ID': '30',
                                'X-Audio-SHA256': hashlib.sha256(data).hexdigest(), 'Content-Type': 'audio/wav'}, expect=(507,))[1]
        require(response['error'] == 'storage_full' and not self.inspect(30)['job'], 'space rejection created an accepted job')
        d.compose('exec', '-T', 'relay', 'python', '-c', "from pathlib import Path; Path('/fault-work/filler').unlink()")
        self.upload_wav(30); self.terminal(30)
        self.record('disk_capacity', {'status': 'PASS', 'real_tmpfs_96MiB_filler_40MiB': True,
                                      '507_before_task_acceptance_then_recovery': True,
                                      'scope': 'actual insufficient work-space branch, not full host disk exhaustion'})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', required=True)
    parser.add_argument('--platform', required=True, choices=('linux/arm64', 'linux/amd64'))
    parser.add_argument('--previous-image', help='Different real compatible source; not a relabeled current image')
    args = parser.parse_args()
    out = ROOT / 'private/container-acceptance' / (time.strftime('%Y%m%dT%H%M%S') + '-' + uuid.uuid4().hex[:8])
    out.mkdir(mode=0o700, parents=True)
    lab = Lab(out, args.image, args.platform, args.previous_image)
    result = 0
    try:
        lab.exercise()
        lab.record('selected_synthetic_status', 'PASS')
        print('Synthetic container cases complete; raw evidence: ' + str(out.relative_to(ROOT)))
    except Exception:
        import traceback
        (out / 'failure.log').write_text(traceback.format_exc())
        lab.record('selected_synthetic_status', 'FAIL')
        print('Container cases failed; inspect ' + str(out.relative_to(ROOT)), file=sys.stderr)
        result = 1
    finally:
        try:
            lab.cleanup()
        except Exception:
            lab.record('cleanup_required', True)
            result = 1
    return result


if __name__ == '__main__':
    raise SystemExit(main())
