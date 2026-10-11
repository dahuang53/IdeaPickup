"""Execute the production cardless request/ACK/finish/recovery and NVS methods.

ESP/RTOS/NVS/encoder delivery are explicit host shims. The Opus packets, R02
ASGI, ledger, broker and production codec/loop are real. No device/TLS claim.
"""
import ctypes as C
import hashlib
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_sync_pipeline import SyncTests, Target, ROOT
from test_stream_service import synthetic_chunks
from test_service_runtime import extract

TRANSPORT=C.CFUNCTYPE(C.c_int,C.c_char_p,C.c_char_p,C.c_uint32,C.c_void_p,C.c_size_t,C.c_char_p,C.c_char_p,C.c_void_p,C.c_size_t)
PRELUDE=r'''
#include "memo_cardless.h"
#include "memo_cardless_record.h"
#include "memo_record_ids.h"
#include "memo_cardless_buffer.h"
#include "memo_cardless_state.h"
#include "network_sync.h"
#include "cJSON.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cassert>
#include <inttypes.h>
#include <map>
#include <string>
#include <vector>
#include "mbedtls/sha256.h"
#define ESP_OK 0
#define ESP_FAIL 2
#define ESP_ERR_NVS_NOT_FOUND 1
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_INVALID_SIZE 4
#define ESP_ERR_NO_MEM 5
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define ESP_AUDIO_ERR_OK 0
#define ESP_OPUS_ENC_CONFIG_DEFAULT() esp_opus_enc_config_t{}
#define ESP_OPUS_ENC_FRAME_DURATION_20_MS 20
#define ESP_OPUS_ENC_APPLICATION_VOIP 1
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define portMAX_DELAY 10000
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
typedef void *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef void *StreamBufferHandle_t;
typedef int StaticStreamBuffer_t;
typedef int nvs_handle_t;
static const char *PARTITION="memo_meta",*NAMESPACE="records1",*TAG="cardless";
static constexpr size_t PCM_BUFFER_BYTES=65536;
static constexpr size_t MAX_RECORDS=1024,FRAME_SAMPLES=320,CHUNK_FRAMES=250,CHUNK_CAPACITY=32000,AUDIO_BUFFER_BYTES=1024*1024;
static std::map<std::string,std::vector<unsigned char>> durable,pending;
static int fault=0,iterations=0,marker_fault=0,audio_flag_fault=0;
static bool audio_flag_write=false;
static std::string last_write;
static bool active_read_fault=false;
static long long now_us=0;
static bool power_ok=true;
static int reads=0;
struct Done {};
static void xSemaphoreTake(void *,int){}
static void xSemaphoreGive(void *){}
static void xTaskNotifyGive(void *){}
static void ulTaskNotifyTake(int,int){if(!iterations--)throw Done{};now_us+=60000000;}
static int64_t esp_timer_get_time(){return now_us;}
static const char *esp_err_to_name(int){return "host-shim";}
static bool power_policy_recovery_ready(){return power_ok;}
static int nvs_open_from_partition(const char *p,const char *n,int,int *h){if(strcmp(p,"memo_meta")||strcmp(n,"records1"))return ESP_FAIL;*h=1;return ESP_OK;}
static void nvs_close(int){}
static int nvs_set_blob(int,const char *key,const void *p,size_t n){last_write=key;audio_flag_write=key[0]==114&&n==MEMO_CARDLESS_RECORD_SIZE&&(((const uint8_t*)p)[32]&4);if(fault==1)return ESP_FAIL;pending[key]=std::vector<unsigned char>((const unsigned char*)p,(const unsigned char*)p+n);return ESP_OK;}
static int nvs_get_blob(int,const char *key,void *p,size_t *n){reads++;if(fault==3||(audio_flag_fault==3&&audio_flag_write&&key[0]==114)||(active_read_fault&&!strcmp(key,"active")))return ESP_FAIL;auto it=durable.find(key);if(it==durable.end())return ESP_ERR_NVS_NOT_FOUND;if(*n<it->second.size())return ESP_FAIL;*n=it->second.size();memcpy(p,it->second.data(),*n);return ESP_OK;}
static int nvs_commit(int){if(fault==1)return ESP_FAIL;
 if(audio_flag_write&&audio_flag_fault){if(audio_flag_fault==1)return ESP_FAIL;durable=pending;return audio_flag_fault==2?ESP_FAIL:ESP_OK;}
 if(last_write=="active"&&marker_fault){if(marker_fault==1)return ESP_FAIL;durable=pending;if(marker_fault==2)return ESP_FAIL;active_read_fault=true;return ESP_OK;}
 durable=pending;return fault==2?ESP_FAIL:ESP_OK;}
static int nvs_erase_key(int,const char *key){bool found=pending.erase(key);return found?ESP_OK:ESP_ERR_NVS_NOT_FOUND;}
static int nvs_get_u32(int,const char *key,uint32_t *v){size_t n=4;return nvs_get_blob(1,key,v,&n);}
static int nvs_set_u32(int,const char *key,uint32_t v){return nvs_set_blob(1,key,&v,4);}
struct nvs_stats_t {size_t free_entries=1024;};
static int nvs_get_stats(const char*,nvs_stats_t *s){s->free_entries=1024;return ESP_OK;}
struct esp_opus_enc_config_t {int sample_rate,channel,bits_per_sample,bitrate,frame_duration,application_mode,complexity;bool enable_fec,enable_dtx,enable_vbr;};
struct esp_audio_enc_info_t {int bitrate,sample_rate,channel;};
static int esp_opus_enc_open(void*,size_t,void **out){*out=(void*)7;return ESP_OK;}
static int esp_opus_enc_get_frame_size(void*,int *in,int *out){*in=640;*out=60;return ESP_OK;}
static int esp_opus_enc_get_info(void*,esp_audio_enc_info_t *out){*out={24000,16000,1};return ESP_OK;}
static void esp_opus_enc_close(void*){}
static uint8_t pcm_memory[PCM_BUFFER_BYTES];
static void *heap_caps_malloc(size_t,int){return pcm_memory;}
static StreamBufferHandle_t xStreamBufferCreateStatic(size_t,int,uint8_t*,StaticStreamBuffer_t*){return (void*)8;}
static void xStreamBufferReset(StreamBufferHandle_t){}
static int ensure_network_task();
extern "C" bool memo_record_ids_reserve(uint32_t floor,uint32_t *id){*id=floor+1;return true;}
static memo_sync_target_t current{};
static int (*transport)(const char*,const char*,uint32_t,const void*,size_t,const char*,const char*,char*,size_t);
static SemaphoreHandle_t http_mutex=(void*)2;
static std::atomic<bool> stream_http_active{false},power_suspended{false};
static int http_claims=0;
static bool swap_before_transport=false;
static std::string base_url,device_token;
static int xSemaphoreTakeRecursive(void*,int){http_claims++;return 1;}
static int xSemaphoreGiveRecursive(void*){http_claims--;return 1;}
static bool ensure_clock(){return true;}
static bool current_service(memo_config_snapshot_t &out){
 if(swap_before_transport){current.instance.bytes[0]^=1;swap_before_transport=false;}
 out={};out.instance=current.instance;snprintf(out.device_id,sizeof(out.device_id),"%s",current.device_id);
 snprintf(out.base_url,sizeof(out.base_url),"%s",base_url.c_str());snprintf(out.token,sizeof(out.token),"%s",device_token.c_str());return true;
}
struct esp_http_client_config_t {const char *url;int (*crt_bundle_attach)(void*);bool disable_auto_redirect;int timeout_ms,buffer_size,buffer_size_tx;};
struct Http {std::string url,payload,response;std::map<std::string,std::string> headers;int method=0,code=0;size_t at=0;};
using esp_http_client_handle_t=Http*;
static const int HTTP_METHOD_GET=0,HTTP_METHOD_POST=1,HTTP_METHOD_PUT=2;
static int esp_crt_bundle_attach(void*){return 0;}
static Http *esp_http_client_init(const esp_http_client_config_t *config){assert(config->disable_auto_redirect&&config->crt_bundle_attach==esp_crt_bundle_attach&&http_claims);auto *h=new Http;h->url=config->url;return h;}
static void esp_http_client_set_method(Http *h,int m){h->method=m;}
static void esp_http_client_set_header(Http *h,const char *k,const char *v){h->headers[k]=v;}
static int esp_http_client_open(Http*,int){return ESP_OK;}
static int esp_http_client_write(Http *h,const char *p,size_t n){h->payload.append(p,n);return (int)n;}
static int esp_http_client_fetch_headers(Http *h){
 assert(h->headers["Authorization"]=="Bearer "+device_token&&h->headers["X-Device-ID"]==current.device_id);
 char reply[2048]{};const char *method=h->method==1?"POST":h->method==2?"PUT":"GET";
 assert(h->url.find(base_url)==0);const char *path=h->url.c_str()+base_url.size();
 h->code=transport(method,path,(uint32_t)std::stoul(h->headers["X-Recording-ID"]),h->payload.data(),h->payload.size(),h->headers.count("Content-Type")?h->headers["Content-Type"].c_str():nullptr,h->headers.count("X-Chunk-SHA256")?h->headers["X-Chunk-SHA256"].c_str():nullptr,reply,sizeof(reply));
 h->response=reply;return h->code>0?(int)h->response.size():-1;
}
static int esp_http_client_get_status_code(Http *h){return h->code;}
static int esp_http_client_read(Http *h,char *p,size_t n){n=std::min(n,h->response.size()-h->at);memcpy(p,h->response.data()+h->at,n);h->at+=n;return (int)n;}
static void esp_http_client_close(Http*){}
static void esp_http_client_cleanup(Http *h){delete h;}

extern "C" bool network_sync_current_target(memo_sync_target_t *out){*out=current;return true;}
extern "C" void network_sync_get_status(memo_network_status_t *out){*out={};out->initialized=out->configured=out->connected=true;snprintf(out->device_id,sizeof(out->device_id),"%s",current.device_id);}

'''
WRAPPERS=r'''
extern "C" void fixture_reset(uint32_t id,const memo_sync_target_t *target){
 durable.clear();pending.clear();fault=marker_fault=audio_flag_fault=0;audio_flag_write=false;active_read_fault=false;last_write.clear();reads=0;current=*target;now_us=0;power_ok=true;swap_before_transport=false;http_claims=0;stream_http_active=power_suspended=false;
 static memo_cardless_record_t memory[MAX_RECORDS];static uint8_t ring[AUDIO_BUFFER_BYTES],chunk[CHUNK_CAPACITY];
 memset(memory,0,sizeof(memory));records=memory;record_count=1;meta_mutex=buffer_mutex=task_mutex=(void*)1;metadata=1;network_chunk=chunk;packet_storage=ring;
 memo_cardless_buffer_init(&packets,ring,sizeof(ring));records[0].id=id;records[0].target=*target;records[0].state=MEMO_JOB_QUEUED;
 initialized=true;metadata_fault=false;remote_complete=false;transfer_blocked=false;active_id=id;capturing=false;encoder_done=true;recovery_mode=false;
 stop_requested=false;finish_requested=true;encode_abort=false;begin_in_progress=false;encoded_samples=0;input_samples=0;
 test_http_id=test_drop_put_id=test_drop_finish_id=test_finish_hold_id=test_put_401_id=test_ring_id=0;
 status_request=retry_request=0;global_error[0]=stop_reason[0]=query_notice[0]=0;query_notice_id=0;
 persist_locked(records[0]);nvs_set_u32(metadata,"active",id);nvs_commit(metadata);
}
extern "C" void fixture_transport(decltype(transport) p,const char *base,const char *token){transport=p;base_url=base;device_token=token;}
extern "C" void fixture_swap_before_transport(){swap_before_transport=true;}
extern "C" int fixture_begin(int kind,uint32_t *id){
 durable.clear();pending.clear();record_count=0;active_id=0;counter=0;capturing=false;encoder_done=true;metadata_fault=false;begin_in_progress=false;
 network_task_handle=(void*)6;encoder_task_handle=(void*)5;marker_fault=kind;active_read_fault=false;*id=0;
 return memo_cardless_begin(0,id);
}
extern "C" void fixture_restore_io(){marker_fault=audio_flag_fault=0;active_read_fault=false;}
extern "C" void fixture_audio_flag_fault(int cut){audio_flag_fault=cut;}
extern "C" int fixture_record_count(){return (int)record_count;}
extern "C" int fixture_begin_next(uint32_t *id){encoder_task_handle=(void*)5;network_task_handle=(void*)6;return memo_cardless_begin(0,id);}


extern "C" bool fixture_push(const void *data,size_t n){
 const uint8_t *p=(const uint8_t*)data;size_t at=0;unsigned frames=0;
 while(at<n){if(at+2>n)return false;size_t size=p[at]|((size_t)p[at+1]<<8);at+=2;if(at+size>n||!memo_cardless_buffer_push(&packets,p+at,size))return false;at+=size;frames++;}
 encoded_samples+=frames*320;input_samples=encoded_samples.load();return true;
}
extern "C" void fixture_run(int steps){iterations=steps;try{transfer_task(nullptr);}catch(const Done&){} }
extern "C" uint32_t fixture_active(){return active_id;}
extern "C" size_t fixture_packets(){return packets.packets;}
extern "C" int fixture_state(){return records[0].state;}
extern "C" int fixture_can_retry(){return records[0].can_retry;}
extern "C" void fixture_query(bool retry){query_selected(records[0].id,retry);}
extern "C" int fixture_faulted(){return metadata_fault;}
extern "C" int fixture_reads(){return reads;}
extern "C" void fixture_fault(int kind){fault=kind;}
extern "C" void fixture_drop(int kind){if(kind==2)test_drop_put_id=active_id.load();else if(kind==3){test_drop_finish_id=active_id.load();test_finish_seconds=1;}}
extern "C" void fixture_power(bool ok){power_ok=ok;}
extern "C" void fixture_target(const memo_sync_target_t *target){current=*target;}
extern "C" int fixture_restore_marker();
extern "C" bool fixture_reboot(){
 capturing=false;encoder_done=true;remote_complete=transfer_blocked=metadata_fault=false;stop_requested=false;
 memo_cardless_buffer_init(&packets,packet_storage,AUDIO_BUFFER_BYTES);encoded_samples=input_samples=0;
 return fixture_restore_marker()==ESP_OK;
}

'''

