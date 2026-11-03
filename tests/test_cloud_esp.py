"""Execute the production ESP adapter with explicit SDK/RTOS substitutes.

Real TLS/WS peers are tested separately in test_cloud_protocol.py. This suite
checks the actual firmware callbacks, flags, fragment queues and ownership;
it is not a radio, certificate handshake or device-memory measurement.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SHIM = r'''
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <set>
#include <map>
using esp_err_t=int;
constexpr int ESP_OK=0,HTTP_METHOD_POST=1,HTTP_EVENT_ON_HEADER=2,HTTP_EVENT_ON_DATA=3;
constexpr int MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;
using esp_event_base_t=const char*;
struct T{};using esp_transport_handle_t=T*;
struct esp_transport_keep_alive_t {bool keep_alive_enable;int keep_alive_idle,keep_alive_interval,keep_alive_count;};
struct Http;using esp_http_client_handle_t=Http*;
struct esp_http_client_event_t {int event_id;char*header_key;char*header_value;void*user_data;int data_len=0;};
struct esp_http_client_config_t {
 const char *url;int(*crt_bundle_attach)(void*);bool disable_auto_redirect;int timeout_ms,buffer_size,buffer_size_tx;
 bool keep_alive_enable;int keep_alive_idle,keep_alive_interval,keep_alive_count;
 void *user_data;int(*event_handler)(esp_http_client_event_t*);T*transport;
};
using esp_log_level_t=int;constexpr int ESP_LOG_NONE=0,ESP_LOG_INFO=3;
esp_log_level_t esp_log_level_get(const char*);void esp_log_level_set(const char*,esp_log_level_t);
int esp_crt_bundle_attach(void*);int64_t esp_timer_get_time();
Http*esp_http_client_init(const esp_http_client_config_t*);
int esp_http_client_set_method(Http*,int);int esp_http_client_set_header(Http*,const char*,const char*);
int esp_http_client_open(Http*,int);int esp_http_client_set_timeout_ms(Http*,int);
int esp_http_client_write(Http*,const char*,int);int64_t esp_http_client_fetch_headers(Http*);
int esp_http_client_get_status_code(Http*);int esp_http_client_read(Http*,char*,size_t);
bool esp_http_client_is_complete_data_received(Http*);int esp_http_client_close(Http*);int esp_http_client_cleanup(Http*);
using TickType_t=uint32_t;
#define pdMS_TO_TICKS(x) (x)
struct Message {size_t capacity=0,used=0;std::deque<std::string> queue;};
using MessageBufferHandle_t=Message*;using StaticMessageBuffer_t=Message;
Message*xMessageBufferCreateStatic(size_t,uint8_t*,Message*);
size_t xMessageBufferSend(Message*,const void*,size_t,TickType_t);
size_t xMessageBufferReceive(Message*,void*,size_t,TickType_t);
void vMessageBufferDelete(Message*);
void*heap_caps_malloc(size_t,int);void shim_free(void*);
struct Ws;using esp_websocket_client_handle_t=Ws*;
using Handler=void(*)(void*,const char*,int32_t,void*);
struct esp_transport_ws_config_t {const char *ws_path=nullptr,*headers=nullptr;bool propagate_control_frames=false;};
T*esp_transport_ws_init(T*);int esp_transport_ws_set_config(T*,const esp_transport_ws_config_t*);void esp_transport_set_default_port(T*,int);int esp_transport_destroy(T*);
struct esp_websocket_client_config_t {
 const char *uri,*headers;T*ext_transport;int(*crt_bundle_attach)(void*);bool disable_auto_reconnect,enable_close_reconnect;
 int task_stack,buffer_size,network_timeout_ms,ping_interval_sec,pingpong_timeout_sec;
};
enum {WEBSOCKET_EVENT_ANY=-1,WEBSOCKET_EVENT_ERROR,WEBSOCKET_EVENT_CONNECTED,WEBSOCKET_EVENT_DISCONNECTED,WEBSOCKET_EVENT_DATA,WEBSOCKET_EVENT_CLOSED};
enum {WEBSOCKET_ERROR_TYPE_NONE,WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT,WEBSOCKET_ERROR_TYPE_SERVER_CLOSE};
struct Error {int esp_tls_cert_verify_flags=0,esp_ws_handshake_status_code=0,error_type=WEBSOCKET_ERROR_TYPE_NONE;};
struct esp_websocket_event_data_t {const char*data_ptr=nullptr;int data_len=0;bool fin=false;uint8_t op_code=0;int payload_len=0,payload_offset=0;Error error_handle;};
Ws*esp_websocket_client_init(const esp_websocket_client_config_t*);
int esp_websocket_register_events(Ws*,int,Handler,void*);
int esp_websocket_client_start(Ws*);int esp_websocket_client_send_text(Ws*,const char*,size_t,TickType_t);
int esp_websocket_client_stop(Ws*);int esp_websocket_client_destroy(Ws*);
'''
HARNESS = r'''
#include "memo_cloud_esp.h"
#include <cassert>
#include <climits>
#include <thread>
static std::thread::id owner=std::this_thread::get_id();
static std::set<void*> allocations;
static int handles=0,leases=0,closes=0,flags=0;
static bool fail_alloc=false,fail_open=false,clock_ready=true,in_callback=false;
static bool fail_arena=false;static int arena_failures=0,http_status=429;
struct Http {esp_http_client_config_t cfg;int returned=0;};
struct Ws {esp_websocket_client_config_t cfg;Handler callback=nullptr;void*context=nullptr;};
static Ws*last_ws=nullptr;
static bool interrupted=false;
static std::map<std::string,int> log_levels;
esp_log_level_t esp_log_level_get(const char*tag){auto it=log_levels.find(tag);return it==log_levels.end()?ESP_LOG_INFO:it->second;}
void esp_log_level_set(const char*tag,int level){log_levels[tag]=level;}
static bool logs_restored(){return esp_log_level_get("HTTP_CLIENT")==ESP_LOG_INFO&&esp_log_level_get("transport_ws")==ESP_LOG_INFO;}
T*esp_transport_ws_init(T*){handles++;return new T;}
int esp_transport_ws_set_config(T*,const esp_transport_ws_config_t*c){assert(c->headers&&strstr(c->headers,"synthetic")&&c->propagate_control_frames);return 0;}
void esp_transport_set_default_port(T*,int port){assert(port==443);}
int esp_transport_destroy(T*t){delete t;handles--;return 0;}
extern "C" bool network_sync_cloud_acquire(){leases++;return true;}
extern "C" void network_sync_cloud_release(){assert(!in_callback);leases--;}
extern "C" bool network_sync_cloud_clock_ready(){return clock_ready;}
int esp_crt_bundle_attach(void*){return 0;}
int64_t esp_timer_get_time(){return 1000000;}
void*heap_caps_malloc(size_t n,int caps){assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));if(fail_alloc)return nullptr;if(fail_arena&&n<7168){arena_failures++;return nullptr;}void*p=malloc(n);allocations.insert(p);return p;}
void shim_free(void*p){if(p){assert(allocations.erase(p)==1);free(p);}}
MemoHttpTransport::MemoHttpTransport(esp_http_client_config_t&c,int64_t,bool){slot_=0;outer_=reinterpret_cast<T*>(1);inner_=reinterpret_cast<T*>(2);c.transport=outer_;}
MemoHttpTransport::~MemoHttpTransport(){}
void MemoHttpTransport::interrupt(){interrupted=true;}
void MemoHttpTransport::read_window(){}
void MemoHttpTransport::write_window(){}
static int64_t send_limit=0;
void MemoHttpTransport::send_until(int64_t deadline){send_limit=deadline;}
int MemoHttpTransport::tls_verify_flags()const{return flags;}
Http*esp_http_client_init(const esp_http_client_config_t*c){assert(c->crt_bundle_attach==esp_crt_bundle_attach&&c->disable_auto_redirect&&c->timeout_ms==10000);handles++;return new Http{*c};}
int esp_http_client_set_method(Http*,int method){assert(method==HTTP_METHOD_POST);return 0;}
int esp_http_client_set_header(Http*,const char*,const char*){return 0;}
int esp_http_client_open(Http*,int){return fail_open?-1:0;}
int esp_http_client_set_timeout_ms(Http*,int n){assert(n>0&&n<=10000);return 0;}
int esp_http_client_write(Http*,const char*,int n){return n;}
int64_t esp_http_client_fetch_headers(Http*h){char key[]="rEtRy-AfTeR",value[]="301";esp_http_client_event_t e{HTTP_EVENT_ON_HEADER,key,value,h->cfg.user_data};h->cfg.event_handler(&e);esp_http_client_event_t body{};body.event_id=HTTP_EVENT_ON_DATA;body.user_data=h->cfg.user_data;body.data_len=4;h->cfg.event_handler(&body);return 4;}
int esp_http_client_get_status_code(Http*){return http_status;}
int esp_http_client_read(Http*h,char*out,size_t cap){size_t n=std::min<size_t>(4-h->returned,cap);memset(out,'x',n);h->returned+=n;return n;}
bool esp_http_client_is_complete_data_received(Http*){return true;}
int esp_http_client_close(Http*){assert(!in_callback);closes++;return 0;}
int esp_http_client_cleanup(Http*h){handles--;delete h;return 0;}
Message*xMessageBufferCreateStatic(size_t n,uint8_t*,Message*m){m->capacity=n;return m;}
size_t xMessageBufferSend(Message*m,const void*p,size_t n,TickType_t wait){assert(wait==0);if(m->used+n+sizeof(size_t)>=m->capacity)return 0;m->queue.emplace_back(static_cast<const char*>(p),n);m->used+=n+sizeof(size_t);return n;}
size_t xMessageBufferReceive(Message*m,void*p,size_t n,TickType_t){if(m->queue.empty())return 0;auto&s=m->queue.front();if(s.size()>n)return 0;size_t got=s.size();memcpy(p,s.data(),got);m->used-=got+sizeof(size_t);m->queue.pop_front();return got;}
void vMessageBufferDelete(Message*m){assert(!in_callback);m->queue.clear();}
Ws*esp_websocket_client_init(const esp_websocket_client_config_t*c){
 assert(c->crt_bundle_attach==esp_crt_bundle_attach&&c->disable_auto_reconnect&&!c->enable_close_reconnect&&c->ext_transport);interrupted=false;
 assert(c->task_stack==8192&&c->buffer_size==4096&&c->network_timeout_ms==10000&&c->ping_interval_sec==10&&c->pingpong_timeout_sec==20);
 assert(esp_log_level_get("transport_ws")==ESP_LOG_NONE&&esp_log_level_get("HTTP_CLIENT")==ESP_LOG_NONE);
 handles++;last_ws=new Ws{*c};return last_ws;
}
int esp_websocket_register_events(Ws*w,int,Handler handler,void*context){w->callback=handler;w->context=context;return 0;}
int esp_websocket_client_start(Ws*){return 0;}
int esp_websocket_client_send_text(Ws*,const char*,size_t n,TickType_t wait){assert(wait<=10000&&send_limit==1000000+static_cast<int64_t>(wait)*1000);return n;}
int esp_websocket_client_stop(Ws*){assert(!in_callback&&std::this_thread::get_id()==owner&&interrupted);closes++;return 0;}
int esp_websocket_client_destroy(Ws*w){handles--;delete w;return 0;}
static void emit(int type,esp_websocket_event_data_t*e=nullptr){in_callback=true;last_ws->callback(last_ws->context,"WS",type,e);in_callback=false;}
static void data(const char*p,int n,int opcode,bool fin,int size,int offset){esp_websocket_event_data_t e{};e.data_ptr=p;e.data_len=n;e.op_code=opcode;e.fin=fin;e.payload_len=size;e.payload_offset=offset;emit(WEBSOCKET_EVENT_DATA,&e);}
int main(){std::atomic<bool>cancelled{false};
 for(int round=0;round<20;round++){
  MemoCloudEsp platform(cancelled);const auto&io=platform.io();assert(io.acquire(io.context));
  assert(io.http_open(io.context,"https://example.test","token synthetic",123,31000)==0);
  uint32_t retry=0;assert(io.http_headers(io.context,&retry,1000)==429&&retry==301);
  assert(!io.http_complete(io.context));char body[4];assert(io.http_read(io.context,body,4,1000)==4);assert(io.http_complete(io.context));io.http_close(io.context);assert(!handles);
  assert(io.ws_open(io.context,"wss://example.test","Authorization: Bearer synthetic\r\n",16000)==0);
  assert(io.ws_send(io.context,"x",1,2000)==1&&send_limit==0);
  // SDK chunks within a frame, then a continuation frame; UTF-8 may split.
  data("ab",2,1,false,3,0);data("c",1,1,false,3,2);data("def",3,0,true,3,0);
  char out[65536];assert(io.ws_read(io.context,out,sizeof(out),0)==6&&!memcmp(out,"abcdef",6));
  if(round==0){
   std::string first(32768,'a'),second(32768,'b');
   data(first.data(),first.size(),1,false,first.size(),0);
   data(second.data(),second.size(),0,true,second.size(),0);
   constexpr char finished[]="{\"type\":\"session.finished\"}";
   data(finished,sizeof(finished)-1,1,true,sizeof(finished)-1,0);
   assert(io.ws_read(io.context,out,sizeof(out),0)==sizeof(out));
   assert(!memcmp(out,first.data(),first.size())&&!memcmp(out+first.size(),second.data(),second.size()));
   assert(io.ws_read(io.context,out,sizeof(out),0)==sizeof(finished)-1);
   assert(!memcmp(out,finished,sizeof(finished)-1));
  }
  data("final",5,1,true,5,0);emit(WEBSOCKET_EVENT_CLOSED);
  assert(io.ws_read(io.context,out,sizeof(out),0)==5);assert(io.ws_read(io.context,out,sizeof(out),0)<0);
  io.ws_close(io.context);io.release(io.context);
  assert(!handles&&!leases&&allocations.empty()&&logs_restored());
 }
 for(int fault=0;fault<9;fault++){
  MemoCloudEsp platform(cancelled);const auto&io=platform.io();assert(io.acquire(io.context));
  assert(io.ws_open(io.context,"wss://example.test","synthetic",16000)==0);
  if(fault==0){esp_websocket_event_data_t e{};e.error_handle.error_type=WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT;e.error_handle.esp_tls_cert_verify_flags=1;emit(WEBSOCKET_EVENT_ERROR,&e);assert(io.failure(io.context)==memo_cloud::Code::Tls);}
  if(fault==0){emit(WEBSOCKET_EVENT_DISCONNECTED);assert(io.failure(io.context)==memo_cloud::Code::Tls);}
  if(fault==1)data("x",1,0,true,1,0); // continuation without start
  if(fault==2)data("x",1,1,true,1,1); // impossible offset
  if(fault==3){std::string large(65536,'x');data(large.data(),large.size(),1,true,large.size(),0);data(large.data(),large.size(),1,true,large.size(),0);}
  // Actual SDK dispatch initializes TLS flags only for TCP_TRANSPORT. Poison
  // the other types as an explicit validity test, rather than zero-fill them.
  if(fault==4||fault==5){esp_websocket_event_data_t e{};e.error_handle.esp_ws_handshake_status_code=fault==4?401:429;e.error_handle.esp_tls_cert_verify_flags=0x5a5a;emit(WEBSOCKET_EVENT_ERROR,&e);e.error_handle.error_type=WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT;e.error_handle.esp_tls_cert_verify_flags=0;emit(WEBSOCKET_EVENT_DISCONNECTED,&e);assert(io.failure(io.context)==(fault==4?memo_cloud::Code::Credentials:memo_cloud::Code::Limited));}
  if(fault==6){emit(WEBSOCKET_EVENT_ERROR);esp_websocket_event_data_t e{};e.error_handle.error_type=WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT;e.error_handle.esp_tls_cert_verify_flags=1;emit(WEBSOCKET_EVENT_DISCONNECTED,&e);assert(io.failure(io.context)==memo_cloud::Code::Tls);}
  if(fault==7){esp_websocket_event_data_t e{};e.error_handle.error_type=WEBSOCKET_ERROR_TYPE_SERVER_CLOSE;e.error_handle.esp_tls_cert_verify_flags=0x5a5a;emit(WEBSOCKET_EVENT_CLOSED,&e);assert(io.failure(io.context)==memo_cloud::Code::Transport);}
  if(fault==8){esp_websocket_event_data_t e{};e.error_handle.esp_tls_cert_verify_flags=0x5a5a;flags=1;emit(WEBSOCKET_EVENT_ERROR,&e);flags=0;assert(io.failure(io.context)==memo_cloud::Code::Tls);}
  char out[16];assert(io.ws_read(io.context,out,sizeof(out),0)<0);io.ws_close(io.context);io.release(io.context);
  assert(!handles&&!leases&&allocations.empty()&&logs_restored());
 }
 {MemoCloudEsp platform(cancelled);auto&io=platform.io();assert(io.acquire(io.context));fail_alloc=true;assert(io.ws_open(io.context,"wss://example.test","synthetic",16000)<0);io.ws_close(io.context);fail_alloc=false;io.release(io.context);}
 {MemoCloudEsp platform(cancelled);auto&io=platform.io();assert(io.acquire(io.context));fail_open=true;flags=1;assert(io.http_open(io.context,"https://example.test","synthetic",12,31000)<0);assert(io.failure(io.context)==memo_cloud::Code::Tls);io.http_close(io.context);io.release(io.context);fail_open=false;}
 {MemoCloudEsp platform(cancelled);MemoCloudEsp second(cancelled);auto&a=platform.io();auto&b=second.io();assert(a.acquire(a.context));assert(!b.acquire(b.context));a.release(a.context);assert(b.acquire(b.context));b.release(b.context);}
 // Compile the production core's ESP branch: PSRAM arena failure must return
 // Memory without falling through to default/internal malloc.
 {MemoCloudEsp platform(cancelled);char wav[64]{};http_status=200;fail_arena=true;
  memo_cloud::Source source{wav,sizeof(wav),[](void*p,uint64_t at,void*out,size_t n){memcpy(out,static_cast<char*>(p)+at,n);return static_cast<int>(n);}};
  memo_cloud::Sink sink{nullptr,[](void*,const char*,size_t){return true;}};
  assert(memo_cloud::file_asr(platform.io(),"synthetic",source,sink).code==memo_cloud::Code::Memory);
  assert(arena_failures==1&&allocations.empty()&&!leases&&logs_restored());fail_arena=false;http_status=429;
 }
 assert(!handles&&!leases&&allocations.empty()&&closes>0);
}
'''

class AdapterTests(unittest.TestCase):
    def test_actual_sdk_callbacks_flags_fragments_and_owner_cleanup(self):
        with tempfile.TemporaryDirectory(prefix='cloud-esp-', dir=ROOT/'private/host-tests') as folder:
            p=Path(folder)
            (p/'shim.h').write_text(SHIM)
            for name in ('esp_http_client.h','esp_transport.h','esp_websocket_client.h','esp_crt_bundle.h','esp_heap_caps.h','esp_timer.h','esp_transport_ws.h','esp_log.h'):
                (p/name).write_text('#include "shim.h"\n')
            (p/'freertos').mkdir();(p/'freertos/message_buffer.h').write_text('#include "shim.h"\n')
            (p/'prelude.h').write_text('#include "shim.h"\n#define free shim_free\n')
            (p/'test.cpp').write_text(HARNESS)
            include=ROOT/'.tools/esp-idf/components/json/cJSON'
            args=['c++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g','-I'+str(p),'-I'+str(ROOT/'main'),'-I'+str(include)]
            subprocess.run([*args,'-include',str(p/'prelude.h'),'-c',str(ROOT/'main/memo_cloud_esp.cpp'),'-o',str(p/'esp.o')],check=True)
            subprocess.run(['cc','-c',str(include/'cJSON.c'),'-I'+str(include),'-o',str(p/'json.o')],check=True)
            subprocess.run(['cc','-fsanitize=address,undefined','-c',str(ROOT/'main/memo_cloud_json.c'),'-I'+str(include),'-o',str(p/'private-json.o')],check=True)
            subprocess.run([*args,'-DESP_PLATFORM','-include',str(p/'prelude.h'),'-c',str(ROOT/'main/memo_cloud.cpp'),'-o',str(p/'core.o')],check=True)
            subprocess.run([*args,str(p/'test.cpp'),str(p/'core.o'),str(p/'esp.o'),str(p/'json.o'),str(p/'private-json.o'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)

if __name__=='__main__':
    unittest.main(verbosity=2)
