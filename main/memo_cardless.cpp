#include "memo_cardless.h"
#include "memo_cardless_record.h"
#include "memo_record_ids.h"
#include "memo_cloud_config.h"
#ifdef MEMO_CARDLESS_TEST
#include "cardless_platform_fixture.h"
#else
#include "memo_cloud_esp.h"
#endif
#include "memo_transcript.h"
#include "network_sync.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>
#include <ctime>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#ifdef MEMO_CARDLESS_TEST
extern void cardless_test_stop_transition();
#endif
namespace {
constexpr size_t MAX_RECORDS=1024,PCM_BYTES=65536,TEXT_BYTES=262144;
constexpr uint32_t LEGACY=0x80000000U;
struct Index {uint32_t id,duration,bytes,state;};
static_assert(sizeof(Index)==16,"bounded history index");
struct Detail {memo_cardless_record_t value{};uint32_t age=0;};
Index *records=nullptr;size_t count=0;Detail details[4];uint32_t age=0,counter=0;
SemaphoreHandle_t meta_mutex=nullptr,pcm_mutex=nullptr,begin_mutex=nullptr;
nvs_handle_t metadata=0,legacy=0;bool legacy_open=false;
std::atomic<bool> initialized{false},metadata_fault{false},configured{false},capturing{false},stopped{false},abort_io{false};
std::atomic<uint32_t> active{0},selected{0},revision{0},samples{0};
std::atomic<int64_t> stopped_at{0};
char error[80]{};std::atomic<bool> cancelled{false},prepared{false};
StreamBufferHandle_t pcm_stream=nullptr;StaticStreamBuffer_t pcm_control;
uint8_t *pcm_storage=nullptr;char *text=nullptr;size_t text_bytes=0;
MemoCloudEsp *platform=nullptr;memo_cloud::Realtime *realtime=nullptr;
memo_cloud_config_t config{};
bool ws_started=false,finalized=false;int64_t delivery_due=0;
char delivery_prefix[160]{};size_t prefix_bytes=0;
memo_cardless_record_t current{};
class Lock {SemaphoreHandle_t m;public:explicit Lock(SemaphoreHandle_t v):m(v){xSemaphoreTake(m,portMAX_DELAY);}~Lock(){xSemaphoreGive(m);}};
void copy(char *to,size_t cap,const char *s){if(to&&cap)snprintf(to,cap,"%s",s?s:"");}
int64_t now(){return esp_timer_get_time()/1000;}
void notice(const char *s){Lock lock(meta_mutex);copy(error,sizeof(error),s);revision.fetch_add(1);}
void fault(){metadata_fault=true;notice("记录状态写入失败");abort_io=true;stopped=true;}
bool terminal(memo_job_state_t s){return s==MEMO_JOB_SENT||s==MEMO_JOB_FAILED||s==MEMO_JOB_REJECTED||s==MEMO_JOB_CANCELLED||s==MEMO_JOB_UNCERTAIN;}
size_t find(uint32_t id){for(size_t i=0;i<count;i++)if(records[i].id==id)return i;return SIZE_MAX;}
void key_for(uint32_t id,char (&key)[16]){snprintf(key,sizeof(key),"r%08lx",(unsigned long)id);}
void remember(const memo_cardless_record_t &r){
 size_t i=find(r.id);if(i!=SIZE_MAX){records[i]={r.id,r.duration_ms,r.encoded_bytes,uint32_t(r.state)|(r.direct?0:LEGACY)};}
 for(auto &d:details)if(d.value.id==r.id){d.value=r;d.age=++age;}
 revision.fetch_add(1);
}
bool read_record(uint32_t id,bool old,memo_cardless_record_t &r){
 uint8_t bytes[MEMO_CARDLESS_RECORD_SIZE];size_t n=sizeof(bytes);char key[16];key_for(id,key);
 if(nvs_get_blob(old?legacy:metadata,key,bytes,&n)!=ESP_OK)return false;
 bool ok=old?memo_cardless_record_decode(bytes,n,&r):memo_cardless_direct_decode(bytes,n,&r);
 if(!ok||r.id!=id)return false;
 if(old){r.can_retry=false;copy(r.error,sizeof(r.error),r.state!=MEMO_JOB_SENT?"旧服务待核对":r.interrupted?"录音曾中断":"");}return true;
}
bool write_record(const memo_cardless_record_t &r){
 uint8_t bytes[MEMO_CARDLESS_DIRECT_SIZE],actual[MEMO_CARDLESS_DIRECT_SIZE];if(!memo_cardless_direct_encode(&r,bytes))return false;
 char key[16];key_for(r.id,key);size_t n=sizeof(actual);
 // No unchanged Flash write. A new read handle confirms the committed value.
 if(nvs_get_blob(metadata,key,actual,&n)==ESP_OK&&n==sizeof(bytes)&&!memcmp(actual,bytes,n))return true;
 if(nvs_set_blob(metadata,key,bytes,sizeof(bytes))!=ESP_OK||nvs_commit(metadata)!=ESP_OK)return false;
 nvs_handle_t reader;if(nvs_open_from_partition("memo_meta","direct1",NVS_READONLY,&reader)!=ESP_OK)return false;
 n=sizeof(actual);esp_err_t rc=nvs_get_blob(reader,key,actual,&n);nvs_close(reader);
 return rc==ESP_OK&&n==sizeof(bytes)&&!memcmp(actual,bytes,n);
}
bool marker(uint32_t id){
 // Tiny explicit active fact; record always commits before this key changes.
 uint8_t bytes[12]={'R','1','C','A'};for(unsigned i=0;i<4;i++){bytes[4+i]=id>>(8*i);bytes[8+i]=~bytes[4+i];}
 if(nvs_set_blob(metadata,"active",bytes,sizeof(bytes))!=ESP_OK||nvs_commit(metadata)!=ESP_OK)return false;
 nvs_handle_t h;if(nvs_open_from_partition("memo_meta","direct1",NVS_READONLY,&h)!=ESP_OK)return false;
 uint8_t actual[12];size_t n=sizeof(actual);int rc=nvs_get_blob(h,"active",actual,&n);nvs_close(h);return rc==ESP_OK&&n==sizeof(bytes)&&!memcmp(bytes,actual,n);
}
bool save_current(){
 bool ok;{Lock lock(meta_mutex);ok=write_record(current);if(ok)remember(current);}
 if(!ok)fault();
 else network_sync_task_result(current.id,current.state,current.error);
 return ok;
}
bool online(){memo_network_status_t n{};network_sync_get_status(&n);return n.initialized&&n.connected&&!n.provisioning;}
void release(){
 // Only the network owner destroys connections. Producer takes pcm_mutex
 // and rechecks capture before touching queue memory.
 prepared=false;capturing=false;abort_io=true;delete realtime;realtime=nullptr;delete platform;platform=nullptr;
 {Lock lock(pcm_mutex);if(pcm_stream)vStreamBufferDelete(pcm_stream);pcm_stream=nullptr;free(pcm_storage);pcm_storage=nullptr;}
 free(text);text=nullptr;text_bytes=0;memo_cloud_config_clear(&config);
 ws_started=finalized=false;delivery_due=0;active=0;revision.fetch_add(1);
}
void finish_record(memo_job_state_t state,const char *reason){
 bool was_capturing=capturing.exchange(false);if(was_capturing)stopped=true;
 {Lock lock(pcm_mutex);} // Wait out the last producer before sampling totals.
 char final_reason[80];copy(final_reason,sizeof(final_reason),reason);
 if(state==MEMO_JOB_FAILED&&stopped){Lock lock(meta_mutex);if(error[0])copy(final_reason,sizeof(final_reason),error);}
 notice(final_reason);
 current.state=state;current.duration_ms=samples.load()/16;current.encoded_bytes=samples.load()*2;current.interrupted=stopped.load();copy(current.error,sizeof(current.error),final_reason);
 bool saved=save_current();if(saved&&!marker(0))fault();release();
}
const char *reason(memo_cloud::Code c){using memo_cloud::Code;switch(c){case Code::Credentials:return "凭据无效";case Code::Limited:return "服务限流";case Code::TooLarge:return "内容超出上限";case Code::Memory:return "录音内存不足";case Code::Empty:return "未识别到语音";case Code::Uncertain:return "投递待核对";case Code::Timeout:return "处理等待超时";default:return "处理已中断";}}
void fail_asr(const memo_cloud::Result &result,const char *stage){
 const auto code=result.code;
 printf("CARDLESS_ASR_ERROR stage=%s code=%d detail=%s\n",stage,int(code),result.provider_code);
 finish_record(cancelled?MEMO_JOB_CANCELLED:code==memo_cloud::Code::Empty?MEMO_JOB_REJECTED:MEMO_JOB_FAILED,cancelled?"":reason(code));}
bool append_text(void *,const char *s,size_t n){if(n>TEXT_BYTES-text_bytes)return false;memcpy(text+text_bytes,s,n);text_bytes+=n;return true;}
struct Clean {size_t n=0;};
bool clean_text(void *p,const char *s,size_t n){auto &v=*static_cast<Clean *>(p);memmove(text+v.n,s,n);v.n+=n;return true;}
int source_text(void *,uint64_t offset,void *out,size_t n){if(offset>prefix_bytes+text_bytes||n>prefix_bytes+text_bytes-offset)return -1;
 size_t copied=0;if(offset<prefix_bytes){copied=std::min<uint64_t>(n,prefix_bytes-offset);memcpy(out,delivery_prefix+offset,copied);offset+=copied;}
 if(copied<n)memcpy(static_cast<char *>(out)+copied,text+offset-prefix_bytes,n-copied);
 return int(n);}
bool load_index(const char *space,bool old){
 nvs_iterator_t it=nullptr;esp_err_t rc=nvs_entry_find("memo_meta",space,NVS_TYPE_BLOB,&it);
 while(rc==ESP_OK){nvs_entry_info_t info{};if(nvs_entry_info(it,&info)!=ESP_OK){nvs_release_iterator(it);return false;}
  if(info.key[0]=='r'){
   unsigned long value=0;int end=0;memo_cardless_record_t r{};
   if(sscanf(info.key,"r%8lx%n",&value,&end)!=1||end!=9||info.key[end]||value>UINT32_MAX||!read_record(uint32_t(value),old,r)||find(r.id)!=SIZE_MAX||count==MAX_RECORDS){nvs_release_iterator(it);return false;}
   records[count++]={r.id,r.duration_ms,r.encoded_bytes,uint32_t(r.state)|(old?LEGACY:0)};counter=std::max(counter,MEMO_CARDLESS_DISPLAY_ID(r.id));
  }
  rc=nvs_entry_next(&it);
 }
 nvs_release_iterator(it);return rc==ESP_ERR_NVS_NOT_FOUND;
}
bool boot_cleanup(){
 uint8_t active_bytes[12];size_t n=sizeof(active_bytes);int rc=nvs_get_blob(metadata,"active",active_bytes,&n);uint32_t marked=0;
 if(rc==ESP_OK){if(n!=12||memcmp(active_bytes,"R1CA",4))return false;for(unsigned i=0;i<4;i++){if(uint8_t(~active_bytes[4+i])!=active_bytes[8+i])return false;marked|=uint32_t(active_bytes[4+i])<<(8*i);}if(marked&&find(marked)==SIZE_MAX)return false;}
 else if(rc!=ESP_ERR_NVS_NOT_FOUND)return false;
 // A crash between record and active-key writes can leave an orphan. Process
 // only new direct records; never mutate old service records/active markers.
 for(size_t i=0;i<count;i++)if(!(records[i].state&LEGACY)){
  memo_cardless_record_t r{};if(!read_record(records[i].id,false,r))return false;
  if(!terminal(r.state)){
   if(r.state==MEMO_JOB_DELIVERING||(r.state==MEMO_JOB_CANCELLING&&r.may_have_remote_audio)){r.state=MEMO_JOB_UNCERTAIN;copy(r.error,sizeof(r.error),"投递待核对");}
   else if(r.state==MEMO_JOB_CANCELLING){r.state=MEMO_JOB_CANCELLED;r.error[0]=0;}
   else {r.state=MEMO_JOB_FAILED;r.interrupted=true;copy(r.error,sizeof(r.error),"录音已中断");}
   if(!write_record(r))return false;
   remember(r);
  }
 }
 return !marked||marker(0);
}
} // namespace
extern "C" esp_err_t memo_cardless_init(){
 if(initialized)return ESP_OK;
 if(meta_mutex)return ESP_ERR_INVALID_STATE;
 meta_mutex=xSemaphoreCreateMutex();pcm_mutex=xSemaphoreCreateMutex();begin_mutex=xSemaphoreCreateMutex();if(!meta_mutex||!pcm_mutex||!begin_mutex)return ESP_ERR_NO_MEM;
 if(nvs_flash_init_partition("memo_meta")!=ESP_OK)return ESP_FAIL;
 if(nvs_open_from_partition("memo_meta","direct1",NVS_READWRITE,&metadata)!=ESP_OK)return ESP_FAIL;
 int rc=nvs_open_from_partition("memo_meta","records1",NVS_READONLY,&legacy);legacy_open=rc==ESP_OK;if(rc!=ESP_OK&&rc!=ESP_ERR_NVS_NOT_FOUND)return ESP_FAIL;
 records=static_cast<Index *>(heap_caps_calloc(MAX_RECORDS,sizeof(Index),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));if(!records)return ESP_ERR_NO_MEM;
 if((legacy_open&&!load_index("records1",true))||!load_index("direct1",false)||!boot_cleanup()){metadata_fault=true;copy(error,sizeof(error),"记录存储校验失败");return ESP_FAIL;}
 std::sort(records,records+count,[](const Index&a,const Index&b){return a.id<b.id;});initialized=true;revision.fetch_add(1);return ESP_OK;
}
extern "C" void memo_cardless_configure(bool ready,bool changed){
 configured=ready;
 if(changed&&active){stopped=true;abort_io=true;}network_sync_wake();
}
extern "C" bool memo_cardless_ready(){return initialized&&!metadata_fault&&configured&&!active&&online();}
extern "C" esp_err_t memo_cardless_observe_id(uint32_t id){if(id&MEMO_CARDLESS_ID_FLAG)return ESP_ERR_INVALID_ARG;if(!initialized)return ESP_ERR_INVALID_STATE;Lock lock(meta_mutex);counter=std::max(counter,id);return ESP_OK;}
extern "C" esp_err_t memo_cardless_begin(uint32_t floor,uint32_t *id){
 if(!id||!initialized)return ESP_ERR_INVALID_STATE;
 *id=0;
 if(xSemaphoreTake(begin_mutex,0)!=pdTRUE)return ESP_ERR_INVALID_STATE;
 struct Exit{~Exit(){xSemaphoreGive(begin_mutex);}}exit;
 if(!memo_cardless_ready()){notice("无卡录音需要联网");return ESP_ERR_INVALID_STATE;}
 memo_cloud_config_t c{};if(!memo_cloud_config_read(&c)||!c.enabled||!memo_cloud_config_ready(&c)){memo_cloud_config_clear(&c);notice("请先配置云服务");return ESP_ERR_INVALID_STATE;}
 nvs_stats_t stats{};{Lock lock(meta_mutex);if(count>=MAX_RECORDS||nvs_get_stats("memo_meta",&stats)!=ESP_OK||stats.free_entries<158){memo_cloud_config_clear(&c);copy(error,sizeof(error),"记录存储已满");return ESP_ERR_NO_MEM;}}
 pcm_storage=static_cast<uint8_t *>(heap_caps_malloc(PCM_BYTES+1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));text=static_cast<char *>(heap_caps_malloc(TEXT_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
 if(pcm_storage)pcm_stream=xStreamBufferCreateStatic(PCM_BYTES+1,1,pcm_storage,&pcm_control);
 if(!pcm_stream||!text){memo_cloud_config_clear(&c);release();notice("录音内存不足");return ESP_ERR_NO_MEM;}
 uint32_t next=0;if(!memo_record_ids_reserve(std::max(counter,floor),&next)){memo_cloud_config_clear(&c);release();notice("记录状态写入失败");return ESP_FAIL;}
 current={};current.direct=true;current.id=next|MEMO_CARDLESS_ID_FLAG;current.state=MEMO_JOB_QUEUED;current.qwen_generation=c.qwen_generation;current.siyuan_generation=c.siyuan_generation;
 if(!save_current()||!marker(current.id)){fault();memo_cloud_config_clear(&c);release();return ESP_FAIL;}
 {Lock lock(meta_mutex);records[count++]={current.id,0,0,uint32_t(current.state)};counter=next;error[0]=0;}
 config=c;memo_cloud_config_clear(&c);text_bytes=0;samples=0;cancelled=false;abort_io=false;stopped=false;stopped_at=0;ws_started=finalized=false;
 uint32_t requested=current.id;prepared=false;capturing=false;active=requested;
 revision.fetch_add(1);network_sync_wake();int64_t deadline=now()+15000;
 while(active.load()==requested&&!prepared.load()&&now()<deadline)vTaskDelay(pdMS_TO_TICKS(20));
 {Lock lock(pcm_mutex);if(active.load()==requested&&prepared.load()&&!stopped.load()&&!cancelled.load()&&pcm_stream){capturing=true;*id=requested;return ESP_OK;}}
 if(active.load()==requested){stopped=true;abort_io=true;network_sync_wake();}
 return ESP_FAIL;
}
extern "C" bool memo_cardless_append(const int16_t *pcm,size_t n){
 if(!pcm||!n||!capturing||!active||stopped)return false;
 {Lock lock(pcm_mutex);if(!capturing||!pcm_stream)return false;
  if(n>PCM_BYTES/2||samples.load()>UINT32_MAX/2-n||xStreamBufferSpacesAvailable(pcm_stream)<n*2||xStreamBufferSend(pcm_stream,pcm,n*2,0)!=n*2){stopped=true;abort_io=true;notice("网络等待过久");network_sync_wake();return false;}
  samples.fetch_add(uint32_t(n));}
network_sync_wake();return true;
}
extern "C" void memo_cardless_finish(bool interrupted,const char *reason){
 if(!active)return;
 {Lock lock(pcm_mutex);int64_t zero=0;stopped_at.compare_exchange_strong(zero,now());
#ifdef MEMO_CARDLESS_TEST
  cardless_test_stop_transition();
#endif
  if(interrupted){if(reason&&reason[0])notice(reason);stopped=true;abort_io=true;}capturing=false;}
 network_sync_wake();
}
extern "C" void memo_cardless_network_step(){
 if(!active)return;
 if(metadata_fault){release();return;}
 if(cancelled){abort_io=true;finish_record(current.may_have_remote_audio?MEMO_JOB_UNCERTAIN:MEMO_JOB_CANCELLED,current.may_have_remote_audio?"投递待核对":"");return;}
 if(stopped||!online()){stopped=true;abort_io=true;finish_record(MEMO_JOB_FAILED,"录音已中断");return;}
 if(!ws_started&&!finalized){
  platform=new(std::nothrow) MemoCloudEsp(abort_io);if(platform)realtime=new(std::nothrow) memo_cloud::Realtime(platform->io());
  if(!realtime){finish_record(MEMO_JOB_FAILED,"录音内存不足");return;}
  auto result=realtime->begin(config.qwen_key,{nullptr,append_text});
  if(result.code!=memo_cloud::Code::Ok){fail_asr(result,"handshake");return;}
  current.state=MEMO_JOB_TRANSCRIBING;if(!save_current()){release();return;}
  ws_started=true;prepared=true;return;
 }
 if(!capturing&&!stopped_at.load())return; // Audio owner has not started acquisition yet.
 if(!finalized){
  uint8_t block[3200];
  while(true){size_t got=0;{Lock lock(pcm_mutex);size_t queued=xStreamBufferBytesAvailable(pcm_stream);if(queued>=sizeof(block)||!capturing)got=xStreamBufferReceive(pcm_stream,block,std::min(queued,sizeof(block)),0);}
   if(!got)break;
   if(!capturing&&now()-stopped_at.load()>=5000){finish_record(MEMO_JOB_FAILED,"处理等待超时");return;}
   uint32_t send_left=capturing?2000:uint32_t(std::min<int64_t>(2000,5000-(now()-stopped_at.load())));
   auto result=realtime->audio(block,got,send_left);if(result.code!=memo_cloud::Code::Ok){fail_asr(result,"append");return;}
   if(stopped||cancelled)break;
  }
  if(stopped||cancelled)return;
  auto result=realtime->poll();if(result.code!=memo_cloud::Code::Ok){fail_asr(result,"poll");return;}
  if(capturing)return;
  int64_t left=30000-(now()-stopped_at.load());if(left<=0){finish_record(MEMO_JOB_FAILED,"处理等待超时");return;}
  result=realtime->finish(uint32_t(left));delete realtime;realtime=nullptr;delete platform;platform=nullptr;
  {Lock lock(pcm_mutex);vStreamBufferDelete(pcm_stream);pcm_stream=nullptr;free(pcm_storage);pcm_storage=nullptr;}
  if(result.code!=memo_cloud::Code::Ok){fail_asr(result,"finish");return;}
  if(cancelled){finish_record(MEMO_JOB_CANCELLED,"");return;}
  Clean cleaned;text_bytes=memo_transcript_clean(text,text_bytes,clean_text,&cleaned);
  if(text_bytes==SIZE_MAX||!text_bytes){finish_record(MEMO_JOB_REJECTED,"未识别到语音");return;}
  time_t stamp=time(nullptr);tm utc{};char imported[32];
  if(!gmtime_r(&stamp,&utc)||!strftime(imported,sizeof(imported),"%Y-%m-%dT%H:%M:%SZ",&utc)){finish_record(MEMO_JOB_FAILED,"处理已中断");return;}
  int length=snprintf(delivery_prefix,sizeof(delivery_prefix),"**喵伴无卡 #%lu** · 云端导入时间 %s\n\n",(unsigned long)MEMO_CARDLESS_DISPLAY_ID(current.id),imported);
  if(length<0||size_t(length)>=sizeof(delivery_prefix)||text_bytes>TEXT_BYTES-size_t(length)){finish_record(MEMO_JOB_FAILED,"内容超出上限");return;}prefix_bytes=size_t(length);
  finalized=true;current.state=MEMO_JOB_WAITING;if(!save_current()){release();return;}
 }
 if(now()+30000>stopped_at.load()+120000){finish_record(MEMO_JOB_FAILED,"处理等待超时");return;}
 if(now()<delivery_due||now()<network_sync_delivery_last()+10000)return;
 current.delivery_attempts++;current.state=MEMO_JOB_DELIVERING;current.may_have_remote_audio=true;
 if(!save_current()){release();return;}
 platform=new(std::nothrow) MemoCloudEsp(abort_io);if(!platform){finish_record(MEMO_JOB_FAILED,"录音内存不足");return;}
 network_sync_delivery_started();char title[128];snprintf(title,sizeof(title),"喵伴无卡语音笔记 #%lu",(unsigned long)MEMO_CARDLESS_DISPLAY_ID(current.id));
 auto result=memo_cloud::deliver(platform->io(),config.siyuan_token,title,{nullptr,text_bytes+prefix_bytes,source_text});delete platform;platform=nullptr;
 if(result.code==memo_cloud::Code::Ok){finish_record(MEMO_JOB_SENT,"");return;}
 if(result.code==memo_cloud::Code::Uncertain){finish_record(MEMO_JOB_UNCERTAIN,"投递待核对");return;}
 current.may_have_remote_audio=false;
 if(cancelled){finish_record(MEMO_JOB_CANCELLED,"");return;}
 bool retry=result.code==memo_cloud::Code::Limited||(!result.may_have_sent&&(result.code==memo_cloud::Code::Timeout||result.code==memo_cloud::Code::Transport||result.code==memo_cloud::Code::Tls));
 uint32_t wait=std::max<uint32_t>(current.delivery_attempts==1?10000:60000,result.retry_after>300?300001:result.retry_after*1000);
 if(!retry||current.delivery_attempts>=3||result.retry_after>300||now()+wait+30000>stopped_at.load()+120000){finish_record(MEMO_JOB_FAILED,reason(result.code));return;}
 current.state=MEMO_JOB_WAITING;if(!save_current()){release();return;}delivery_due=now()+wait;
}
extern "C" bool memo_cardless_recording(){return capturing;}
extern "C" bool memo_cardless_busy(){return initialized&&active.load();}
extern "C" bool memo_cardless_should_stop(char *out,size_t cap){if(stopped){Lock lock(meta_mutex);copy(out,cap,error[0]?error:"录音已中断");return true;}return false;}
extern "C" uint32_t memo_cardless_elapsed_ms(){return samples.load()/16;}
extern "C" void memo_cardless_error(char *out,size_t cap){if(!meta_mutex){copy(out,cap,"记录存储不可用");return;}Lock lock(meta_mutex);copy(out,cap,error);}
extern "C" size_t memo_cardless_count(){if(!initialized)return 0;Lock lock(meta_mutex);return count;}
extern "C" size_t memo_cardless_list(size_t offset,memo_cardless_record_t *out,size_t capacity){
 if(!initialized||!out)return 0;
 Lock lock(meta_mutex);size_t n=0;while(offset+n<count&&n<capacity){auto &i=records[count-1-offset-n];out[n]={};out[n].id=i.id;out[n].duration_ms=i.duration;out[n].encoded_bytes=i.bytes;out[n].state=memo_job_state_t(i.state&~LEGACY);out[n].direct=!(i.state&LEGACY);n++;}return n;
}
extern "C" bool memo_cardless_get(uint32_t id,memo_cardless_record_t *out){
 if(!initialized||!out)return false;
 Lock lock(meta_mutex);size_t at=find(id);if(at==SIZE_MAX)return false;
 Detail *victim=nullptr;for(auto &d:details){if(d.value.id==id){d.age=++age;*out=d.value;return true;}if(!d.value.id||(d.value.id!=active&&d.value.id!=selected&&(!victim||d.age<victim->age)))victim=&d;}
 if(!victim)return false;
 memo_cardless_record_t r{};if(!read_record(id,(records[at].state&LEGACY)!=0,r))return false;victim->value=r;victim->age=++age;*out=r;return true;
}
extern "C" void memo_cardless_select(uint32_t id){selected=id;}
extern "C" bool memo_cardless_peek(uint32_t id,memo_cardless_record_t *out){if(!initialized||!out)return false;
 Lock lock(meta_mutex);for(auto &d:details)if(d.value.id==id){*out=d.value;return true;}return false;}
extern "C" uint32_t memo_cardless_revision(){return revision;}
extern "C" bool memo_cardless_can_cancel(uint32_t id){return id&&id==active&&!metadata_fault&&!cancelled;}
extern "C" bool memo_cardless_cancel(uint32_t id){
 if(!memo_cardless_can_cancel(id))return false;
 {Lock lock(pcm_mutex);int64_t zero=0;stopped_at.compare_exchange_strong(zero,now());cancelled=true;abort_io=true;capturing=false;}
 network_sync_wake();return true;
}
