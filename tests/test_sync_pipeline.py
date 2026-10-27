"""Actual portable device sync engine against R02 ASGI/ledger/broker.

ESP transport uses explicit Python callbacks; files and C/C++ codec/state
are production code. No device, radio, TLS socket or live provider claim.
"""
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

class SyncTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='sync-pipeline-',dir=ROOT/'private/host-tests')
        cls.directory=Path(cls.temp.name);sdk=ROOT/'.tools/esp-idf'
        assert subprocess.check_output(['git','-C',str(sdk),'rev-parse','HEAD']).decode().strip()=='fcae32885b0296b32044cb99ecbdc50d98dddb83'
        sources=['memo_store.c','memo_sync_cache.c','memo_record_ids.c','memo_network_config.c','memo_service_pairing.cpp','memo_sd_sync.cpp','memo_cardless_record.c']
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
        lib.memo_sd_sync_attempt.argtypes=[C.c_char_p,C.c_char_p,C.c_uint32,C.POINTER(Target),C.c_int,C.POINTER(IO),C.POINTER(Result)]
        lib.memo_sd_sync_pending.argtypes=[C.c_char_p,C.POINTER(Target),C.POINTER(C.c_uint32),C.POINTER(C.c_bool),C.POINTER(C.c_bool)]
        lib.memo_sync_entry_write.argtypes=[C.c_char_p,C.POINTER(Entry)];lib.memo_sync_entry_write.restype=C.c_bool
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
        self.make_wav(1);self.intent(1)
    def make_wav(self,id,variant=1):
        path=self.root/f'memo_{id:010}.wav'
        with wave.open(str(path),'wb') as wav:
            wav.setnchannels(1);wav.setsampwidth(2);wav.setframerate(16000)
            wav.writeframes(b''.join(struct.pack('<h',int(9000*math.sin(i*0.04*variant))) for i in range(32000)))
        return path
    def path(self,id):return self.root/f'memo_{id:010}.wav'
    def cache(self,id):return self.root/f'sync_r1_{id:010}.job'
    def intent(self,id):
        e=Entry();e.id=id;e.state=2;e.automatic=True
        self.assertTrue(self.lib.memo_sync_entry_write(str(self.cache(id)).encode(),C.byref(e)))
    def read_entry(self,id):
        e=Entry();self.assertEqual(self.lib.memo_sync_entry_read(str(self.cache(id)).encode(),id,C.byref(e)),1);return e
    def run_device(self,id=1,mode=0,cache=None):
        def emit(reply,out,capacity):
            content=reply.content;C.memset(out,0,capacity)
            if len(content)>=capacity:return 0
            C.memmove(out,content,len(content));return reply.status_code
        def get(_,rid,out,capacity):
            try:
                self.events.append(('GET',rid))
                response=self.fixture.client.get(self.fixture.url+f'/v1/memos/{self.device}/{rid}',headers={'Authorization':'Bearer '+self.token})
                if self.replace_on_get:
                    self.make_wav(rid,2);self.replace_on_get=False
                    if self.replace_metadata_on_get:
                        data=self.path(rid).read_bytes();e=Entry();e.id=rid;e.state=2;e.automatic=True;e.bound=True;e.target=self.target;e.audio.bytes=len(data)
                        C.memmove(e.audio.sha256,hashlib.sha256(data).digest(),32)
                        assert self.lib.memo_sync_entry_write(str(self.cache(rid)).encode(),C.byref(e))
                if self.status_override is not None:return self.status_override
                return emit(response,out,capacity)
            except Exception as error:self.callbacks.append(str(error));return 0
        def upload(_,record,cancel_token,out,capacity):
            try:
                rid=record.contents.id;self.events.append(('POST',rid))
                if self.pre_admission_drop:self.pre_admission_drop=False;return 0
                data=Path(record.contents.path.decode()).read_bytes()
                response=self.fixture.client.post(self.fixture.url+'/v1/memos',content=data,headers={
                 'Authorization':'Bearer '+self.token,'Content-Type':'audio/wav','X-Device-ID':self.device,'X-Recording-ID':str(rid),**({'X-Cancel-Token':cancel_token.decode()} if cancel_token else {})})
                if self.drop:self.drop=False;return 0
                return emit(response,out,capacity)
            except Exception as error:self.callbacks.append(str(error));return 0
        def retry(_,rid,cancel_token,out,capacity):
            try:
                self.events.append(('RETRY',rid));response=self.fixture.client.post(self.fixture.url+f'/v1/memos/{self.device}/{rid}/retry',json={'cancel_token':cancel_token.decode() if cancel_token else ''},headers={'Authorization':'Bearer '+self.token})
                if self.drop_retry:self.drop_retry=False;return 0
                return emit(response,out,capacity)
            except Exception as error:self.callbacks.append(str(error));return 0
        def cancel(_,rid,cancel_token,out,capacity):
            self.events.append(('CANCEL',rid))
            if self.status_override is not None:return self.status_override
            return emit(self.fixture.client.post(self.fixture.url+f'/v1/memos/{self.device}/{rid}/cancel',json={'cancel_token':cancel_token.decode() if cancel_token else ''},headers={'Authorization':'Bearer '+self.token}),out,capacity)
        io=IO(None,BEGIN(lambda _:not self.busy),END(lambda _:None),GET(get),UPLOAD(upload),CONTROL(retry),CONTROL(cancel));out=Result()
        self.lib.memo_sd_sync_attempt(str(self.path(id)).encode(),str(cache or self.cache(id)).encode(),id,C.byref(self.target),mode,C.byref(io),C.byref(out))
        self.assertEqual(self.callbacks,[]);return out
    def work(self):self.assertTrue(self.fixture.runtime.run_pending_once())
    def reboot_pairing_and_server(self):
        instance = self.fixture.runtime.instance_uuid
        self.native = Native(self.lib, self.native.durable)
        snapshot = Snapshot()
        self.assertEqual(self.lib.memo_config_credentials(C.byref(self.native.cfg), C.byref(snapshot)), 0)
        self.assertEqual((snapshot.device_id.decode(), snapshot.token.decode()), (self.device, self.token))
        self.fixture.restart()
        self.assertEqual(self.fixture.runtime.instance_uuid, instance)
        self.assertTrue(self.fixture.runtime.readiness()['ready'])
    def test_paired_wav_pending_restarts_both_ends_then_delivers_once(self):
        self.drop=True
        self.assertNotEqual(self.run_device().state,9)
        self.reboot_pairing_and_server()
        # WAV staging is intentionally volatile. Restart reports a retryable
        # reupload of the SD original; it must not invent a completed upload.
        result=self.run_device()
        self.assertEqual(result.state,10);self.assertTrue(result.can_retry)
        self.assertEqual(self.run_device(mode=1).state,6)
        self.work()
        self.reboot_pairing_and_server()
        self.assertEqual(self.run_device(mode=2).state,9)
        self.assertEqual(self.events.count(('POST',1)),2)
        self.assertEqual((len(self.asr),len(self.deliveries)),(1,1))
    def test_paired_wav_retry_restarts_both_ends_preserving_text(self):
        self.mode='delivery_fail';self.run_device();self.work()
        self.reboot_pairing_and_server();self.mode='ok'
        self.assertEqual(self.run_device(mode=1).state,8)
        self.reboot_pairing_and_server();self.work()
        self.assertEqual(self.run_device(mode=2).state,9)
        self.assertEqual(self.events.count(('RETRY',1)),1)
        self.assertEqual((len(self.asr),len(self.deliveries)),(1,1))
    def test_pending_directory_error_is_not_empty_queue(self):
        cursor=C.c_uint32(0);manual=C.c_bool();complete=C.c_bool(True)
        missing=self.root/'missing'
        self.assertEqual(self.lib.memo_sd_sync_pending(str(missing).encode(),C.byref(self.target),C.byref(cursor),C.byref(manual),C.byref(complete)),0)
        self.assertFalse(complete.value)
        missing.mkdir()
        self.assertEqual(self.lib.memo_sd_sync_pending(str(missing).encode(),C.byref(self.target),C.byref(cursor),C.byref(manual),C.byref(complete)),0)
        self.assertTrue(complete.value)
    def test_unchanged_state_keeps_existing_slots_and_generation(self):
        entry=self.read_entry(1)
        paths=[Path(str(self.cache(1))+f'.{n}') for n in (0,1)]
        before=[p.read_bytes() if p.exists() else None for p in paths]
        for _ in range(6):
            self.assertTrue(self.lib.memo_sync_entry_write(str(self.cache(1)).encode(),C.byref(entry)))
        self.assertEqual([p.read_bytes() if p.exists() else None for p in paths],before)
        entry.automatic=False;entry.state=1
        self.assertTrue(self.lib.memo_sync_entry_write(str(self.cache(1)).encode(),C.byref(entry)))
        self.assertNotEqual([p.read_bytes() if p.exists() else None for p in paths],before)
        self.assertFalse(self.read_entry(1).automatic)
    def test_new_offline_multiple_then_reboot_each_sync(self):
        for id in range(2,5):self.make_wav(id);self.intent(id)
        cursor=C.c_uint32(0);manual=C.c_bool();discovered=[]
        for expected in range(1,5):
            id=self.lib.memo_sd_sync_pending(str(self.root).encode(),C.byref(self.target),C.byref(cursor),C.byref(manual),None)
            self.assertEqual(id,expected);self.assertFalse(manual.value);discovered.append(id)
            self.assertEqual(self.run_device(id).state,6);self.work();self.assertEqual(self.run_device(id).state,9)
            cursor=C.c_uint32(0) # discard the only RAM scanning hint between each durable reconstruction
        self.assertEqual(discovered,[1,2,3,4])
        self.assertEqual(self.lib.memo_sd_sync_pending(str(self.root).encode(),C.byref(self.target),C.byref(cursor),C.byref(manual),None),0)
        self.assertEqual(len(self.deliveries),4)
        before=len(self.events)
        for id in range(1,5):self.assertEqual(self.run_device(id,2).state,9)
        self.assertEqual(len(self.events),before)
    def test_lost_upload_reply_queries_same_binding_after_restart(self):
        self.drop=True;self.assertEqual(self.run_device().state,5)
        bound=self.read_entry(1);self.assertFalse(bound.bound);self.work()
        self.assertEqual(self.run_device().state,9);self.assertEqual(self.events.count(('POST',1)),1)
    def test_local_status_accepts_same_id_same_size_replacement_without_scanning_audio(self):
        self.run_device();self.work();self.assertEqual(self.run_device().state,9)
        self.make_wav(1,2)
        # Explicitly accepted product tradeoff: browsing shows the saved result,
        # even if somebody externally replaced same-size audio under the same ID.
        before=list(self.events)
        self.assertEqual(self.run_device(1,3).state,9);self.assertEqual(self.events,before)
    def test_server_receipt_recovers_missing_local_binding(self):
        self.run_device();self.work()
        for slot in (0,1):Path(str(self.cache(1))+f'.{slot}').unlink(missing_ok=True)
        self.assertEqual(self.run_device(mode=1).state,9)
        self.assertEqual(self.events.count(('POST',1)),1)
    def test_detail_rejects_changed_length_using_header_only(self):
        self.run_device();self.work();self.run_device()
        with wave.open(str(self.path(1)),'wb') as wav:
            wav.setnchannels(1);wav.setsampwidth(2);wav.setframerate(16000);wav.writeframes(b'\0'*6400)
        self.assertEqual(self.run_device(1,3).error,b'recording_changed')
    def test_accepted_job_get_404_never_reuploads(self):
        self.run_device();self.status_override=404
        result=self.run_device();self.assertEqual(result.state,12)
        self.assertEqual(result.error,b'server_task_missing');self.assertEqual(self.events.count(('POST',1)),1)
    def test_cancel_offline_queue_stops_auto_and_keeps_audio(self):
        original=self.path(1).read_bytes();self.status_override=0
        result=self.run_device(mode=4);self.assertEqual(result.state,13)
        self.assertFalse(result.pending);self.assertTrue(result.can_retry);self.assertEqual(self.events,[])
        self.assertEqual(self.path(1).read_bytes(),original)
        self.assertEqual(self.run_device().error,b'no_pending_intent')
        self.status_override=None;self.assertEqual(self.run_device(mode=1).state,6)
    def test_cancel_received_retries_confirmation_without_resubmission(self):
        self.run_device();self.status_override=0
        result=self.run_device(mode=4);self.assertEqual(result.state,14);self.assertTrue(result.pending)
        self.status_override=None;self.assertEqual(self.run_device().state,13)
        self.work();self.assertEqual(self.deliveries,[]);self.assertEqual(self.events.count(('POST',1)),1)
        self.assertEqual(self.run_device(mode=1).state,6);self.work();self.run_device()
        self.assertEqual(len(self.deliveries),1)
    def test_lost_first_upload_reply_can_still_be_cancelled(self):
        self.drop=True;self.run_device();self.assertFalse(self.read_entry(1).bound)
        self.assertEqual(self.run_device(mode=4).state,13);self.work()
        self.assertEqual(self.deliveries,[]);self.assertEqual(self.events.count(('POST',1)),1)
    def test_cancel_then_explicit_submit_lost_ack_failure_stops_without_second_post(self):
        self.run_device();self.assertEqual(self.run_device(mode=4).state,13);self.work()
        self.mode='asr_fail';self.drop=True
        self.assertEqual(self.run_device(mode=1).state,5)
        self.assertTrue(self.read_entry(1).manual)
        self.work()
        # A reboot discovers the persisted manual intent, but this upload was
        # already attempted. Its failure is a result, not a new retry request.
        self.assertEqual(self.run_device(mode=1).state,10)
        self.assertFalse(self.read_entry(1).automatic)
        self.assertEqual(self.run_device().error,b'no_pending_intent')
        self.assertEqual(self.events.count(('POST',1)),2);self.assertEqual(len(self.asr),1)

    def test_cancel_too_late_tracks_sending_and_clears_cancel_state(self):
        self.run_device()
        self.fixture.runtime.store.update(self.device,1,status='sending')
        self.fixture.runtime._update_job(self.device,1,phase='sending')
        result=self.run_device(mode=4)
        self.assertEqual(result.state,8);self.assertTrue(result.pending)
        self.assertEqual(result.error,b'cancel_too_late')
        self.assertEqual(self.read_entry(1).state,8)
        self.assertEqual(self.run_device().state,8)
        self.assertEqual(self.events.count(('POST',1)),1)

    def test_cancel_successful_remote_does_not_invent_cancelled(self):
        self.run_device();self.work()
        self.assertEqual(self.run_device(mode=4).state,9)
        before=list(self.events);result=self.run_device(mode=1)
        self.assertEqual(result.state,9);self.assertFalse(result.can_retry);self.assertEqual(self.events,before)
    def test_swapped_media_task_metadata_is_not_overwritten_by_old_attempt(self):
        self.run_device();self.work();self.replace_on_get=self.replace_metadata_on_get=True
        self.assertEqual(self.run_device().error,b'recording_changed')
        e=self.read_entry(1);self.assertTrue(e.automatic)
        self.assertEqual(bytes(e.audio.sha256),hashlib.sha256(self.path(1).read_bytes()).digest())
    def test_missing_cache_manual_queries_before_upload(self):
        for slot in (0,1):Path(str(self.cache(1))+f'.{slot}').unlink(missing_ok=True)
        self.assertEqual(self.run_device().error,b'no_pending_intent');self.assertEqual(self.events,[])
        self.assertEqual(self.run_device(1,1).state,6);self.assertEqual(self.events,[('GET',1),('POST',1)])
    def test_corrupt_cache_not_automatic_manual_recreates_from_real_audio(self):
        Path(str(self.cache(1))+'.0').write_bytes(b'corrupt')
        self.assertEqual(self.run_device().error,b'no_pending_intent');self.assertEqual(self.events,[])
        self.assertEqual(self.run_device(1,1).state,6)
    def test_binding_write_failure_sends_no_http(self):
        result=self.run_device(1,1,self.root/'missing'/'job')
        self.assertEqual(result.error,b'sync_metadata_write_failed');self.assertEqual(self.events,[])
    def test_read_failure_sends_no_http_and_keeps_other_slot(self):
        Path(str(self.cache(1))+'.1').mkdir()
        self.assertEqual(self.run_device().error,b'sync_metadata_unreadable');self.assertEqual(self.events,[])
        Path(str(self.cache(1))+'.1').rmdir();self.assertEqual(self.run_device().state,6)
    def test_audio_admission_delays_sd_without_requests(self):
        self.busy=True;self.assertEqual(self.run_device().error,b'audio_busy');self.assertEqual(self.events,[])
        self.busy=False;self.assertEqual(self.run_device().state,6)
    def test_auth_and_timeout_never_replace_verified_success(self):
        self.run_device();self.work();self.run_device()
        for status in (401,0):self.status_override=status;self.assertEqual(self.run_device(1,2).state,9)
    def test_old_bound_queued_sidecar_does_not_imply_server_acceptance(self):
        e=self.read_entry(1);e.bound=True;e.target=self.target
        data=self.path(1).read_bytes();e.audio.bytes=len(data)
        C.memmove(e.audio.sha256,hashlib.sha256(data).digest(),32)
        self.assertTrue(self.lib.memo_sync_entry_write(str(self.cache(1)).encode(),C.byref(e)))
        self.assertEqual(self.run_device(mode=1).state,6);self.work();self.assertEqual(self.run_device().state,9)
    def test_rejected_content_can_be_explicitly_submitted_but_sent_cannot(self):
        self.run_device();self.work()
        self.fixture.runtime.store.update(self.device,1,status='transcription_rejected',error='transcript_rejected')
        self.fixture.runtime._update_job(self.device,1,phase='transcription_rejected')
        self.assertEqual(self.run_device().state,11)
        self.assertTrue(self.run_device(mode=3).can_retry)
        self.assertEqual(self.run_device(mode=1).state,6);self.work();self.assertEqual(self.run_device().state,9)
        self.assertFalse(self.run_device(mode=3).can_retry)

    def test_rejected_manual_intent_survives_reboot_and_is_cancellable(self):
        self.run_device();self.work()
        self.fixture.runtime.store.update(self.device,1,status='transcription_rejected',error='transcript_rejected')
        self.fixture.runtime._update_job(self.device,1,phase='transcription_rejected')
        self.assertEqual(self.run_device().state,11)
        e=self.read_entry(1);e.manual=e.automatic=True
        self.assertTrue(self.lib.memo_sync_entry_write(str(self.cache(1)).encode(),C.byref(e)))
        cursor=C.c_uint32(0);manual=C.c_bool();complete=C.c_bool()
        self.assertEqual(self.lib.memo_sd_sync_pending(str(self.root).encode(),C.byref(self.target),C.byref(cursor),C.byref(manual),C.byref(complete)),1)
        self.assertTrue(manual.value);self.assertTrue(complete.value)
        self.assertTrue(self.run_device(mode=3).can_cancel)
        # This is still the prior rejected server task: cancellation may truthfully
        # report that terminal result, but must stop the new local submit intent.
        out=self.run_device(mode=4)
        self.assertFalse(out.pending);self.assertIn(out.state,(11,13))
        self.assertEqual(self.run_device().error,b'no_pending_intent')
        self.assertEqual(self.events.count(('POST',1)),1)

    def test_failed_manual_submit_interrupted_before_admission_resumes_once(self):
        self.mode='asr_fail';self.run_device();self.work();self.assertEqual(self.run_device().state,10)
        self.mode='ok';self.pre_admission_drop=True
        self.assertEqual(self.run_device(mode=1).state,5)
        self.assertTrue(self.read_entry(1).automatic)
        self.assertEqual(self.run_device().state,6);self.work();self.assertEqual(self.run_device().state,9)
        self.assertEqual(len(self.asr),2);self.assertEqual(len(self.deliveries),1)

    def test_asr_failed_manual_reuploads_same_audio(self):
        self.mode='asr_fail';self.run_device();self.work();self.assertEqual(self.run_device().state,10)
        self.mode='ok';self.assertEqual(self.run_device(1,1).state,6);self.work();self.assertEqual(self.run_device().state,9)
        self.assertEqual(len(self.deliveries),1)
    def test_delivery_failed_manual_uses_existing_text_no_second_asr(self):
        self.mode='delivery_fail';self.run_device();self.work();self.assertEqual(self.run_device().state,10)
        self.mode='ok';self.assertEqual(self.run_device(1,1).state,8);self.work();self.assertEqual(self.run_device().state,9)
        self.assertEqual(len(self.asr),1);self.assertEqual(len(self.deliveries),1)
    def test_lost_retry_response_polls_without_second_retry(self):
        self.mode='delivery_fail';self.run_device();self.work();self.run_device();self.mode='ok';self.drop_retry=True
        self.assertEqual(self.run_device(1,1).state,5);self.work();self.assertEqual(self.run_device().state,9)
        self.assertEqual(self.events.count(('RETRY',1)),1)
    def test_uncertain_cannot_trigger_device_resend(self):
        self.mode='uncertain';self.run_device();self.work();self.assertEqual(self.run_device().state,12)
        self.assertEqual(self.run_device(1,1).state,12);self.assertEqual(self.events.count(('POST',1)),1);self.assertNotIn(('RETRY',1),self.events)
    def test_new_destination_cannot_automatically_resume_old_task(self):
        self.run_device();original=self.read_entry(1)
        self.target.instance.bytes[0]^=1;before=len(self.events)
        self.assertEqual(self.run_device().error,b'different_service');self.assertEqual(len(self.events),before)
        self.assertEqual(bytes(self.read_entry(1).target.instance.bytes),bytes(original.target.instance.bytes))
    def test_local_detail_never_queries_or_retries_and_preserves_result(self):
        self.run_device();self.work();self.run_device()
        before=list(self.events);raw=[Path(str(self.cache(1))+f'.{n}').read_bytes() for n in (0,1)]
        for _ in range(4):self.assertEqual(self.run_device(1,3).state,9)
        self.assertEqual(self.events,before)
        self.assertEqual(raw,[Path(str(self.cache(1))+f'.{n}').read_bytes() for n in (0,1)])
        audio=self.path(1).read_bytes();self.path(1).write_bytes(audio[:-1]+bytes([audio[-1]^1]))
        result=self.run_device(1,3)
        self.assertEqual(result.error,b'');self.assertEqual(result.state,9)
        self.assertEqual(self.events,before)

    def test_no_auto_history_without_version_created_intent(self):
        self.make_wav(42);self.assertEqual(self.run_device(42).error,b'no_pending_intent');self.assertEqual(self.events,[])
    def test_device_retry_permission_and_terminal_rejection(self):
        self.run_device();self.work();path=self.fixture.url+f'/v1/memos/{self.device}/1/retry'
        self.assertEqual(self.fixture.client.post(path,json={}).status_code,401)
        self.assertEqual(self.fixture.client.post(path,json={},headers={'Authorization':'Bearer '+self.token}).status_code,409)
        self.assertEqual(self.fixture.client.post(self.fixture.url+'/v1/memos/another-device/1/retry',json={},headers={'Authorization':'Bearer '+self.token}).status_code,401)

    def test_device_retry_duplicate_and_source_isolation(self):
        self.mode='delivery_fail';self.run_device();self.work();path=self.fixture.url+f'/v1/memos/{self.device}/1/retry'
        headers={'Authorization':'Bearer '+self.token}
        self.assertEqual(self.fixture.client.post(path,json={},headers=headers).status_code,202)
        self.assertEqual(self.fixture.client.post(path,json={},headers=headers).status_code,409)
        from stream_service import FORMAT
        from test_stream_service import synthetic_chunks
        rid=0x80000002;auth={**headers,'X-Device-ID':self.device,'X-Recording-ID':str(rid)}
        self.assertEqual(self.fixture.client.post(self.fixture.url+'/v1/streams',json=FORMAT,headers=auth).status_code,201)
        data=synthetic_chunks(frequencies=(440,),frames=50)[0]
        prefix=self.fixture.url+f'/v1/streams/{self.device}/{rid}'
        self.assertEqual(self.fixture.client.put(prefix+'/chunks/0',content=data,headers={**headers,'Content-Type':'application/octet-stream','X-Chunk-SHA256':hashlib.sha256(data).hexdigest()}).status_code,201)
        self.assertEqual(self.fixture.client.post(prefix+'/finish',json={'chunks':1,'samples':16000,'interrupted':False,'reason':''},headers=headers).status_code,202)
        self.work();self.work() # stream Memo row is published by its actual worker
        wrong=self.fixture.client.post(self.fixture.url+f'/v1/memos/{self.device}/{rid}/retry',json={},headers=headers)
        self.assertEqual(wrong.status_code,409);self.assertEqual(wrong.json()['error'],'wrong_task_source')
    def test_device_retry_uncertain_remains_rejected(self):
        self.mode='uncertain';self.run_device();self.work()
        response=self.fixture.client.post(self.fixture.url+f'/v1/memos/{self.device}/1/retry',json={},headers={'Authorization':'Bearer '+self.token})
        self.assertEqual(response.status_code,409);self.assertEqual(response.json()['error'],'delivery_uncertain')

    def inspect_pending(self,manual):
        if manual:
            self.run_device(1,2);e=self.read_entry(1);e.manual=e.automatic=True
            self.assertTrue(self.lib.memo_sync_entry_write(str(self.cache(1)).encode(),C.byref(e)))
        self.run_device(1,2);e=self.read_entry(1);self.assertTrue(e.automatic);self.assertEqual(e.manual,manual)
        cursor=C.c_uint32(0);flag=C.c_bool()
        self.assertEqual(self.lib.memo_sd_sync_pending(str(self.root).encode(),C.byref(self.target),C.byref(cursor),C.byref(flag),None),1)
        self.assertEqual(flag.value,manual)
        self.assertEqual(self.run_device(1,int(manual)).state,6);self.work();self.assertEqual(self.run_device(1,2).state,9)
        self.assertEqual(self.events.count(('POST',1)),1)
    def test_inspect_before_upload_preserves_auto_intent_across_reconstruction(self):self.inspect_pending(False)
    def test_inspect_before_upload_preserves_manual_intent_across_reconstruction(self):self.inspect_pending(True)
    def test_inspect_failed_job_preserves_queued_explicit_retry(self):
        self.mode='asr_fail';self.run_device();self.work();self.run_device()
        e=self.read_entry(1);e.automatic=e.manual=True
        self.assertTrue(self.lib.memo_sync_entry_write(str(self.cache(1)).encode(),C.byref(e)))
        self.run_device(1,2);e=self.read_entry(1);self.assertTrue(e.automatic and e.manual)
        self.mode='ok';self.assertEqual(self.run_device(1,1).state,6);self.work();self.assertEqual(self.run_device(1,2).state,9)
        self.assertEqual(len(self.deliveries),1)
    def fallback_with_orphan(self,discard):
        self.path(1).unlink();store=Store();self.assertEqual(self.lib.memo_store_init(C.byref(store),str(self.root).encode()),0)
        if discard:
            for slot in (0,1):Path(str(self.cache(1))+f'.{slot}').unlink(missing_ok=True)
            self.assertEqual(self.lib.memo_store_begin_reserved(C.byref(store),1),0);self.intent(1)
            self.assertEqual(self.lib.memo_store_abort(C.byref(store)),0)
            self.assertEqual(self.lib.memo_store_discard_partial(C.byref(store),1),0)
        original=Path(str(self.cache(1))+'.0').read_bytes()
        floor=C.c_uint32();self.assertEqual(self.lib.memo_store_max_id(C.byref(store),C.byref(floor)),0);self.assertEqual(floor.value,1)
        self.assertEqual(self.lib.memo_store_begin_after(C.byref(store),floor.value),0);self.assertEqual(store.id,2)
        pcm=(C.c_int16*32000)(*[123]*32000);self.assertEqual(self.lib.memo_store_append(C.byref(store),pcm,32000),0)
        record=Record();self.assertEqual(self.lib.memo_store_finish(C.byref(store),C.byref(record)),0)
        cursor=C.c_uint32(0);manual=C.c_bool();self.assertEqual(self.lib.memo_sd_sync_pending(str(self.root).encode(),C.byref(self.target),C.byref(cursor),C.byref(manual),None),0)
        self.assertEqual(self.run_device(2).error,b'no_pending_intent');self.assertEqual(self.events,[])
        self.assertEqual(Path(str(self.cache(1))+'.0').read_bytes(),original)
        self.assertEqual(self.run_device(2,1).state,6);self.assertEqual(self.events.count(('POST',2)),1)
    def test_allocator_fault_fallback_avoids_deleted_wav_orphan_intent(self):self.fallback_with_orphan(False)
    def test_allocator_fault_fallback_avoids_discarded_part_orphan_intent(self):self.fallback_with_orphan(True)

if __name__=='__main__':unittest.main(verbosity=2)
