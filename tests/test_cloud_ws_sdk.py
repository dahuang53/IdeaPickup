"""Locked SDK handshake/receive functions against the production socket guard.

TLS, time and RTOS use deterministic shims/socket pairs. The SDK function
bodies are read from pinned dependencies, not reimplemented. No RF claim.
"""
from pathlib import Path
import hashlib
import re
import subprocess
import tempfile
import unittest
from urllib.request import urlopen
from test_http_deadline import HEADER, HARNESS
from test_service_runtime import extract

ROOT=Path(__file__).resolve().parents[1]
WS_SHA='50e375e283596439a4128555935cce7fbc0b77a242e288fb7b4c0aa4287495ca'
WS_URL='https://raw.githubusercontent.com/espressif/esp-protocols/websocket-v1.5.0/components/esp_websocket_client/esp_websocket_client.c'

def locked_websocket_source(managed, cache):
    # Host CI installs IDF but need not build firmware/resolve managed components.
    # Obtain only this public pinned source, with the same digest as the registry
    # component. A missing source or digest mismatch fails, never skips the test.
    if managed.is_file():
        data=managed.read_bytes()
    elif cache.is_file():
        data=cache.read_bytes()
    else:
        with urlopen(WS_URL,timeout=30) as response:
            data=response.read(1024*1024+1)
        if len(data)>1024*1024:
            raise ValueError('WebSocket SDK source exceeds limit')
        if hashlib.sha256(data).hexdigest()!=WS_SHA:
            raise ValueError('WebSocket SDK source digest mismatch')
        cache.parent.mkdir(parents=True,exist_ok=True)
        with tempfile.NamedTemporaryFile(dir=cache.parent,delete=False) as output:
            temporary=Path(output.name)
            output.write(data)
        temporary.replace(cache)
    if hashlib.sha256(data).hexdigest()!=WS_SHA:
        raise ValueError('WebSocket SDK source digest mismatch')
    return data.decode('utf-8')

