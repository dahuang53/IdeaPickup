#include "memo_http_transport.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ssl.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/opt.h"
#include "lwip/sys.h"
#include <atomic>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>

namespace {
// One fixed, dedicated cancellation owner. Never block ESP timer/event tasks.
// All fd binding/unbinding uses this lock, so shutdown cannot hit a reused fd.
struct SocketDeadline {int fd=-1;int64_t deadline=0,retry_at=0;bool used=false,expired=false;};
SocketDeadline sockets[2];
StaticSemaphore_t deadline_mutex_storage;
SemaphoreHandle_t deadline_mutex;
portMUX_TYPE init_lock=portMUX_INITIALIZER_UNLOCKED;
StaticTask_t deadline_task_storage;
// Measured peak: 1052 B including real socket cancellation; retain >1 KiB margin.
StackType_t deadline_stack[2560/sizeof(StackType_t)];
std::atomic<TaskHandle_t> deadline_task{nullptr};
std::atomic<bool> deadline_ready{false};
void deadline_worker(void *) {
 // Allocate lwIP's per-thread semaphore before admitting any request. A
 // failed allocation leaves HTTP unavailable; it cannot disable cancellation
 // only after a request is already blocked inside TLS.
#if LWIP_NETCONN_SEM_PER_THREAD
 while(!LWIP_NETCONN_THREAD_SEM_GET())ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
#endif
 deadline_ready.store(true);
 for(;;){
  xSemaphoreTake(deadline_mutex,portMAX_DELAY);
  const int64_t now=esp_timer_get_time();int64_t next=0;
  for(auto &s:sockets)if(s.used && s.fd>=0 && s.deadline && !s.expired){
   int64_t due=s.retry_at?s.retry_at:s.deadline;
   if(now>=due){
    // Only successful shutdown (or an already disconnected socket) is final.
    // Resource/transient failures retain ownership and retry after 100 ms.
    if(shutdown(s.fd,SHUT_RDWR)==0 || errno==ENOTCONN)s.expired=true;
    else s.retry_at=esp_timer_get_time()+100000;
   }
   if(!s.expired){due=s.retry_at?s.retry_at:s.deadline;if(!next||due<next)next=due;}
  }
  xSemaphoreGive(deadline_mutex);
  int64_t remaining=next?next-esp_timer_get_time():0;
  TickType_t wait=next?pdMS_TO_TICKS((remaining>0?remaining:0)/1000+1):portMAX_DELAY;
  ulTaskNotifyTake(pdTRUE,wait?wait:1);
 }
}
int reserve_socket() {
 portENTER_CRITICAL(&init_lock);
 if(!deadline_mutex)deadline_mutex=xSemaphoreCreateMutexStatic(&deadline_mutex_storage);
 portEXIT_CRITICAL(&init_lock);
 if(!deadline_mutex)return -1;
 xSemaphoreTake(deadline_mutex,portMAX_DELAY);
 if(!deadline_task)deadline_task=xTaskCreateStatic(deadline_worker,"memo_http_stop",sizeof(deadline_stack),nullptr,3,deadline_stack,&deadline_task_storage);
 TaskHandle_t task=deadline_task;
 xSemaphoreGive(deadline_mutex);
 if(!task)return -1;
 xTaskNotifyGive(task);
 const int64_t until=esp_timer_get_time()+1000000;
 while(!deadline_ready.load() && esp_timer_get_time()<until)vTaskDelay(1);
 if(!deadline_ready.load())return -1;
 xSemaphoreTake(deadline_mutex,portMAX_DELAY);
 int slot=-1;
 for(int i=0;i<2;i++)if(!sockets[i].used){sockets[i]={};sockets[i].used=true;slot=i;break;}
 xSemaphoreGive(deadline_mutex);return slot;
}
void unbind_socket(int slot,bool release=false) {
 if(slot<0)return;
 xSemaphoreTake(deadline_mutex,portMAX_DELAY);
 sockets[slot].fd=-1;sockets[slot].deadline=0;
 if(release)sockets[slot].used=false;
 xSemaphoreGive(deadline_mutex);xTaskNotifyGive(deadline_task);
}
bool arm_socket(int slot,int64_t deadline) {
 xSemaphoreTake(deadline_mutex,portMAX_DELAY);
 auto &s=sockets[slot];bool ok=s.fd>=0 && !s.expired;
 if(ok){s.deadline=deadline;s.retry_at=0;}
 xSemaphoreGive(deadline_mutex);xTaskNotifyGive(deadline_task);return ok;
}
void disarm_socket(int slot) {
 xSemaphoreTake(deadline_mutex,portMAX_DELAY);sockets[slot].deadline=0;
 xSemaphoreGive(deadline_mutex);xTaskNotifyGive(deadline_task);
}
}

