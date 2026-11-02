"""Shared synthetic relay fixture for the remaining no-card path (step 4)."""
import ctypes as C
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import uuid
import wave
from test_firmware_pairing import ROOT, Fixture, Native, Config, Storage, Snapshot, UUID, PairIO, Receipt
from relay.providers import ConfiguredService
from app import RelayError

class Target(C.Structure):
    _fields_=[('instance',UUID),('url_hash',C.c_uint8*32),('device_id',C.c_char*40)]
class Audio(C.Structure):
    _fields_=[('sha256',C.c_uint8*32),('bytes',C.c_uint64)]
class Entry(C.Structure):
    _fields_=[('id',C.c_uint32),('state',C.c_int),('automatic',C.c_bool),('bound',C.c_bool),('manual',C.c_bool),('target',Target),('audio',Audio)]
class Record(C.Structure):
    _fields_=[('id',C.c_uint32),('data_bytes',C.c_uint32),('duration_ms',C.c_uint32),('path',C.c_char*256)]
class Store(C.Structure):
    _fields_=[('root',C.c_char*256),('file',C.c_void_p),('id',C.c_uint32),('data_bytes',C.c_uint32),('fail_after',C.c_uint64),('next_fail_after',C.c_uint64),('failed',C.c_bool),('error',C.c_int),('partial',C.c_char*256)]
class Result(C.Structure):
    _fields_=[('state',C.c_int),('can_retry',C.c_bool),('pending',C.c_bool),('can_cancel',C.c_bool),('error',C.c_char*80)]
BEGIN=C.CFUNCTYPE(C.c_bool,C.c_void_p)
END=C.CFUNCTYPE(None,C.c_void_p)
GET=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_uint32,C.c_void_p,C.c_size_t)
UPLOAD=C.CFUNCTYPE(C.c_int,C.c_void_p,C.POINTER(Record),C.c_char_p,C.c_void_p,C.c_size_t)
CONTROL=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_uint32,C.c_char_p,C.c_void_p,C.c_size_t)
class IO(C.Structure):
    _fields_=[('context',C.c_void_p),('begin',BEGIN),('end',END),('status',GET),('upload',UPLOAD),('retry',CONTROL),('cancel',CONTROL)]

