"""Compile the production config/protocol engine; connect it to the real R02 app.

No ESP, radio or real TLS claim: C ABI storage + HTTP transport are explicit
test substitutes. Server authentication/ledger/enrollment semantics are real.
"""
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import secrets
import subprocess
import sys
import tempfile
import unittest
import uuid
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'server-relay'))
from fastapi.testclient import TestClient
from relay.runtime import Runtime
from relay.web import create_app
from test_support import LocalBroker

class UUID(C.Structure):
    _fields_ = [('bytes', C.c_uint8 * 16)]
class Snapshot(C.Structure):
    _fields_ = [('generation', C.c_uint64), ('enabled', C.c_bool), ('phase', C.c_int),
               ('base_url', C.c_char * 241), ('device_id', C.c_char * 40), ('token', C.c_char * 65),
               ('enrollment_id', C.c_char * 37), ('pairing_code', C.c_char * 17), ('instance', UUID)]
READ = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_char_p, C.POINTER(C.c_uint8), C.POINTER(C.c_size_t))
WRITE = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_char_p, C.POINTER(C.c_uint8), C.c_size_t)
COMMIT = C.CFUNCTYPE(C.c_int, C.c_void_p)
class Storage(C.Structure):
    _fields_ = [('context', C.c_void_p), ('read', READ), ('write', WRITE), ('commit', COMMIT)]
class Config(C.Structure):
    _fields_ = [('storage', Storage), ('value', Snapshot), ('loaded', C.c_bool), ('needs_reload', C.c_bool), ('result', C.c_int)]
REQUEST = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_char_p, C.c_char_p, C.c_char_p, C.c_char_p, C.c_char_p, C.c_void_p, C.c_size_t)
BIND = C.CFUNCTYPE(C.c_int, C.c_void_p, C.POINTER(Snapshot), C.POINTER(UUID))
class PairIO(C.Structure):
    _fields_ = [('context', C.c_void_p), ('request', REQUEST), ('bind', BIND)]
class Receipt(C.Structure):
    _fields_ = [('instance', UUID), ('error_code', C.c_char * 48)]

class Native:
    def __init__(self, lib, durable=b''):
        self.lib, self.durable, self.pending = lib, durable, durable
        self.write_fault = False
        self.bind_busy = False
        self.requests, self.callback_errors = [], []
        def read(_, key, out, length):
            assert key == b'service'
            if not self.durable: return 1
            if length[0] < len(self.durable): return 2
            C.memmove(out, self.durable, len(self.durable)); length[0] = len(self.durable); return 0
        def write(_, key, data, size):
            assert key == b'service'
            self.pending = C.string_at(data, size)
            return 2 if self.write_fault else 0
        def commit(_): self.durable = self.pending; return 0
        self.storage = Storage(None, READ(read), WRITE(write), COMMIT(commit))
        self.cfg = Config()
        assert lib.memo_config_load(C.byref(self.cfg), C.byref(self.storage)) == 0

    def stage(self, url, code, device=b'future-model-unit'):
        return self.lib.memo_config_set_service(C.byref(self.cfg), device, url.encode(), code.encode(),
                                                str(uuid.uuid4()).encode(), secrets.token_hex(32).encode(), False)

    def run(self, fixture, drop=None, alter=None):
        snapshot = Snapshot()
        if self.lib.memo_config_read_snapshot(C.byref(self.cfg), C.byref(snapshot)) != 0: return 4
        def request(_, base, method, path, token, payload, out, capacity):
            try:
                assert base.decode() == fixture.url
                self.requests.append((method.decode(), path.decode(), bool(token)))
                headers={'Authorization': 'Bearer ' + token.decode()} if token else {}
                if payload is not None: headers['Content-Type']='application/json'
                endpoint=C.create_string_buffer(1024)
                assert self.lib.memo_config_join_url(base,path,endpoint,len(endpoint))==0
                response = fixture.client.request(method.decode(), endpoint.value.decode(), content=payload,headers=headers)
                body = response.content
                if alter: body = alter(path.decode(), body)
                C.memset(out, 0, capacity)
                if len(body) >= capacity: return 0
                C.memmove(out, body, len(body))
                if drop and drop(path.decode()): return 0
                return response.status_code
            except Exception as error:
                self.callback_errors.append(str(error)); return 0
        def bind(_, pending, instance):
            if self.bind_busy: return 4  # actual MEMO_CONFIG_BUSY
            result = self.lib.memo_config_bind_instance(C.byref(self.cfg), pending.contents.generation, instance)
            if result == 0: C.memmove(pending, C.byref(self.cfg.value), C.sizeof(Snapshot))
            return result
        self.io = PairIO(None, REQUEST(request), BIND(bind)); receipt = Receipt()
        result = self.lib.memo_pairing_run(snapshot, C.byref(self.io), C.byref(receipt))
        if result == 0:
            saved = self.lib.memo_config_finish_pairing(C.byref(self.cfg), self.cfg.value.generation, C.byref(receipt.instance))
            if saved: result = 4
        elif result in (2, 6):
            saved = self.lib.memo_config_reject_pairing(C.byref(self.cfg), self.cfg.value.generation)
            if saved: result = 4
        return result