SUPPORT=r'''
#include <cstdarg>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
static int log_level=3;
static std::vector<std::string> logs;
static void sdk_log(int level,const char*,const char*format,...){
 if(level>log_level)return;
 char message[4096];va_list args;va_start(args,format);vsnprintf(message,sizeof(message),format,args);va_end(args);logs.emplace_back(message);
}
#define ESP_LOGE(tag,...) sdk_log(1,tag,__VA_ARGS__)
#define ESP_LOGD(tag,...) sdk_log(4,tag,__VA_ARGS__)
#define ESP_LOGV(tag,...) sdk_log(5,tag,__VA_ARGS__)
static const char*TAG="transport_ws";
static const char*esp_err_to_name(int){return "synthetic";}
#define WS_BUFFER_SIZE 2048
#define WS_HTTP_TEMPORARY_REDIRECT(x) ((x)==307 || (x)==302)
#define WS_HTTP_PERMANENT_REDIRECT(x) ((x)==301 || (x)==308)
struct transport_ws_t {T*parent;char buffer[2048]{};const char*path="/",*user_agent=nullptr,*sub_protocol=nullptr,*auth=nullptr,*headers=nullptr;int buffer_len=0,http_status_code=0;char*redir_host=nullptr;};
static ssize_t getrandom(void*p,size_t n,int){memset(p,1,n);return n;}
static int esp_crypto_base64_encode(unsigned char*out,size_t,size_t*length,const unsigned char*,size_t){strcpy(reinterpret_cast<char*>(out),"synthetic-key");*length=13;return 0;}
static int esp_crypto_sha1(const unsigned char*,size_t,unsigned char*out){memset(out,0,20);return 0;}
static int get_http_status_code(char*){return 101;}
static char*get_http_header(char*,const char*){return nullptr;}
#define SDK_WS_CONTEXT(t) static_cast<transport_ws_t*>(esp_transport_get_context_data(t))
'''
RECV_SUPPORT=r'''
#define CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK 1
constexpr int ESP_FAIL=-1,WS_TRANSPORT_OPCODES_NONE=256,WS_TRANSPORT_OPCODES_TEXT=1,WS_TRANSPORT_OPCODES_PING=9,WS_TRANSPORT_OPCODES_PONG=10,WS_TRANSPORT_OPCODES_CLOSE=8,WS_TRANSPORT_OPCODES_FIN=128,WEBSOCKET_STATE_CLOSING=7,WEBSOCKET_EVENT_DATA=3;
constexpr int pdPASS=1,WEBSOCKET_TX_LOCK_TIMEOUT_MS=200;
constexpr uint32_t portMAX_DELAY=0xffffffffU;
constexpr int portTICK_PERIOD_MS=10,WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT=1;
using TickType_t=uint32_t;using ws_transport_opcodes_t=int;
static bool ping_case=false,tx_owned=false;
static int pong_writes=0;
static int send_lock_ms=0;
static int xSemaphoreTakeRecursive(int lock,TickType_t ticks){assert(lock==1);if(send_case){advance(send_lock_ms);tx_owned=true;return pdPASS;}assert(ticks==200);if(tx_owned){advance(ticks*10);return 0;}tx_owned=true;return pdPASS;}
static void xSemaphoreGiveRecursive(int lock){assert(lock==1&&tx_owned);tx_owned=false;}
using esp_err_t=int;
struct RecvConfig {int network_timeout_ms=10000;};
struct RecvClient {T*transport;char rx_buffer[4096]{};uint8_t tx_buffer[4096]{};int buffer_size=4096,payload_offset=0,payload_len=0,last_opcode=0,state=0,tx_lock=1;bool last_fin=false,wait_for_pong_resp=false;RecvConfig*config;};
using esp_websocket_client_handle_t=RecvClient*;
static void esp_websocket_free_buf(RecvClient*,bool){}
static int esp_websocket_new_buf(RecvClient*,bool){return ESP_OK;}
static void esp_websocket_client_error(RecvClient*,const char*,...){}
static bool esp_websocket_client_is_connected(RecvClient*){return true;}
static void esp_websocket_client_abort_connection(RecvClient*,int){}
static void esp_websocket_client_dispatch_event(RecvClient*,int,char*,int){}
static int esp_transport_ws_get_read_payload_len(T*){return ping_case?1:100000;}
static bool esp_transport_ws_get_fin_flag(T*){return true;}
static int esp_transport_ws_get_read_opcode(T*){return ping_case?WS_TRANSPORT_OPCODES_PING:WS_TRANSPORT_OPCODES_TEXT;}
static int esp_transport_ws_send_raw(T*t,int,const char*p,int n,int ms){
 assert(tx_owned);
 if(send_case){
  // SDK framing is a declared I/O substitute here. Both header and payload
  // enter the actual production transport and its socket cancellation owner.
  if(esp_transport_write(t,"h",1,ms)!=1)return -1;
  return esp_transport_write(t,p,n,ms);
 }
 pong_writes++;return n;
}
'''
MAIN=r'''
int main(){
 overlap();
 // Existing socket cancellation scenarios continue to pass with the guard.
 run("https://example.test",2); // Other legacy scenarios stay in test_http_deadline.py.
 for(int scenario=0;scenario<3;scenario++){
  now=1;nested=false;delay=9000;reply=1;shutdown_failures=0;
  esp_http_client_config_t cfg{};cfg.url="https://cloud";cfg.crt_bundle_attach=bundle;
  {MemoHttpTransport guard(cfg,0,true);interrupt_target=&guard;
   interrupt_at=scenario==0?15000001:scenario==1?30000001:2000001;
   int64_t deadline=interrupt_at;
   if(scenario==0){
    transport_ws_t ws{cfg.transport};ws.headers="Authorization: Bearer synthetic-secret\r\n";T outer{};outer.ctx=&ws;
    assert(ws_connect(&outer,"example.test",443,10000)<0);
   }else{
    assert(esp_transport_connect(cfg.transport,"example.test",443,10000)==0);
    RecvConfig config;RecvClient client{};client.transport=cfg.transport;client.config=&config;
    assert(esp_websocket_client_recv(&client)==ESP_FAIL); // Real SDK do/while ignores run.
   }
   assert(now<=deadline+100000);assert(!interrupt_target);
  }
  assert(!live);
 }
 // Locked SDK send repeats timeout after TX lock and for each 4096-B frame.
 // The production SEND lane must cover all of them as one operation.
 for(int scenario=0;scenario<4;scenario++){
  now=1;delay=scenario==0?1:scenario==1?1800:scenario==2?600:100;
  reply=1;nested=false;send_case=true;tx_owned=false;send_lock_ms=scenario==1?1800:0;
  esp_http_client_config_t cfg{};cfg.url="https://cloud";cfg.crt_bundle_attach=bundle;
  {MemoHttpTransport guard(cfg,0,true);assert(!esp_transport_connect(cfg.transport,"example.test",443,10000));
   RecvClient client{};client.transport=cfg.transport;uint8_t payload[4400]{};
   guard.send_until(now+2000000);
   if(scenario==3){interrupt_target=&guard;interrupt_at=now+200000;}
   int sent=esp_websocket_client_send_with_exact_opcode(&client,1|128,payload,sizeof(payload),200);
   guard.send_until(0);
   assert(sent==(scenario==0?4400:-1));assert(!tx_owned);assert(now<=2100001);
   if(scenario)assert(ended(last_inner));
   send_case=false;
  }
  assert(!live);run_worker();assert(wait_ticks==0xffffffffU);
 }
 // Actual SDK PONG path must share the TX lock with application sends. Model
 // an application holding that lock: SDK's nominal-ms field is really ticks,
 // so 200 ticks at the pinned 100 Hz is 2 s, not the default 20 s.
 for(bool blocked: {false,true}){
  now=1;delay=0;reply=1;nested=false;ping_case=true;tx_owned=blocked;pong_writes=0;
  esp_http_client_config_t cfg{};cfg.url="https://cloud";cfg.crt_bundle_attach=bundle;
  {MemoHttpTransport guard(cfg,0,true);assert(!esp_transport_connect(cfg.transport,"example.test",443,10000));
   RecvConfig config;RecvClient client{};client.transport=cfg.transport;client.config=&config;
   assert(esp_websocket_client_recv(&client)==(blocked?ESP_FAIL:ESP_OK));
   assert(pong_writes==(blocked?0:1));assert(tx_owned==blocked);assert(now<=2000001);
  }
  assert(!live);ping_case=false;tx_owned=false;
 }
 // Execute the actual SDK credential-leaking write-failure line. First prove
 // the injection reaches it at INFO, then assert tag suppression removes it.
 for(int level: {3,0}){
  now=1;delay=0;reply=0;nested=false;logs.clear();log_level=level;
  esp_http_client_config_t cfg{};cfg.url="https://cloud";cfg.crt_bundle_attach=bundle;
  {MemoHttpTransport guard(cfg,0,true);transport_ws_t ws{cfg.transport};ws.headers="Authorization: Bearer synthetic-secret\r\n";T outer{};outer.ctx=&ws;
   assert(ws_connect(&outer,"example.test",443,10000)<0);
  }
  bool exposed=false;for(const auto&line:logs)if(line.find("synthetic-secret")!=std::string::npos)exposed=true;
  assert(exposed==(level==3));assert(!live);
 }
}
'''

