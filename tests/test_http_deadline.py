"""Real transport decorator against deterministic slow/partial TCP/TLS shims.

The parser can call read indefinitely; this tests the lower boundary, including
partial-write loops and ownership. Socket pairs verify shutdown; TLS/RF are shims. No providers or device are used.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
HEADER=r'''
#pragma once
#include <cstdint>
#include <cstddef>
struct T;using esp_transport_handle_t=T*;
using connect_func=int(*)(T*,const char*,int,int);
using io_read_func=int(*)(T*,char*,int,int);
using io_func=int(*)(T*,const char*,int,int);
using trans_func=int(*)(T*);
using poll_func=int(*)(T*,int);
struct esp_transport_keep_alive_t {bool keep_alive_enable;int keep_alive_idle,keep_alive_interval,keep_alive_count;};
struct esp_http_client_config_t {const char*url;int(*crt_bundle_attach)(void*);bool keep_alive_enable;int keep_alive_idle,keep_alive_interval,keep_alive_count;T*transport;};
constexpr int ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT=0;
T*esp_transport_init();T*esp_transport_tcp_init();T*esp_transport_ssl_init();
int esp_transport_destroy(T*);void esp_transport_ssl_crt_bundle_attach(T*,int(*)(void*));
void esp_transport_ssl_set_keep_alive(T*,esp_transport_keep_alive_t*);void esp_transport_tcp_set_keep_alive(T*,esp_transport_keep_alive_t*);
void esp_transport_set_context_data(T*,void*);void*esp_transport_get_context_data(T*);void esp_transport_set_default_port(T*,int);
void esp_transport_set_func(T*,connect_func,io_read_func,io_func,trans_func,poll_func,poll_func,trans_func);
int esp_transport_connect(T*,const char*,int,int);int esp_transport_read(T*,char*,int,int);int esp_transport_write(T*,const char*,int,int);
int esp_transport_close(T*);int esp_transport_poll_read(T*,int);int esp_transport_poll_write(T*,int);
int64_t esp_timer_get_time();int esp_transport_get_socket(T*);
constexpr int ESP_OK=0;
struct esp_timer_create_args_t {void(*callback)(void*);void*arg;const char*name;};
using esp_timer_handle_t=void*;
int esp_timer_create(const esp_timer_create_args_t*,void**);int esp_timer_start_once(void*,uint64_t);int esp_timer_stop(void*);
'''
HARNESS=r"""
#include "memo_http_transport.h"
#include <cassert>
#include <algorithm>
#include <cerrno>
#include <sys/socket.h>
#include <unistd.h>
struct T {void*ctx=nullptr;connect_func con=nullptr;io_read_func rd=nullptr;io_func wr=nullptr;trans_func cl=nullptr;poll_func pr=nullptr,pw=nullptr;bool tls=false,bundle=false;int fd=-1,peer=-1;};
static int live=0,bundles=0,reads=0,writes=0,delay=1000,reply=1;
static int64_t now=1;static bool nested=false;static int shutdown_failures=0;
static bool sem_available=true;
void*test_sem_get(){return sem_available?(void*)1:nullptr;}
static void(*worker)(void*);static uint32_t wait_ticks=0;
struct Parked{};
void run_worker(){if(worker)try{worker(nullptr);}catch(Parked&){} }
void* xTaskCreateStatic(void(*f)(void*),const char*,unsigned,void*,unsigned,uint8_t*,void*){worker=f;return (void*)1;}
void xTaskNotifyGive(void*){}
unsigned ulTaskNotifyTake(int,unsigned ticks){wait_ticks=ticks;throw Parked{};}
unsigned uxTaskGetStackHighWaterMark(void*){return 4096;}
void vTaskDelay(unsigned){now+=1000;run_worker();}
extern "C" int guarded_shutdown(int fd,int how){if(shutdown_failures){shutdown_failures--;errno=ENOMEM;return -1;}return shutdown(fd,how);}
static void advance(int ms){now+=(int64_t)ms*1000;run_worker();}
T*esp_transport_init(){live++;return new T;}
T*esp_transport_tcp_init(){return esp_transport_init();}
T*esp_transport_ssl_init(){auto*t=esp_transport_init();t->tls=true;return t;}
int esp_transport_destroy(T*t){if(t->fd>=0)close(t->fd);if(t->peer>=0)close(t->peer);live--;delete t;return 0;}
void esp_transport_ssl_crt_bundle_attach(T*t,int(*)(void*)){assert(t->tls);t->bundle=true;bundles++;}
void esp_transport_ssl_set_keep_alive(T*,esp_transport_keep_alive_t*){}
void esp_transport_tcp_set_keep_alive(T*,esp_transport_keep_alive_t*){}
void esp_transport_set_context_data(T*t,void*p){t->ctx=p;}void*esp_transport_get_context_data(T*t){return t->ctx;}
void esp_transport_set_default_port(T*,int){}
void esp_transport_set_func(T*t,connect_func c,io_read_func r,io_func w,trans_func cl,poll_func pr,poll_func pw,trans_func){t->con=c;t->rd=r;t->wr=w;t->cl=cl;t->pr=pr;t->pw=pw;}
int64_t esp_timer_get_time(){return now;}
int esp_transport_get_socket(T*t){return t->fd;}
int esp_transport_connect(T*t,const char*h,int p,int ms){if(t->con)return t->con(t,h,p,ms);assert(!t->tls||t->bundle);int fds[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,fds));t->fd=fds[0];t->peer=fds[1];return 0;}
static bool ended(T*t){char c;int n=recv(t->fd,&c,1,MSG_DONTWAIT);return n==0;}
int esp_transport_read(T*t,char*p,int n,int ms){
 if(t->rd)return t->rd(t,p,n,ms);
 reads++;
 // Model a TLS inner loop that ignores fresh transport timeouts: only actual
 // socket shutdown by the production callback releases this inner call.
 if(nested){int attempts=0;while(!ended(t)){advance(1000);assert(++attempts<=33);}return -1;}
 advance(std::min(delay,ms));if(delay>=ms){errno=ETIMEDOUT;return 0;}if(n)*p='x';return reply;
}
int esp_transport_write(T*t,const char*p,int n,int ms){
 if(t->wr)return t->wr(t,p,n,ms);writes++;
 if(nested){int attempts=0;while(!ended(t)){advance(1000);assert(++attempts<=21);}return -1;}
 advance(std::min(delay,ms));return delay>=ms?0:reply;
}
int esp_transport_close(T*t){if(t->cl)return t->cl(t);if(t->fd>=0){close(t->fd);t->fd=-1;}return 0;}
int esp_transport_poll_read(T*t,int ms){return t->pr?t->pr(t,ms):1;}
int esp_transport_poll_write(T*t,int ms){return t->pw?t->pw(t,ms):1;}
static int bundle(void*){return 0;}
static void run(const char*url,int scenario){
 esp_http_client_config_t cfg{};cfg.url=url;cfg.crt_bundle_attach=bundle;
 now=1;reads=writes=0;nested=scenario>=3;
 {MemoHttpTransport t(cfg);assert(t.ready());assert(!esp_transport_connect(cfg.transport,"example.test",443,20000));
  const int64_t start=now;char c;delay=7000;reply=1;
  if(scenario==0){while(esp_transport_read(cfg.transport,&c,1,20000)>0){}assert(now-start<=30000000&&reads==5);}
  if(scenario==1){while(esp_transport_write(cfg.transport,"a",1,20000)>0){}assert(now-start<=20000000&&writes==3);}
  if(scenario==2){delay=10;reply=4096;for(int i=0;i<1000;i++){t.write_window();assert(esp_transport_write(cfg.transport,"x",4096,12000)==4096);}}
  if(scenario==3){shutdown_failures=1;assert(esp_transport_read(cfg.transport,&c,1,20000)==0&&now-start<=31000000&&reads==1&&shutdown_failures==0);}
  if(scenario==4){assert(esp_transport_write(cfg.transport,"x",1,20000)<0&&now-start<=20000000&&writes==1);}
  assert(!esp_transport_close(cfg.transport));
 }
 assert(!live);run_worker();assert(wait_ticks==0xffffffffU);advance(60000);assert(wait_ticks==0xffffffffU);
}
int main(){esp_http_client_config_t unavailable{};unavailable.url="http://transport.example.test";sem_available=false;{MemoHttpTransport t(unavailable);assert(!t.ready());}assert(!live);sem_available=true;for(int n=0;n<5;n++){run("http://transport.example.test",n);run("https://example.test",n);}assert(bundles==5);
 esp_http_client_config_t cfg{};cfg.url="https://example.test";{MemoHttpTransport t(cfg);assert(!t.ready());}assert(!live);
 cfg.crt_bundle_attach=bundle;{MemoHttpTransport a(cfg),b(cfg),c(cfg);assert(a.ready()&&b.ready()&&!c.ready());}assert(!live);
}
"""

class HttpDeadlineTests(unittest.TestCase):
 def test_tcp_tls_partial_responses_writes_certificate_and_cleanup(self):
  with tempfile.TemporaryDirectory(dir=ROOT/'private',prefix='http-deadline-') as d:
   p=Path(d);(p/'shim.h').write_text(HEADER)
   (p/'freertos').mkdir()
   (p/'freertos/FreeRTOS.h').write_text('#pragma once\n#include <mutex>\n#include <cstdint>\nusing TickType_t=uint32_t;using StackType_t=uint8_t;using StaticTask_t=void*;using TaskHandle_t=void*;\n#define pdMS_TO_TICKS(x) (x)\nusing portMUX_TYPE=std::mutex;\n#define portMUX_INITIALIZER_UNLOCKED {}\n#define portENTER_CRITICAL(p) (p)->lock()\n#define portEXIT_CRITICAL(p) (p)->unlock()\n#define portMAX_DELAY 0xffffffffU\n#define pdTRUE 1\n')
   (p/'freertos/semphr.h').write_text('#pragma once\n#include "FreeRTOS.h"\nusing StaticSemaphore_t=std::mutex;using SemaphoreHandle_t=std::mutex*;\ninline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t*p){return p;}\ninline int xSemaphoreTake(SemaphoreHandle_t p,int){p->lock();return 1;}\ninline void xSemaphoreGive(SemaphoreHandle_t p){p->unlock();}\n')
   (p/'freertos/task.h').write_text('#pragma once\n#include "FreeRTOS.h"\nvoid* xTaskCreateStatic(void(*)(void*),const char*,unsigned,void*,unsigned,uint8_t*,void*);\nvoid xTaskNotifyGive(void*);unsigned ulTaskNotifyTake(int,unsigned);void vTaskDelay(unsigned);unsigned uxTaskGetStackHighWaterMark(void*);\n')
   (p/'lwip').mkdir();(p/'lwip/opt.h').write_text('#define LWIP_NETCONN_SEM_PER_THREAD 1\nvoid*test_sem_get();\n#define LWIP_NETCONN_THREAD_SEM_GET() test_sem_get()\n');(p/'lwip/sys.h').write_text('')
   (p/'cancel.h').write_text('int guarded_shutdown(int,int);\n')
   for name in ('esp_http_client.h','esp_transport.h','esp_transport_tcp.h','esp_transport_ssl.h','esp_timer.h'):
    (p/name).write_text('#include "shim.h"\n')
   (p/'test.cpp').write_text(HARNESS)
   common=['c++','-std=c++17','-Wall','-Wextra','-Werror','-g','-O1','-fsanitize=address,undefined','-I',str(p),'-I',str(ROOT/'main')]
   subprocess.run([*common,'-Dshutdown=guarded_shutdown','-c',str(ROOT/'main/memo_http_transport.cpp'),'-o',str(p/'transport.o')],check=True)
   subprocess.run([*common,str(p/'transport.o'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True)

if __name__=='__main__':unittest.main(verbosity=2)