class Fixture:
    def __init__(self, directory, url='https://relay.example:9443/memo'):
        self.url = url
        self.root = Path(directory); (self.root/'secrets').mkdir(mode=0o700)
        self.broker = LocalBroker(self.root/'secrets')
        self.runtime = Runtime(self.root/'state.db', self.broker, self.root/'work', self.root/'streams', start_workers=False, min_free_bytes=0)
        self.app = create_app(self.root/'state.db', public_url=self.url, broker=self.broker, runtime=self.runtime)
        parsed=urlsplit(url);authority=parsed.scheme+'://'+parsed.netloc
        self.client = TestClient(self.app, base_url=authority)
        page=self.client.get(parsed.path+'/admin/',follow_redirects=True)
        page.raise_for_status();self.admin_path=urlsplit(str(page.url)).path
        assert self.client.get(self.admin_path+'assets/admin.css').status_code==200
        assert self.client.get(self.admin_path+'assets/admin.js').status_code==200
        origin = {'Origin': authority}
        code = self.app.state.admins.issue_bootstrap()
        self.client.post(self.admin_path+'api/session/initialize',json={'code':code,'password':'synthetic-password-123'},headers=origin).raise_for_status()
        login=self.client.post(self.admin_path+'api/session/login',json={'password':'synthetic-password-123'},headers=origin)
        login.raise_for_status();csrf=login.json()['csrf']
        cookie=login.headers['set-cookie'];assert 'Secure' in cookie and 'HttpOnly' in cookie and 'SameSite=strict' in cookie
        assert 'Path='+self.admin_path.rstrip('/') in cookie
        assert self.client.get(self.admin_path+'api/config').status_code==200
        self.headers = {**origin,'X-CSRF-Token':csrf}
    def code(self):
        response=self.client.post(self.admin_path+'api/devices/pairing-code',json={},headers=self.headers)
        response.raise_for_status();return response.json()['pairing_code']
    def restart(self):
        # Reopen actual persisted server/broker state; no bootstrap, pairing,
        # config save or copying an older database during this simulated restart.
        factory = self.runtime._provider_factory
        self.client.close(); self.runtime.close(); self.broker.close()
        self.broker = LocalBroker(self.root/'secrets')
        self.runtime = Runtime(self.root/'state.db', self.broker, self.root/'work', self.root/'streams', start_workers=False, min_free_bytes=0)
        self.runtime._provider_factory = factory
        self.app = create_app(self.root/'state.db', public_url=self.url, broker=self.broker, runtime=self.runtime)
        parsed=urlsplit(self.url)
        self.client = TestClient(self.app, base_url=parsed.scheme+'://'+parsed.netloc)
    def close(self): self.client.close(); self.runtime.close(); self.broker.close()

class PairingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        (ROOT/'private/host-tests').mkdir(mode=0o700,exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='pairing-',dir=ROOT/'private/host-tests')
        cls.directory = Path(cls.temp.name)
        sdk = ROOT/'.tools/esp-idf'
        assert subprocess.check_output(['git','-C',str(sdk),'rev-parse','HEAD']).decode().strip() == 'fcae32885b0296b32044cb99ecbdc50d98dddb83'
        includes = ['-I',str(ROOT/'main'),'-I',str(sdk/'components/json/cJSON')]
        objects = []
        for source, compiler in [(ROOT/'main/memo_network_config.c','cc'),(ROOT/'main/memo_service_pairing.cpp','c++'),(sdk/'components/json/cJSON/cJSON.c','cc')]:
            obj = cls.directory/(source.stem+'.o');objects.append(str(obj))
            subprocess.run([compiler,'-fPIC','-g','-O1','-Wall','-Wextra','-Werror', '-std=c++17' if compiler=='c++' else '-std=c11', *includes,'-c',str(source),'-o',str(obj)],check=True)
        library = cls.directory/'pairing.so';subprocess.run(['c++','-shared',*objects,'-o',str(library)],check=True)
        cls.lib = C.CDLL(str(library));lib = cls.lib
        lib.memo_config_load.argtypes=[C.POINTER(Config),C.POINTER(Storage)]
        lib.memo_config_set_service.argtypes=[C.POINTER(Config),C.c_char_p,C.c_char_p,C.c_char_p,C.c_char_p,C.c_char_p,C.c_bool]
        lib.memo_config_join_url.argtypes=[C.c_char_p,C.c_char_p,C.c_void_p,C.c_size_t]
        lib.memo_config_read_snapshot.argtypes=lib.memo_config_credentials.argtypes=[C.POINTER(Config),C.POINTER(Snapshot)]
        lib.memo_config_bind_instance.argtypes=lib.memo_config_finish_pairing.argtypes=[C.POINTER(Config),C.c_uint64,C.POINTER(UUID)]
        lib.memo_config_reject_pairing.argtypes=[C.POINTER(Config),C.c_uint64]
        lib.memo_pairing_run.argtypes=[Snapshot,C.POINTER(PairIO),C.POINTER(Receipt)]
    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()
    def setUp(self):
        self.temp_case = tempfile.TemporaryDirectory(dir=self.directory)
        self.fixture = Fixture(self.temp_case.name)
        self.addCleanup(self.temp_case.cleanup);self.addCleanup(self.fixture.close)
        self.native = Native(self.lib)
        self.assertEqual(self.native.stage(self.fixture.url,self.fixture.code()),0)
    def verify(self, native):
        self.assertEqual(native.callback_errors,[])
        self.assertEqual(native.cfg.value.phase,2)
        self.assertFalse(native.cfg.value.pairing_code)
        self.assertFalse(native.cfg.value.enrollment_id)
        snapshot=Snapshot();self.assertEqual(self.lib.memo_config_credentials(C.byref(native.cfg),C.byref(snapshot)),0)
        response=self.fixture.client.get(self.fixture.url+'/v1/device-info',headers={'X-Device-ID':snapshot.device_id.decode(),'Authorization':'Bearer '+snapshot.token.decode()})
        self.assertEqual(response.status_code,200)
        self.assertNotIn(snapshot.token.decode(),response.text)
        with self.fixture.runtime.store.connect() as db:
            self.assertEqual(db.execute('SELECT COUNT(*) FROM device_enrollments').fetchone()[0],1)
            self.assertEqual(db.execute('SELECT phase FROM device_enrollments').fetchone()[0],'finalized')
        self.assertNotIn(snapshot.token, (self.fixture.root/'state.db').read_bytes())
    def lost(self, suffix):
        value=self.native.cfg.value;identity=(value.enrollment_id,value.token)
        self.assertEqual(self.native.run(self.fixture,drop=lambda path:path.endswith(suffix)),1)
        reboot=Native(self.lib,self.native.durable)
        self.assertEqual((reboot.cfg.value.enrollment_id,reboot.cfg.value.token),identity)
        self.assertEqual(reboot.run(self.fixture),0);self.verify(reboot)
    def test_real_fresh_pair_and_credentials_after_reboot(self):
        self.assertEqual(self.native.run(self.fixture),0);self.verify(Native(self.lib,self.native.durable))
    def test_lost_create_response_then_reboot(self): self.lost('/v1/enrollments')
    def test_lost_activate_response_then_reboot(self): self.lost('/activate')
    def test_lost_finalize_response_then_reboot(self): self.lost('/finalize')
    def test_audio_busy_binding_never_activates_before_durable_binding(self):
        self.native.bind_busy=True;self.assertEqual(self.native.run(self.fixture),5)
        self.assertFalse(any(p.endswith('/activate') for _,p,_ in self.native.requests))
        self.native.bind_busy=False;self.assertEqual(self.native.run(self.fixture),0);self.verify(self.native)
    def test_failed_binding_write_stops_http_and_recovers_after_reload(self):
        self.native.write_fault=True;self.assertEqual(self.native.run(self.fixture),4)
        self.assertFalse(any(p.endswith('/activate') for _,p,_ in self.native.requests))
        reboot=Native(self.lib,self.native.durable);self.assertEqual(reboot.run(self.fixture),0);self.verify(reboot)
    def test_final_local_write_failure_replays_finalized_server_after_reload(self):
        def fail_final_commit(path,body):
            if path.endswith('/finalize'):self.native.write_fault=True
            return body
        self.assertEqual(self.native.run(self.fixture,alter=fail_final_commit),4)
        snapshot=Snapshot();self.assertEqual(self.lib.memo_config_credentials(C.byref(self.native.cfg),C.byref(snapshot)),2)
        reboot=Native(self.lib,self.native.durable)
        self.assertEqual(reboot.run(self.fixture),0);self.verify(reboot)
    def test_new_address_uses_new_candidate_without_original_authorization(self):
        self.assertEqual(self.native.run(self.fixture),0);original=self.native.cfg.value.token
        directory=tempfile.TemporaryDirectory(dir=self.directory)
        other=Fixture(directory.name,'https://second.example:9443/memo')
        self.addCleanup(directory.cleanup);self.addCleanup(other.close)
        self.assertEqual(self.native.stage(other.url,other.code()),0)
        self.assertNotEqual(self.native.cfg.value.token,original)
        self.assertEqual(self.native.run(other),0)
        self.assertEqual(self.native.callback_errors,[])
    def test_mismatching_device_response_cannot_publish_success(self):
        def alter(path,body):
            value=json.loads(body)
            if path=='/v1/enrollments': value['device_id']='another-device'
            return json.dumps(value).encode()
        self.assertEqual(self.native.run(self.fixture,alter=alter),3)
        self.assertEqual(self.native.cfg.value.phase,1)
        self.assertFalse(any(p.endswith('/activate') for _,p,_ in self.native.requests))
        self.assertEqual(self.native.run(self.fixture),0);self.verify(self.native)
    def test_instance_replacement_sends_no_old_candidate_to_new_server(self):
        self.assertEqual(self.native.run(self.fixture,drop=lambda p:p.endswith('/activate')),1)
        directory=tempfile.TemporaryDirectory(dir=self.directory);other=Fixture(directory.name)
        self.addCleanup(directory.cleanup);self.addCleanup(other.close)
        before=len(self.native.requests);self.assertEqual(self.native.run(other),6)
        self.assertEqual(self.native.requests[before:],[('GET','/v1/info',False)])
        self.assertEqual(self.native.cfg.value.phase,0)
        self.assertFalse(self.native.cfg.value.token)
        self.assertEqual(self.native.stage(other.url,other.code()),0)
        self.assertEqual(self.native.run(other),0)
    def test_invalid_code_stops_automatic_pairing_but_offline_remains(self):
        native=Native(self.lib);self.assertEqual(native.stage(self.fixture.url,'23456789AB'),0)
        self.assertEqual(native.run(self.fixture),2);self.assertEqual(native.cfg.value.phase,0)
        snapshot=Snapshot();self.assertEqual(self.lib.memo_config_credentials(C.byref(native.cfg),C.byref(snapshot)),5)
        self.assertEqual(native.stage(self.fixture.url,self.fixture.code()),0)
        self.assertEqual(native.run(self.fixture),0);self.verify(native)
    def encoded(self,prefix):
        directory=tempfile.TemporaryDirectory(dir=self.directory)
        fixture=Fixture(directory.name,'https://relay.example:9443'+prefix)
        self.addCleanup(directory.cleanup);self.addCleanup(fixture.close)
        native=Native(self.lib);self.assertEqual(native.stage(fixture.url,fixture.code()),0)
        self.assertEqual(native.run(fixture),0);self.assertEqual(native.callback_errors,[])
        snapshot=Snapshot();self.assertEqual(self.lib.memo_config_credentials(C.byref(native.cfg),C.byref(snapshot)),0)
        self.assertEqual(fixture.client.get(fixture.url+'/v1/device-info',headers={
            'X-Device-ID':snapshot.device_id.decode(),'Authorization':'Bearer '+snapshot.token.decode()}).status_code,200)
        for suffix in ('/','/admin'):
            redirect=fixture.client.get(fixture.url+suffix,follow_redirects=False)
            self.assertEqual(redirect.status_code,307)
            self.assertEqual(redirect.headers['location'],fixture.admin_path)
        # The HTML redirect must not loop on the internal /admin/ alias.
        # Publication proxy/ASGI root-path configuration is verified in R07.
        self.assertEqual(fixture.client.get('/admin/',follow_redirects=False).status_code,200)
        for path in ('/v1/info',fixture.url+'/v1/info',fixture.url+'/administrator'):
            self.assertNotIn('cookie',fixture.client.build_request('GET',path).headers)
        response=fixture.client.get(fixture.admin_path+'api/session');self.assertTrue(response.json()['authenticated'])
        self.assertEqual(fixture.client.get(fixture.url+'/v1/info').status_code,200)
        self.assertEqual(fixture.client.get(fixture.url+'/health/ready').status_code,503)
        self.assertEqual(fixture.client.post(fixture.admin_path+'api/config/validate',json={'patch':{}},
            headers={**fixture.headers,'Origin':'https://wrong.example'}).status_code,403)
        self.assertEqual(fixture.client.post(fixture.admin_path+'api/config/validate',json={'patch':{}},
            headers={**fixture.headers,'X-CSRF-Token':'wrong'}).status_code,403)
        logout=fixture.client.post(fixture.admin_path+'api/session/logout',json={},headers=fixture.headers)
        self.assertEqual(logout.status_code,200)
        self.assertFalse(fixture.client.get(fixture.admin_path+'api/session').json()['authenticated'])
    def test_encoded_ascii_prefix_management_and_device_pairing(self): self.encoded('/%61')
    def test_encoded_utf8_prefix_management_and_device_pairing(self): self.encoded('/%E8%AF%AD%E9%9F%B3')
    def test_raw_utf8_prefix_management_and_device_pairing(self): self.encoded('/语音')
    def test_root_prefix_management_and_device_pairing(self): self.encoded('')

if __name__=='__main__': unittest.main(verbosity=2)