uint32_t MemoHttpTransport::stack_free() {return deadline_task?uxTaskGetStackHighWaterMark(deadline_task)*sizeof(StackType_t):0;}

MemoHttpTransport::MemoHttpTransport(esp_http_client_config_t &cfg) {
 const bool tls=cfg.url && !strncmp(cfg.url,"https://",8);
 if(!tls && (!cfg.url || strncmp(cfg.url,"http://",7)))return;
 if(tls && !cfg.crt_bundle_attach)return; // Never weaken certificate checking.
 inner_=tls?esp_transport_ssl_init():esp_transport_tcp_init();
 outer_=esp_transport_init();
 slot_=reserve_socket();
 if(!ready())return;
 if(tls)esp_transport_ssl_crt_bundle_attach(inner_,cfg.crt_bundle_attach);
 keepalive_={cfg.keep_alive_enable,cfg.keep_alive_idle,cfg.keep_alive_interval,cfg.keep_alive_count};
 if(cfg.keep_alive_enable){
  if(tls)esp_transport_ssl_set_keep_alive(inner_,&keepalive_);
  else esp_transport_tcp_set_keep_alive(inner_,&keepalive_);
 }
 esp_transport_set_context_data(outer_,this);
 esp_transport_set_default_port(outer_,tls?443:80);
 esp_transport_set_func(outer_,connect,read,write,close,poll_read,poll_write,nullptr);
 cfg.transport=outer_; // Official IDF custom transport API, no SDK patch.
}
MemoHttpTransport::~MemoHttpTransport() {
 unbind_socket(slot_,true);
 // HTTP client must already be cleaned up. IDF does not own cfg.transport.
 if(inner_)esp_transport_destroy(inner_);
 if(outer_)esp_transport_destroy(outer_);
}
MemoHttpTransport &MemoHttpTransport::self(esp_transport_handle_t t) {
 return *static_cast<MemoHttpTransport *>(esp_transport_get_context_data(t));
}
int MemoHttpTransport::budget(int64_t &deadline,int window_ms,int timeout_ms) {
 const int64_t now=esp_timer_get_time();
 if(!deadline)deadline=now+(int64_t)window_ms*1000;
 const int64_t remaining=(deadline-now)/1000;
 if(remaining<=0){errno=ETIMEDOUT;return 0;}
 return timeout_ms>0 && timeout_ms<remaining?timeout_ms:(int)remaining;
}
void MemoHttpTransport::write_window(){write_deadline_=0;}
int MemoHttpTransport::connect(esp_transport_handle_t t,const char *host,int port,int ms) {
 auto &s=self(t);
 int result=esp_transport_connect(s.inner_,host,port,ms);
 if(result)return result;
 // The cancellation owner also interrupts nested TLS recv/send beyond IDF poll.
 int fd=esp_transport_get_socket(s.inner_);
 if(fd<0){esp_transport_close(s.inner_);return -1;}
 xSemaphoreTake(deadline_mutex,portMAX_DELAY);sockets[s.slot_].fd=fd;xSemaphoreGive(deadline_mutex);
 return 0;
}
int MemoHttpTransport::read(esp_transport_handle_t t,char *p,int n,int ms) {
 auto &s=self(t);int wait=budget(s.read_deadline_,30000,ms);
 if(!wait || !arm_socket(s.slot_,s.read_deadline_))return ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT;
 int result=esp_transport_read(s.inner_,p,n,wait);disarm_socket(s.slot_);
 if(esp_timer_get_time()>=s.read_deadline_){errno=ETIMEDOUT;return ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT;}
 return result;
}
int MemoHttpTransport::write(esp_transport_handle_t t,const char *p,int n,int ms) {
 auto &s=self(t);int wait=budget(s.write_deadline_,20000,ms);
 if(!wait || !arm_socket(s.slot_,s.write_deadline_))return -1;
 int result=esp_transport_write(s.inner_,p,n,wait);disarm_socket(s.slot_);
 if(esp_timer_get_time()>=s.write_deadline_){errno=ETIMEDOUT;return -1;}
 return result;
}
int MemoHttpTransport::close(esp_transport_handle_t t){auto &s=self(t);unbind_socket(s.slot_);return esp_transport_close(s.inner_);}
int MemoHttpTransport::poll_read(esp_transport_handle_t t,int ms) {
 auto &s=self(t);int wait=budget(s.read_deadline_,30000,ms);
 return wait?esp_transport_poll_read(s.inner_,wait):0;
}
int MemoHttpTransport::poll_write(esp_transport_handle_t t,int ms) {
 auto &s=self(t);int wait=budget(s.write_deadline_,20000,ms);
 return wait?esp_transport_poll_write(s.inner_,wait):0;
}