class SDKTests(unittest.TestCase):
    def test_actual_sdk_slow_drip_cancel_and_secret_log_path(self):
        sdk=ROOT/'.tools/esp-idf'
        ws=(sdk/'components/tcp_transport/transport_ws.c').read_text()
        client=locked_websocket_source(
            ROOT/'managed_components/espressif__esp_websocket_client/esp_websocket_client.c',
            ROOT/'.tools/component-cache/host-source/esp-websocket-client-1.5.0.c')
        connect=extract(ws,'static int ws_connect(')
        # One C-to-C++ void* cast only; the entire function body is unchanged.
        connect=connect.replace('esp_transport_get_context_data(t)','SDK_WS_CONTEXT(t)')
        recv=extract(client,'static esp_err_t esp_websocket_client_recv(')
        send=extract(client,'static int esp_websocket_client_send_with_exact_opcode(')
        prefix=HARNESS.split('int main(){',1)[0]
        prefix=prefix.replace('static void advance(int ms){now+=(int64_t)ms*1000;run_worker();}',
            '''static bool ended(T*);
static MemoHttpTransport*interrupt_target=nullptr;static int64_t interrupt_at=0;
static void advance(int ms){while(ms>0){int step=std::min(ms,100);now+=(int64_t)step*1000;ms-=step;
 if(interrupt_target&&now>=interrupt_at){interrupt_target->interrupt();interrupt_target=nullptr;run_worker();return;}run_worker();if(send_case&&last_inner&&ended(last_inner))return;}}''')
        prefix=prefix.replace('return delay>=ms?0:reply;',
            'if(send_case&&ended(t))return -1;return delay>=ms?0:send_case?n:reply;')
        with tempfile.TemporaryDirectory(prefix='cloud-sdk-',dir=ROOT/'private/host-tests') as folder:
            p=Path(folder);(p/'shim.h').write_text(HEADER)
            (p/'freertos').mkdir();(p/'lwip').mkdir()
            (p/'freertos/FreeRTOS.h').write_text('#pragma once\n#include <mutex>\n#include <cstdint>\nusing TickType_t=uint32_t;using StackType_t=uint8_t;using StaticTask_t=void*;using TaskHandle_t=void*;\n#define pdMS_TO_TICKS(x) (x)\nusing portMUX_TYPE=std::mutex;\n#define portMUX_INITIALIZER_UNLOCKED {}\n#define portENTER_CRITICAL(p) (p)->lock()\n#define portEXIT_CRITICAL(p) (p)->unlock()\n#define portMAX_DELAY 0xffffffffU\n#define pdTRUE 1\n')
            (p/'freertos/semphr.h').write_text('#pragma once\n#include "FreeRTOS.h"\nusing StaticSemaphore_t=std::mutex;using SemaphoreHandle_t=std::mutex*;\ninline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t*p){return p;}\ninline int xSemaphoreTake(SemaphoreHandle_t p,int){p->lock();return 1;}\ninline void xSemaphoreGive(SemaphoreHandle_t p){p->unlock();}\n')
            (p/'freertos/task.h').write_text('#pragma once\n#include "FreeRTOS.h"\nvoid* xTaskCreateStatic(void(*)(void*),const char*,unsigned,void*,unsigned,uint8_t*,void*);\nvoid xTaskNotifyGive(void*);unsigned ulTaskNotifyTake(int,unsigned);void vTaskDelay(unsigned);unsigned uxTaskGetStackHighWaterMark(void*);\n')
            (p/'lwip/opt.h').write_text('#define LWIP_NETCONN_SEM_PER_THREAD 1\nvoid*test_sem_get();\n#define LWIP_NETCONN_THREAD_SEM_PER_THREAD 1\n#define LWIP_NETCONN_THREAD_SEM_GET() test_sem_get()\n');(p/'lwip/sys.h').write_text('')
            for name in ('esp_http_client.h','esp_transport.h','esp_transport_tcp.h','esp_transport_ssl.h','esp_timer.h','esp_tls.h'):
                (p/name).write_text('#include "shim.h"\n')
            (p/'test.cpp').write_text('static bool send_case=false;\n'+prefix+SUPPORT+connect+RECV_SUPPORT+recv+send+MAIN)
            common=['c++','-std=c++17','-Wall','-Wextra','-Werror','-g','-O1','-fsanitize=address,undefined','-I'+str(p),'-I'+str(ROOT/'main')]
            subprocess.run([*common,str(ROOT/'main/memo_http_transport.cpp'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)

if __name__=='__main__':
    unittest.main(verbosity=2)