class CardlessTests(SyncTests):
    # Reuse the real pairing/provider fixture, not its inherited SD test methods.
    @classmethod
    def setUpClass(cls):
        super().setUpClass();source=(ROOT/'main/memo_cardless.cpp').read_text();a=source.index('SemaphoreHandle_t meta_mutex;');b=source.index('void encode_task(',a)
        initial=source[a:b]
        # Globals/locks/helpers up to encoding; encode_task itself is the hardware boundary.
        network=(ROOT/'main/network_sync.cpp').read_text()
        parts=[PRELUDE,extract(network,'class HttpGuard')+';',extract(network,'esp_err_t read_http_body'),
               extract(network,'extern "C" int network_sync_stream_request'),initial,extract(source,'struct Reply')+';']
        names=['Reply parse_reply','memo_job_state_t remote_state','void selected_notice','void clear_selected_notice','void accept_reply','bool active_marker_matches','bool clear_active_marker','bool mark_remote_audio','void release_active','esp_err_t restore_active_record','void make_path','int request','bool online','void sha_hex','int finish_remote','void query_selected','void transfer_task']
        parts += [extract(source,name) for name in names]
        boot='extern "C" int fixture_restore_marker(){metadata_fault=false;active_id=0;recovery_mode=false;encoder_done=true;record_count=counter=0;for(auto &item:durable){if(item.first[0]==114){memo_cardless_record_t r{};if(!memo_cardless_record_decode(item.second.data(),item.second.size(),&r))return ESP_FAIL;records[record_count++]=r;counter=std::max(counter,MEMO_CARDLESS_DISPLAY_ID(r.id));}}return restore_active_record();}'
        parts += [extract(source,'extern "C" bool memo_cardless_get'),'static int ensure_network_task(){return ESP_OK;}',
                  extract(source,'extern "C" esp_err_t memo_cardless_begin'),extract(source,'extern "C" bool memo_cardless_busy'),WRAPPERS,boot]
        cpp=cls.directory/'cardless-runtime.cpp';cpp.write_text('\n'.join(parts));obj=cls.directory/'cardless-runtime.o'
        crypto=ROOT/'.tools/esp-idf/components/mbedtls/mbedtls'
        (cls.directory/'native-mbedtls.h').write_text('#define MBEDTLS_SHA256_C\n')
        crypto_flags=['-I',str(cls.directory),'-I',str(crypto/'include'),'-DMBEDTLS_CONFIG_FILE="native-mbedtls.h"']
        subprocess.run(['c++',*crypto_flags,'-std=c++17','-fPIC','-g','-O1','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-DMEMO_HOST_TEST',
          '-I',str(ROOT/'main'),'-I',str(ROOT/'.tools/esp-idf/components/json/cJSON'),'-c',str(cpp),'-o',str(obj)],check=True)
        objects=[]
        for name in ('sha256','platform_util'):
            o=cls.directory/(name+'.o');subprocess.run(['cc','-std=c11','-fPIC','-g','-O1','-Wall','-Wextra','-Werror',*crypto_flags,'-c',str(crypto/'library'/(name+'.c')),'-o',str(o)],check=True);objects.append(str(o))
        for name in ('memo_cardless_buffer','memo_cardless_state'):
            o=cls.directory/(name+'.o');subprocess.run(['cc','-std=c11','-fPIC','-g','-O1','-Wall','-Wextra','-Werror','-DMEMO_HOST_TEST','-I',str(ROOT/'main'),'-c',str(ROOT/'main'/(name+'.c')),'-o',str(o)],check=True);objects.append(str(o))
        library=cls.directory/'cardless-runtime.so';subprocess.run(['c++','-shared',str(obj),*objects,str(cls.directory/'memo_cardless_record.o'),str(cls.directory/'memo_sync_cache.o'),str(cls.directory/'memo_network_config.o'),str(cls.directory/'cJSON.o'),'-o',str(library)],check=True)
        cls.runtime_lib=C.CDLL(str(library));lib=cls.runtime_lib
        lib.fixture_reset.argtypes=[C.c_uint32,C.POINTER(Target)];lib.fixture_transport.argtypes=[TRANSPORT,C.c_char_p,C.c_char_p];lib.fixture_push.argtypes=[C.c_void_p,C.c_size_t];lib.fixture_push.restype=C.c_bool
        lib.fixture_run.argtypes=[C.c_int];lib.fixture_active.restype=C.c_uint32;lib.fixture_packets.restype=C.c_size_t
        lib.fixture_begin.argtypes=[C.c_int,C.POINTER(C.c_uint32)];lib.fixture_begin_next.argtypes=[C.POINTER(C.c_uint32)];lib.fixture_restore_marker.restype=C.c_int;
        lib.memo_cardless_busy.restype=C.c_bool;
        lib.fixture_target.argtypes=[C.POINTER(Target)];lib.fixture_reboot.restype=C.c_bool
        cls.chunks=synthetic_chunks(frames=250)
    def setUp(self):
        super().setUp();self.rid=0x80000001;self.runtime_lib.fixture_reset(self.rid,C.byref(self.target));self.calls=[]
        self.lost_finish=False;self.block_get=False;self.query_status=0;self.bad_ack=False;self.bad_ack_left=0;self.held_on_bad_ack=[]
        def transport(method,path,id,body,length,content_type,hash,out,capacity):
            try:
                method=method.decode();path=path.decode();self.calls.append((method,path))
                if self.block_get and method=='GET':return 0
                if self.query_status and method=='GET':return self.query_status
                headers={'Authorization':'Bearer '+self.token,'X-Device-ID':self.device,'X-Recording-ID':str(id)}
                if content_type:headers['Content-Type']=content_type.decode()
                if hash:headers['X-Chunk-SHA256']=hash.decode()
                response=self.fixture.client.request(method,self.fixture.url+path,content=C.string_at(body,length) if length else None,headers=headers)
                if self.lost_finish and path.endswith('/finish'):
                    self.lost_finish=False;self.block_get=True;return 0
                if self.bad_ack and method=='PUT':
                    import json
                    self.held_on_bad_ack.append(self.runtime_lib.fixture_packets())
                    self.bad_ack_left-=1
                    if not self.bad_ack_left:self.bad_ack=False
                    value=response.json();value['next_seq']=value['next_seq']+0.5
                    encoded=json.dumps(value).encode();C.memset(out,0,capacity);C.memmove(out,encoded,len(encoded));return response.status_code
                C.memset(out,0,capacity)
                if len(response.content)>=capacity:return 0
                C.memmove(out,response.content,len(response.content));return response.status_code
            except Exception as error:self.callbacks.append(str(error));return 0
        self.transport=TRANSPORT(transport);self.runtime_lib.fixture_transport(self.transport,self.fixture.url.encode(),self.token.encode())
    def push(self,chunk):self.assertTrue(self.runtime_lib.fixture_push(chunk,len(chunk)))
    def tick(self,n=12):self.runtime_lib.fixture_run(n);self.assertEqual(self.callbacks,[])
    def test_paired_opus_pending_restarts_both_ends_then_delivers_once(self):
        for chunk in self.chunks:self.push(chunk)
        self.lost_finish=True;self.tick(8)
        self.assertNotEqual(self.runtime_lib.fixture_active(),0)
        self.reboot_pairing_and_server()
        self.assertTrue(self.runtime_lib.fixture_reboot());self.block_get=False;self.tick()
        self.work();self.runtime_lib.fixture_query(False)
        self.assertEqual(self.runtime_lib.fixture_state(),9)
        self.assertEqual((len(self.asr),len(self.deliveries)),(1,1))
        self.reboot_pairing_and_server();self.assertTrue(self.runtime_lib.fixture_reboot())
        self.runtime_lib.fixture_query(False)
        self.assertEqual(self.runtime_lib.fixture_state(),9)
        self.assertEqual((len(self.asr),len(self.deliveries)),(1,1))
    def test_paired_opus_retry_restarts_both_ends_preserving_text(self):
        self.mode='delivery_fail'
        for chunk in self.chunks:self.push(chunk)
        self.tick();self.work();self.runtime_lib.fixture_query(False)
        self.assertNotEqual(self.runtime_lib.fixture_state(),9)
        self.assertTrue(self.runtime_lib.fixture_can_retry())
        self.reboot_pairing_and_server();self.assertTrue(self.runtime_lib.fixture_reboot())
        self.mode='ok';self.runtime_lib.fixture_query(False)
        self.assertTrue(self.runtime_lib.fixture_can_retry())
        self.runtime_lib.fixture_query(True)
        self.reboot_pairing_and_server();self.work();self.runtime_lib.fixture_query(False)
        self.assertEqual(self.runtime_lib.fixture_state(),9)
        self.assertEqual((len(self.asr),len(self.deliveries)),(1,1))
    def test_cardless_create_ack_finish_real_R02(self):
        for chunk in self.chunks:self.push(chunk)
        self.tick();self.assertEqual(self.runtime_lib.fixture_active(),0);self.work()
        status=self.fixture.client.get(self.fixture.url+f'/v1/streams/{self.device}/{self.rid}',headers={'Authorization':'Bearer '+self.token}).json()
        self.assertEqual(status['status'],'sent');self.assertEqual(status['received_samples'],160000);self.assertFalse(status['interrupted']);self.assertEqual(len(self.deliveries),1)
    def test_cardless_lost_ack_replays_same_chunk_once(self):
        for chunk in self.chunks:self.push(chunk)
        self.runtime_lib.fixture_drop(2);self.tick(20);self.assertEqual(self.runtime_lib.fixture_packets(),0);self.assertEqual(self.runtime_lib.fixture_active(),0)
        puts=[p for m,p in self.calls if m=='PUT'];self.assertEqual(sum(p.endswith('/0') for p in puts),2)
        self.work();self.assertEqual(len(self.deliveries),1)
    def test_cardless_lost_finish_ack_then_reboot_keeps_full_result(self):
        for chunk in self.chunks:self.push(chunk)
        self.lost_finish=True;self.tick(8);self.assertNotEqual(self.runtime_lib.fixture_active(),0)
        self.work();self.assertTrue(self.runtime_lib.fixture_reboot());self.block_get=False;self.tick()
        self.assertEqual(self.runtime_lib.fixture_active(),0);self.assertEqual(self.runtime_lib.fixture_state(),9)
        status=self.fixture.client.get(self.fixture.url+f'/v1/streams/{self.device}/{self.rid}',headers={'Authorization':'Bearer '+self.token}).json()
        self.assertFalse(status['interrupted']);self.assertEqual(status['received_samples'],160000);self.assertEqual(len(self.deliveries),1)
    def test_cardless_reboot_finishes_only_confirmed_prefix(self):
        for chunk in self.chunks:self.push(chunk)
        self.tick(2);self.assertEqual(self.runtime_lib.fixture_packets(),250)
        self.assertTrue(self.runtime_lib.fixture_reboot());self.runtime_lib.fixture_power(False);self.tick(2)
        self.assertNotEqual(self.runtime_lib.fixture_active(),0)
        self.runtime_lib.fixture_power(True);self.tick();self.assertEqual(self.runtime_lib.fixture_active(),0);self.work()
        status=self.fixture.client.get(self.fixture.url+f'/v1/streams/{self.device}/{self.rid}',headers={'Authorization':'Bearer '+self.token}).json()
        self.assertTrue(status['interrupted']);self.assertEqual(status['received_samples'],80000)
    def test_cardless_target_change_sends_no_old_task(self):
        self.push(self.chunks[0]);other=Target();C.memmove(C.byref(other),C.byref(self.target),C.sizeof(Target));other.instance.bytes[0]^=1
        self.runtime_lib.fixture_target(C.byref(other));self.tick(4)
        self.assertEqual(self.calls,[]);self.assertNotEqual(self.runtime_lib.fixture_active(),0);self.assertEqual(self.runtime_lib.fixture_packets(),250)
    def test_cardless_metadata_failure_keeps_unique_slot(self):
        for chunk in self.chunks:self.push(chunk)
        self.runtime_lib.fixture_fault(1);self.tick(8)
        self.assertNotEqual(self.runtime_lib.fixture_active(),0);self.assertTrue(self.runtime_lib.fixture_faulted())
        self.assertGreater(self.runtime_lib.fixture_reads(),0)

    def test_cardless_query_auth_and_timeout_keep_known_sent(self):
        for chunk in self.chunks:self.push(chunk)
        self.tick();self.work();self.runtime_lib.fixture_query(False);self.assertEqual(self.runtime_lib.fixture_state(),9)
        self.query_status=401;self.runtime_lib.fixture_query(False);self.assertEqual(self.runtime_lib.fixture_state(),9)
        self.query_status=0;self.block_get=True;self.runtime_lib.fixture_query(False);self.assertEqual(self.runtime_lib.fixture_state(),9)
    def test_cardless_unknown_commit_result_never_releases_unique_slot(self):
        for chunk in self.chunks:self.push(chunk)
        self.runtime_lib.fixture_fault(2);self.tick(8)
        self.assertNotEqual(self.runtime_lib.fixture_active(),0);self.assertTrue(self.runtime_lib.fixture_faulted())
        self.runtime_lib.fixture_fault(0);self.assertTrue(self.runtime_lib.fixture_reboot());self.tick()
        self.assertEqual(self.runtime_lib.fixture_active(),0)

    def test_cardless_fractional_ack_never_releases_packets(self):
        for chunk in self.chunks:self.push(chunk)
        self.bad_ack=True;self.bad_ack_left=2;self.tick(20)
        self.assertEqual(self.held_on_bad_ack,[500,500])
        self.assertEqual(self.runtime_lib.fixture_packets(),0);self.assertEqual(self.runtime_lib.fixture_active(),0)

    def test_cardless_closed_task_transport_rechecks_changed_target(self):
        for chunk in self.chunks:self.push(chunk)
        self.tick();self.work();self.runtime_lib.fixture_query(False);self.assertEqual(self.runtime_lib.fixture_state(),9)
        count=len(self.calls);self.runtime_lib.fixture_swap_before_transport();self.runtime_lib.fixture_query(False)
        self.assertEqual(len(self.calls),count);self.assertEqual(self.runtime_lib.fixture_state(),9)

    def test_actual_begin_marker_fault_then_target_change_reboot_unblocks(self):
        for cut in (1,2,3):
            with self.subTest(marker_fault=cut):
                self.runtime_lib.fixture_reset(self.rid,C.byref(self.target));id=C.c_uint32(0)
                self.assertNotEqual(self.runtime_lib.fixture_begin(cut,C.byref(id)),0);self.assertEqual(id.value,0)
                self.assertFalse(self.runtime_lib.memo_cardless_busy())
                other=Target();C.memmove(C.byref(other),C.byref(self.target),C.sizeof(Target));other.instance.bytes[0]^=1
                self.runtime_lib.fixture_target(C.byref(other));self.runtime_lib.fixture_restore_io()
                before=len(self.calls);self.assertEqual(self.runtime_lib.fixture_restore_marker(),0);self.tick(3)
                self.assertEqual(self.runtime_lib.fixture_active(),0);self.assertEqual(len(self.calls),before)
                new_id=C.c_uint32(0);self.assertEqual(self.runtime_lib.fixture_begin_next(C.byref(new_id)),0);self.assertGreater(new_id.value,0x80000001)
    def test_zero_remote_audio_reboot_releases_without_current_server(self):
        self.assertTrue(self.runtime_lib.fixture_reboot());self.assertEqual(self.runtime_lib.fixture_active(),0);self.assertEqual(self.calls,[])

    def test_remote_audio_possibility_write_fault_retains_unconfirmed_packets(self):
        for cut in (1,2,3):
            with self.subTest(cut=cut):
                self.runtime_lib.fixture_reset(self.rid,C.byref(self.target))
                for chunk in self.chunks:self.push(chunk)
                self.runtime_lib.fixture_audio_flag_fault(cut);start=len(self.calls);self.tick(5)
                self.assertEqual(self.runtime_lib.fixture_packets(),500);self.assertTrue(self.runtime_lib.memo_cardless_busy())
                self.assertFalse(any(method=='PUT' for method,_ in self.calls[start:]))
                self.runtime_lib.fixture_restore_io()
                self.assertTrue(self.runtime_lib.fixture_reboot());self.tick(5)
                self.assertEqual(self.runtime_lib.fixture_active(),0)

# Avoid rerunning the unrelated inherited SD methods; they have their own suite.
for name in list(SyncTests.__dict__):
    if name.startswith('test_'):setattr(CardlessTests,name,None)
del SyncTests
if __name__=='__main__':unittest.main(verbosity=2)