class LegacyRelayFixture(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='sync-pipeline-',dir=ROOT/'private/host-tests')
        cls.directory=Path(cls.temp.name);sdk=ROOT/'.tools/esp-idf'
        assert subprocess.check_output(['git','-C',str(sdk),'rev-parse','HEAD']).decode().strip()=='fcae32885b0296b32044cb99ecbdc50d98dddb83'
        sources=['memo_store.c','memo_sync_cache.c','memo_record_ids.c','memo_network_config.c','memo_service_pairing.cpp','memo_cardless_record.c']
        objects=[]
        for source in [*(ROOT/'main'/s for s in sources),sdk/'components/json/cJSON/cJSON.c']:
            obj=cls.directory/(source.stem+'.o');objects.append(str(obj));cpp=source.suffix=='.cpp'
            subprocess.run(['c++' if cpp else 'cc','-fPIC','-g','-O1','-Wall','-Wextra','-Werror','-DMEMO_HOST_TEST','-D_POSIX_C_SOURCE=200809L',
              '-std=c++17' if cpp else '-std=c11','-I',str(ROOT/'main'),'-I',str(sdk/'components/json/cJSON'),'-c',str(source),'-o',str(obj)],check=True)
        library=cls.directory/'pipeline.so';subprocess.run(['c++','-shared',*objects,'-o',str(library)],check=True)
        cls.lib=C.CDLL(str(library));lib=cls.lib
        lib.memo_config_load.argtypes=[C.POINTER(Config),C.POINTER(Storage)]
        lib.memo_config_set_service.argtypes=[C.POINTER(Config),C.c_char_p,C.c_char_p,C.c_char_p,C.c_char_p,C.c_char_p,C.c_bool]
        lib.memo_config_read_snapshot.argtypes=lib.memo_config_credentials.argtypes=[C.POINTER(Config),C.POINTER(Snapshot)]
        lib.memo_config_bind_instance.argtypes=lib.memo_config_finish_pairing.argtypes=[C.POINTER(Config),C.c_uint64,C.POINTER(UUID)]
        lib.memo_config_reject_pairing.argtypes=[C.POINTER(Config),C.c_uint64]
        lib.memo_config_join_url.argtypes=[C.c_char_p,C.c_char_p,C.c_void_p,C.c_size_t]
        lib.memo_pairing_run.argtypes=[Snapshot,C.POINTER(PairIO),C.POINTER(Receipt)]
        lib.memo_store_init.argtypes=[C.POINTER(Store),C.c_char_p];lib.memo_store_max_id.argtypes=[C.POINTER(Store),C.POINTER(C.c_uint32)]
        lib.memo_store_begin_after.argtypes=lib.memo_store_begin_reserved.argtypes=[C.POINTER(Store),C.c_uint32]
        lib.memo_store_append.argtypes=[C.POINTER(Store),C.c_void_p,C.c_size_t];lib.memo_store_finish.argtypes=[C.POINTER(Store),C.POINTER(Record)]
        lib.memo_store_abort.argtypes=[C.POINTER(Store)];lib.memo_store_discard_partial.argtypes=[C.POINTER(Store),C.c_uint32]
        lib.memo_sync_entry_read.argtypes=[C.c_char_p,C.c_uint32,C.POINTER(Entry)]
    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(dir=self.directory);self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name);(self.root/'relay').mkdir(mode=0o700);self.fixture=Fixture(str(self.root/'relay'));self.addCleanup(self.fixture.close)
        self.native=Native(self.lib);self.assertEqual(self.native.stage(self.fixture.url,self.fixture.code()),0)
        self.assertEqual(self.native.run(self.fixture),0)
        self.device=self.native.cfg.value.device_id.decode();self.token=self.native.cfg.value.token.decode()
        self.target=Target();C.memmove(self.target.instance.bytes,uuid.UUID(self.fixture.runtime.instance_uuid).bytes,16)
        C.memmove(self.target.url_hash,hashlib.sha256(self.fixture.url.encode()).digest(),32);self.target.device_id=self.device.encode()
        view=self.fixture.client.get(self.fixture.admin_path+'api/config').json()
        self.fixture.client.post(self.fixture.admin_path+'api/config',json={'expected_revision':view['active_revision'],'patch':{
          'qwen_api_key':{'op':'replace','value':'synthetic-qwen'},'siyuan_token':{'op':'replace','value':'synthetic-siyuan'}}},headers=self.fixture.headers).raise_for_status()
        self.deliveries=[];self.asr=[];self.mode='ok'
        def factory(store,config,progress=None):
            service=ConfiguredService(store,config,progress=progress)
            def asr(data,duration):
                self.asr.append(duration)
                if self.mode=='asr_fail':raise RelayError(401,'qwen_auth','synthetic')
                return '合成录音用于验证设备同步流程','synthetic'
            def send(title,content):
                if self.mode=='delivery_fail':raise RelayError(502,'siyuan_rejected','synthetic')
                if self.mode=='uncertain':raise RelayError(503,'siyuan_uncertain','synthetic')
                self.deliveries.append(title)
            service._qwen_transcriber,service._siyuan_sender=asr,send;return service
        self.fixture.runtime._provider_factory=factory
        self.events=[];self.callbacks=[];self.pre_admission_drop=False;self.drop=False;self.drop_retry=False;self.status_override=None;self.replace_on_get=False;self.replace_metadata_on_get=False;self.busy=False
    def work(self):self.assertTrue(self.fixture.runtime.run_pending_once())
    def reboot_pairing_and_server(self):
        instance=self.fixture.runtime.instance_uuid
        self.native=Native(self.lib,self.native.durable)
        snapshot=Snapshot()
        self.assertEqual(self.lib.memo_config_credentials(C.byref(self.native.cfg),C.byref(snapshot)),0)
        self.assertEqual((snapshot.device_id.decode(),snapshot.token.decode()),(self.device,self.token))
        self.fixture.restart()
        self.assertEqual(self.fixture.runtime.instance_uuid,instance)
        self.assertTrue(self.fixture.runtime.readiness()['ready'])
