#include "shim.h"
#include "memo_cardless.h"
#include "memo_cloud_config.h"
#include "memo_cloud.h"
#include "memo_record_ids.h"
#include "legacy_cardless_fixture.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>
static int64_t clock_ms=100000;
static std::map<int,std::map<std::string,std::vector<uint8_t>>> disk,pending;
static size_t free_entries=8000,partition_entries=8064;static int writes=0,fault_write=0,handles=0;
static bool wifi=true,config_enabled=true,fail_alloc=false,fail_record_read=false;
static unsigned highwater=0;static bool yield_stop=false;
static std::map<void *,size_t> allocations;static size_t live_bytes=0,peak_bytes=0;
extern "C" void *heap_caps_malloc(size_t n,int caps){assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));if(fail_alloc)return nullptr;void *p=malloc(n);if(p){allocations[p]=n;live_bytes+=n;peak_bytes=std::max(peak_bytes,live_bytes);}return p;}
extern "C" void *heap_caps_calloc(size_t n,size_t size,int caps){void*p=heap_caps_malloc(n*size,caps);if(p)memset(p,0,n*size);return p;}
static void cardless_free(void *p){auto it=allocations.find(p);if(it!=allocations.end()){live_bytes-=it->second;allocations.erase(it);}free(p);}
extern "C" void *memo_cloud_test_allocate(size_t n){return heap_caps_malloc(n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);}
extern "C" void memo_cloud_test_free(void *p){cardless_free(p);}
extern "C" SemaphoreHandle_t xSemaphoreCreateMutex(){return new std::recursive_mutex;}
extern "C" int xSemaphoreTake(void *p,unsigned wait){if(!wait)return static_cast<std::recursive_mutex*>(p)->try_lock();static_cast<std::recursive_mutex*>(p)->lock();return 1;}
extern "C" int xSemaphoreGive(void *p){static_cast<std::recursive_mutex*>(p)->unlock();return 1;}
extern "C" StreamBufferHandle_t xStreamBufferCreateStatic(size_t n,size_t,uint8_t *data,StaticStreamBuffer_t *s){*s={data,n-1,0,0};return s;}
extern "C" void vStreamBufferDelete(StreamBufferHandle_t){}
extern "C" size_t xStreamBufferSpacesAvailable(StreamBufferHandle_t s){return s->cap-s->used;}
extern "C" size_t xStreamBufferBytesAvailable(StreamBufferHandle_t s){return s->used;}
extern "C" size_t xStreamBufferSend(StreamBufferHandle_t s,const void *p,size_t n,unsigned){if(n>s->cap-s->used)return 0;for(size_t i=0;i<n;i++)s->data[(s->head+s->used+i)%s->cap]=static_cast<const uint8_t *>(p)[i];s->used+=n;return n;}
extern "C" size_t xStreamBufferReceive(StreamBufferHandle_t s,void *out,size_t n,unsigned){n=std::min(n,s->used);for(size_t i=0;i<n;i++)static_cast<uint8_t *>(out)[i]=s->data[(s->head+i)%s->cap];s->used-=n;s->head=(s->head+n)%s->cap;return n;}
extern "C" int64_t esp_timer_get_time(){return clock_ms*1000;}
extern "C" int nvs_flash_init_partition(const char *s){assert(!strcmp(s,"memo_meta"));return ESP_OK;}
extern "C" int nvs_open_from_partition(const char *p,const char *s,int mode,int *h){assert(!strcmp(p,"memo_meta"));int id=!strcmp(s,"direct1")?1:2;if(mode==NVS_READONLY&&!disk.count(id))return ESP_ERR_NVS_NOT_FOUND;*h=id;handles++;return ESP_OK;}
extern "C" void nvs_close(int){handles--;}
extern "C" int nvs_get_blob(int h,const char *key,void *out,size_t *n){if(fail_record_read)return ESP_FAIL;auto &d=disk[h];auto it=d.find(key);if(it==d.end())return ESP_ERR_NVS_NOT_FOUND;if(*n<it->second.size())return ESP_FAIL;*n=it->second.size();memcpy(out,it->second.data(),*n);return ESP_OK;}
extern "C" int nvs_set_blob(int h,const char *key,const void *p,size_t n){writes++;if(fault_write==1)return ESP_FAIL;auto *bytes=static_cast<const uint8_t *>(p);pending[h][key]={bytes,bytes+n};return ESP_OK;}
extern "C" int nvs_commit(int h){if(fault_write==1)return ESP_FAIL;for(auto &pair:pending[h])disk[h][pair.first]=pair.second;pending[h].clear();return fault_write==2?ESP_FAIL:ESP_OK;}
static size_t used_entries(){size_t used=0;for(auto &space:disk){used++;for(auto &entry:space.second)used+=2+(entry.second.size()+31)/32;}return used;}
extern "C" int nvs_get_stats(const char *,nvs_stats_t *s){size_t used=used_entries();s->free_entries=std::min(free_entries,partition_entries>used?partition_entries-used:0);return ESP_OK;}
struct Iterator{std::vector<std::string> keys;size_t at=0;};
extern "C" int nvs_entry_find(const char *,const char *space,int,void **out){auto *it=new Iterator;for(auto &p:disk[!strcmp(space,"direct1")?1:2])it->keys.push_back(p.first);if(it->keys.empty()){delete it;*out=nullptr;return ESP_ERR_NVS_NOT_FOUND;}*out=it;return ESP_OK;}
extern "C" int nvs_entry_next(void **p){auto *it=static_cast<Iterator *>(*p);return ++it->at<it->keys.size()?ESP_OK:ESP_ERR_NVS_NOT_FOUND;}
extern "C" int nvs_entry_info(void *p,nvs_entry_info_t *out){auto *it=static_cast<Iterator *>(p);snprintf(out->key,sizeof(out->key),"%s",it->keys[it->at].c_str());return ESP_OK;}
extern "C" void nvs_release_iterator(void *p){delete static_cast<Iterator *>(p);}
extern "C" bool memo_record_ids_reserve(uint32_t floor,uint32_t *id){*id=highwater=std::max(highwater,floor)+1;return true;}
extern "C" bool memo_cloud_config_read(memo_cloud_config_t *c){*c={};c->enabled=config_enabled;c->qwen_generation=c->siyuan_generation=1;strcpy(c->workspace,"llm-test");strcpy(c->qwen_key,"synthetic-key");strcpy(c->siyuan_token,"synthetic-token");return true;}
extern "C" bool memo_cloud_config_ready(const memo_cloud_config_t *c){return c&&c->qwen_generation&&c->siyuan_generation&&c->qwen_key[0]&&c->siyuan_token[0];}
extern "C" void memo_cloud_config_clear(memo_cloud_config_t *c){memset(c,0,sizeof(*c));}
#include "network_sync.h"
extern "C" void network_sync_get_status(memo_network_status_t *n){*n={};n->initialized=true;n->connected=wifi;}
static int64_t shared_delivery=0;
extern "C" int64_t network_sync_delivery_last(){return shared_delivery;}
extern "C" void network_sync_delivery_started(){shared_delivery=clock_ms;}
extern "C" void network_sync_task_result(uint32_t,memo_job_state_t,const char*){}
extern "C" void network_sync_wake(){}
static std::deque<std::string> events;static std::string mode="ok",body,response;static size_t response_at=0;static bool leased=false,sent=false,ws=false;static int deliveries=0,leases=0,ws_opened=0,ws_closed=0;static size_t audio_bytes=0;static unsigned item=0;static int64_t handshake_until=0;static std::vector<int64_t> delivery_times;
memo_cloud::IO cardless_test_io{};
#define free cardless_free
#include "../main/memo_cardless.cpp"
#undef free
void cardless_test_stop_transition(){if(yield_stop){assert(capturing&&stopped_at>0);memo_cardless_network_step();assert(capturing&&active&&!deliveries);}}
extern "C" void vTaskDelay(unsigned ms){clock_ms+=ms;memo_cardless_network_step();}
static void pair_text(const std::string &value){std::string prev=item?"item"+std::to_string(item):"";item++;std::string id="item"+std::to_string(item);events.push_back("{\"type\":\"input_audio_buffer.committed\",\"item_id\":\""+id+"\",\"previous_item_id\":"+(prev.empty()?"null":"\""+prev+"\"")+"}");events.push_back("{\"type\":\"conversation.item.input_audio_transcription.completed\",\"item_id\":\""+id+"\",\"transcript\":\""+value+"\"}");}
static void io_init(){
 cardless_test_io.now=[](void*){return clock_ms;};cardless_test_io.cancelled=[](void*){return abort_io.load();};
 cardless_test_io.acquire=[](void*){assert(!leased);leased=true;leases++;return true;};cardless_test_io.release=[](void*){assert(leased);leased=false;leases--;};cardless_test_io.failure=[](void*){return memo_cloud::Code::Transport;};
 cardless_test_io.ws_open=[](void*,const char*,const char*,int64_t){ws=true;ws_opened++;item=0;if(mode=="slow_handshake"||mode=="handshake_timeout")handshake_until=clock_ms+(mode=="slow_handshake"?3000:16000);events.push_back("{\"type\":\"session.created\"}");return 0;};
 cardless_test_io.ws_send=[](void*,const char *s,size_t n,int){std::string v(s,n);if(v.find("session.update")!=std::string::npos){events.push_back("{\"type\":\"session.updated\"}");if(mode=="cancel_handshake")memo_cardless_cancel(active);if(mode=="credentials")events.push_front("{\"type\":\"error\",\"error\":{\"code\":\"invalid_api_key\"}}");}
  if(v.find("input_audio_buffer.append")!=std::string::npos){audio_bytes+=n;if(mode=="cancel_audio"){memo_cardless_cancel(active);return -1;}if(mode=="send_block"){clock_ms+=2001;return -1;}if(mode=="disconnect")return -1;}
  if(v.find("session.finish")!=std::string::npos&&mode!="final_timeout"){if(!item)pair_text("这是合成测试正文");events.push_back("{\"type\":\"session.finished\"}");}return int(n);};
 cardless_test_io.ws_read=[](void*,char *out,size_t n,int ms){
  if(handshake_until>clock_ms){int16_t pcm[512]{};assert(!memo_cardless_append(pcm,512)&&!capturing&&samples==0);clock_ms+=ms;return 0;}
  if(events.empty()){clock_ms+=ms;return 0;}std::string s=events.front();events.pop_front();assert(s.size()<=n);memcpy(out,s.data(),s.size());return int(s.size());};
 cardless_test_io.ws_close=[](void*){assert(ws);ws=false;ws_closed++;events.clear();};
 cardless_test_io.http_open=[](void*,const char*,const char*,size_t,int64_t){assert(!ws&&current.state==MEMO_JOB_DELIVERING&&current.may_have_remote_audio);body.clear();sent=false;response_at=0;deliveries++;delivery_times.push_back(clock_ms);return mode=="delivery_retry"?-1:0;};
 cardless_test_io.http_write=[](void*,const void *p,size_t n,int){sent=true;body.append(static_cast<const char *>(p),n);if(mode=="cancel_delivery"){memo_cardless_cancel(active);return -1;}return int(n);};
 cardless_test_io.http_headers=[](void*,uint32_t *retry,int){*retry=0;response="{\"code\":0}";if(mode=="uncertain")return -1;if(mode=="delivery_limited"){*retry=10;return 429;}return 200;};
 cardless_test_io.http_read=[](void*,void *p,size_t n,int){n=std::min(n,response.size()-response_at);memcpy(p,response.data()+response_at,n);response_at+=n;if(mode=="cancel_success"&&response_at==response.size())memo_cardless_cancel(active);return int(n);};
 cardless_test_io.http_complete=[](void*){return response_at==response.size();};cardless_test_io.http_sent=[](void*){return sent;};cardless_test_io.http_close=[](void*){};
}
static void boot(){assert(memo_cardless_init()==ESP_OK);memo_cardless_configure(config_enabled,false);}
static void reset_boot(){if(active)release();cardless_free(records);records=nullptr;for(void *p:{meta_mutex,pcm_mutex,begin_mutex})delete static_cast<std::recursive_mutex *>(p);meta_mutex=pcm_mutex=begin_mutex=nullptr;if(metadata){nvs_close(metadata);metadata=0;}if(legacy_open){nvs_close(legacy);legacy_open=false;}count=counter=age=0;for(auto &d:details)d={};initialized=metadata_fault=false;selected=0;highwater=0;}
static uint32_t begin(){uint32_t id=0;assert(memo_cardless_begin(0,&id)==ESP_OK&&id);return id;}
static void feed(){int16_t pcm[1600]{};assert(memo_cardless_append(pcm,1600));memo_cardless_network_step();}
static memo_cardless_record_t end(uint32_t id){memo_cardless_finish(false,"");for(int i=0;i<15&&memo_cardless_busy();i++){memo_cardless_network_step();clock_ms+=10000;}assert(!memo_cardless_busy());memo_cardless_record_t r{};assert(memo_cardless_get(id,&r));assert(!leases&&!ws&&live_bytes==16384);return r;}
int main(int argc,char **argv){assert(argc==2);std::string name=argv[1];io_init();boot();
 if(name=="no_network"){wifi=false;uint32_t id=0;assert(!memo_cardless_ready()&&memo_cardless_begin(0,&id)!=ESP_OK&&!id&&!writes);}
 else if(name=="capacity"){free_entries=157;uint32_t id=0;assert(memo_cardless_begin(0,&id)!=ESP_OK&&!id&&writes==0);}
 else if(name=="memory"){fail_alloc=true;uint32_t id=0;assert(memo_cardless_begin(0,&id)!=ESP_OK&&!id&&!writes);assert(live_bytes==16384);}
 else if(name=="credentials"||name=="cancel_handshake"){
  mode=name;uint32_t id=0;assert(memo_cardless_begin(0,&id)!=ESP_OK&&!id&&!capturing&&!active&&live_bytes==16384&&!deliveries);
 }
 else if(name=="metadata_begin"||name=="metadata_commit"){fault_write=name=="metadata_begin"?1:2;uint32_t id=0;assert(memo_cardless_begin(0,&id)!=ESP_OK&&!id&&metadata_fault&&!memo_cardless_ready()&&live_bytes==16384&&!ws_opened);}
 else if(name=="mixed_full"){
  partition_entries=20000;reset_boot();for(unsigned i=1;i<=1023;i++){memo_cardless_record_t r{};r.id=MEMO_CARDLESS_ID_FLAG|i;r.state=i==1?MEMO_JOB_SENT:MEMO_JOB_WAITING;r.target.instance.bytes[0]=1;r.target.url_hash[0]=2;strcpy(r.target.device_id,"old-fixture");uint8_t bytes[MEMO_CARDLESS_RECORD_SIZE];assert(memo_cardless_record_encode(&r,bytes));char key[16];snprintf(key,sizeof(key),"r%08x",r.id);disk[2][key]={bytes,bytes+sizeof(bytes)};}
  auto original=disk[2];boot();assert(count==1023&&live_bytes==16384);memo_cardless_record_t r{};assert(memo_cardless_get(MEMO_CARDLESS_ID_FLAG|2,&r)&&!strcmp(r.error,"旧服务待核对")&&!r.can_retry);uint32_t id=begin();assert(MEMO_CARDLESS_DISPLAY_ID(id)==1024);feed();assert(end(id).state==MEMO_JOB_SENT&&count==1024);uint32_t rejected=0;assert(memo_cardless_begin(0,&rejected)!=ESP_OK&&!rejected&&disk[2]==original);int calls=deliveries;reset_boot();boot();assert(count==1024&&!active&&calls==deliveries&&disk[2]==original);
 }
 else if(name=="stop_interleave"){uint32_t id=begin();feed();yield_stop=true;memo_cardless_finish(false,"");yield_stop=false;assert(end(id).state==MEMO_JOB_SENT);}
 else if(name=="capture_error"){uint32_t id=begin();feed();memo_cardless_finish(true,"麦克风同步失败");auto r=end(id);assert(r.state==MEMO_JOB_FAILED&&!strcmp(r.error,"麦克风同步失败"));}
 else if(name=="mixed_capacity"){
  reset_boot();for(unsigned i=1;i<=790;i++){memo_cardless_record_t r{};r.id=MEMO_CARDLESS_ID_FLAG|i;r.state=MEMO_JOB_SENT;r.target.instance.bytes[0]=1;r.target.url_hash[0]=2;strcpy(r.target.device_id,"old-fixture");uint8_t b[228];assert(memo_cardless_record_encode(&r,b));char k[16];snprintf(k,sizeof(k),"r%08x",r.id);disk[2][k]={b,b+sizeof(b)};}
  boot();nvs_stats_t stats{};nvs_get_stats("memo_meta",&stats);assert(stats.free_entries>=158&&stats.free_entries<170);uint32_t id=begin();feed();assert(end(id).state==MEMO_JOB_SENT);nvs_get_stats("memo_meta",&stats);assert(stats.free_entries<158);uint32_t rejected=0;assert(memo_cardless_begin(0,&rejected)!=ESP_OK&&!rejected);
 }
 else if(name=="slow_handshake"||name=="handshake_timeout"){
  mode=name;uint32_t id=0;int64_t start=clock_ms;int result=memo_cardless_begin(0,&id);
  assert(!samples&&clock_ms>=start+3000);
  if(name=="slow_handshake"){assert(result==ESP_OK&&id&&capturing&&prepared);feed();assert(end(id).state==MEMO_JOB_SENT);}
  else {assert(result!=ESP_OK&&!id&&!capturing&&!active&&clock_ms<=start+15100);}
 }
 else if(name=="cross_rate"){shared_delivery=clock_ms;int64_t previous=clock_ms;uint32_t id=begin();feed();assert(end(id).state==MEMO_JOB_SENT&&delivery_times[0]>=previous+10000);}
 else if(name=="config_noop"){uint32_t id=begin();feed();memo_cardless_configure(config_enabled,false);assert(!stopped&&!abort_io&&capturing);assert(end(id).state==MEMO_JOB_SENT);}
 else if(name=="config_stop"){uint32_t id=begin();feed();config_enabled=false;memo_cardless_configure(config_enabled,true);assert(end(id).state==MEMO_JOB_FAILED&&!deliveries&&!memo_cardless_ready());}
 else if(name=="orphan"){
  uint32_t id=begin();feed();fault_write=1;memo_cardless_finish(false,"");memo_cardless_network_step();assert(metadata_fault&&!active);fault_write=0;reset_boot();boot();memo_cardless_record_t r{};assert(memo_cardless_get(id,&r)&&r.state==MEMO_JOB_FAILED&&!ws&&!deliveries);
 }
 else if(name=="boot"){
  for(auto state:{MEMO_JOB_TRANSCRIBING,MEMO_JOB_DELIVERING,MEMO_JOB_CANCELLING}){memo_cardless_record_t r{};r.id=MEMO_CARDLESS_ID_FLAG|++highwater;r.state=state;r.direct=true;r.qwen_generation=r.siyuan_generation=1;r.may_have_remote_audio=state!=MEMO_JOB_TRANSCRIBING;assert(write_record(r)&&marker(r.id));}
  reset_boot();boot();assert(count==3&&!active);for(size_t i=0;i<count;i++){memo_cardless_record_t r;assert(memo_cardless_get(records[i].id,&r));assert(r.state==(i?MEMO_JOB_UNCERTAIN:MEMO_JOB_FAILED));}assert(!deliveries&&!ws_opened);
 }
 else if(name=="history"){
  for(unsigned i=0;i<40;i++){uint32_t id=begin();feed();assert(end(id).state==MEMO_JOB_SENT);}for(unsigned round=0;round<10;round++)for(size_t i=0;i<count;i++){memo_cardless_record_t r;memo_cardless_select(records[i].id);assert(memo_cardless_get(records[i].id,&r)&&!r.can_retry);assert(memo_cardless_peek(r.id,&r));}int before=writes;for(int i=0;i<100;i++)memo_cardless_network_step();assert(writes==before&&live_bytes==16384);
 }
 else if(name=="iterations"){
  for(int i=0;i<50;i++){mode=i%3==0?"ok":i%3==1?"disconnect":"cancel_capture";item=0;uint32_t id=begin();feed();if(mode=="cancel_capture")memo_cardless_cancel(id);auto r=end(id);assert(terminal(r.state));}assert(ws_opened==ws_closed);
 }
 else {
  uint32_t id=begin();
  if(name=="queue_full"){int16_t pcm[16384]{};assert(memo_cardless_append(pcm,16384));assert(memo_cardless_append(pcm,16384));assert(!memo_cardless_append(pcm,1));auto r=end(id);assert(r.state==MEMO_JOB_FAILED&&!strcmp(r.error,"网络等待过久"));}
  else if(name=="text_limit"){feed();for(int i=0;i<17&&active;i++){pair_text(std::string(16384,'a'));memo_cardless_network_step();memo_cardless_network_step();}assert(end(id).state==MEMO_JOB_FAILED&&!deliveries);}
  else if(name=="metadata_terminal"){feed();fault_write=1;memo_cardless_finish(false,"");memo_cardless_network_step();assert(metadata_fault&&!active&&!leases&&!memo_cardless_ready());}
  else if(name=="cancel_capture"){feed();assert(memo_cardless_cancel(id));assert(end(id).state==MEMO_JOB_CANCELLED&&!deliveries);}
  else if(name=="drain_timeout"){feed();int16_t pcm[1600]{};assert(memo_cardless_append(pcm,1600));memo_cardless_finish(false,"");clock_ms+=5001;assert(end(id).state==MEMO_JOB_FAILED&&!deliveries);}
  else {mode=name;feed();auto r=end(id);
   if(name=="cancel_audio"||name=="cancel_handshake"){assert(r.state==MEMO_JOB_CANCELLED&&!deliveries);}
   else if(name=="success"||name=="cancel_success"){assert(r.state==MEMO_JOB_SENT&&deliveries==1);}
   else if(name=="uncertain"||name=="cancel_delivery"){assert(r.state==MEMO_JOB_UNCERTAIN&&deliveries==1);}
   else if(name=="delivery_retry"||name=="delivery_limited"){assert(r.state==MEMO_JOB_FAILED&&deliveries==3);assert(delivery_times[1]-delivery_times[0]>=10000&&delivery_times[2]-delivery_times[1]>=60000);}
   else if(name=="metadata_begin"){assert(r.state==MEMO_JOB_SENT);}
   else {assert(r.state==MEMO_JOB_FAILED&&!deliveries);}
  }
 }
 reset_boot();assert(allocations.empty()&&handles==0);printf("PASS %s runtime_peak=%zu (excludes SDK/TLS; step6)\n",name.c_str(),peak_bytes);
}
