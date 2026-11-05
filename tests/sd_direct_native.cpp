// Synthetic files and transport only; production SD state machine and protocol core.
#include "memo_sd_sync.h"
#include "memo_transcript.h"
#include "legacy_sync_fixture.h"
#include "network_sync_state.h"
#include "nvs.h"
#include <cassert>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <unistd.h>
using namespace memo_sd;
static std::mutex sharing;
static std::map<FILE*,std::string> files;
static std::string fail_open,drop_filter;
static int drop_write=0,fail_sync=0,fail_unlink=0,write_calls=0,conflicts=0;
static size_t metadata_reads=0,wav_reads=0;static bool fail_wave_read=false;
extern "C" FILE *sd_test_fopen(const char *path,const char *mode){
 std::lock_guard<std::mutex> lock(sharing);if(!fail_open.empty()&&std::string(path).find(fail_open)!=std::string::npos){errno=EACCES;return nullptr;}
 bool writer=mode[0]!='r'||strchr(mode,'+');
 for(const auto &item:files)if(item.second==path&&writer){conflicts++;errno=EACCES;return nullptr;}
 FILE *f=fopen(path,mode);if(f)files[f]=path;return f;
}
extern "C" int sd_test_fclose(FILE *f){std::lock_guard<std::mutex> lock(sharing);files.erase(f);return fclose(f);}
extern "C" size_t sd_test_fread(void *p,size_t size,size_t n,FILE *f){if(files[f].find(".job.")!=std::string::npos)metadata_reads++;if(fail_wave_read&&files[f].find(".wav")!=std::string::npos){fail_wave_read=false;errno=EIO;return 0;}size_t got=fread(p,size,n,f);if(files[f].find(".wav")!=std::string::npos)wav_reads+=got*size;return got;}
extern "C" size_t sd_test_fwrite(const void *p,size_t size,size_t n,FILE *f){
 write_calls++;if(drop_write&&files[f].find(drop_filter)!=std::string::npos){drop_write--;errno=EIO;return 0;}return fwrite(p,size,n,f);
}
extern "C" int sd_test_fsync(int fd){if(fail_sync){fail_sync--;errno=EIO;return -1;}return fsync(fd);}
extern "C" int sd_test_unlink(const char *path){if(fail_unlink&&std::string(path).find(".text")!=std::string::npos){fail_unlink--;errno=EACCES;return -1;}return unlink(path);}
static std::vector<uint8_t> nvs_data;static int handles=0,nvs_mode=0;
extern "C" int nvs_open(const char *name,int mode,int *h){assert(!strcmp(name,"memo_cloud1")&&mode==NVS_READONLY);if(nvs_mode==1)return ESP_ERR_NVS_NOT_FOUND;if(nvs_mode==2)return 2;handles++;*h=1;return ESP_OK;}
extern "C" int nvs_get_blob(int,const char *key,void *out,size_t *n){assert(!strcmp(key,"config"));if(nvs_data.empty())return ESP_ERR_NVS_NOT_FOUND;if(*n<nvs_data.size())return 2;*n=nvs_data.size();memcpy(out,nvs_data.data(),*n);return ESP_OK;}
extern "C" void nvs_close(int){handles--;}
static uint32_t crc(const uint8_t *p,size_t n){uint32_t c=UINT32_MAX;while(n--){c^=*p++;for(unsigned i=0;i<8;i++)c=(c>>1)^(0xedb88320U&(0U-(c&1)));}return ~c;}
static void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=v>>(8*i);}
static uint32_t get32(const uint8_t *p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
static std::vector<uint8_t> bytes(const std::string &path){std::ifstream f(path,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static void raw_write(const std::string &path,const std::vector<uint8_t> &data){std::ofstream f(path,std::ios::binary);f.write(reinterpret_cast<const char *>(data.data()),data.size());assert(f.good());}
struct Fixture {
 std::string root,mode="ok",request,response;memo_cloud_config_t config{};
 Runtime runtime;IO env{};memo_cloud::IO cloud{};Result result;
 Control control=Control::Run;int64_t now=100000;
 bool online=true,clock=true,leased=false,sent=false,delivery=false,capture=false;
 int status=200,asr=0,posted=0,delivered=0,opens=0,clock_calls=0;
 uint32_t id=1;size_t cursor=0;
 std::vector<int64_t> delivery_starts;
 explicit Fixture(std::string path):root(std::move(path)){
  config.enabled=true;config.qwen_generation=config.siyuan_generation=1;
  strcpy(config.qwen_key,"synthetic-key");strcpy(config.siyuan_token,"synthetic-token");
  env.context=this;env.cloud=&cloud;env.now_ms=[](void*p){return static_cast<Fixture*>(p)->now;};
  env.unix_seconds=[](void*){return int64_t(1800000000);};env.connected=[](void*p){return static_cast<Fixture*>(p)->online;};
  env.clock_ready=[](void*p){auto &s=*static_cast<Fixture*>(p);s.clock_calls++;return s.clock;};env.control=[](void*p){return static_cast<Fixture*>(p)->control;};
  cloud.context=this;cloud.now=env.now_ms;cloud.cancelled=[](void*p){return static_cast<Fixture*>(p)->control!=Control::Run;};
  cloud.acquire=[](void*p){auto &s=*static_cast<Fixture*>(p);assert(!s.leased);s.leased=true;return true;};cloud.release=[](void*p){auto &s=*static_cast<Fixture*>(p);assert(s.leased);s.leased=false;};
  cloud.http_open=[](void*p,const char *url,const char*,size_t,int64_t){
   auto &s=*static_cast<Fixture*>(p);s.opens++;s.request.clear();s.cursor=0;s.sent=false;s.delivery=strstr(url,"siyuan")!=nullptr;
   Entry e;assert(read(s.root.c_str(),s.id,&e)==MEMO_CACHE_OK&&e.intent);
   assert(s.delivery?(e.delivery_used&&e.may_deliver&&e.state==MEMO_JOB_DELIVERING):(e.asr_used&&e.state==MEMO_JOB_TRANSCRIBING));
   if(s.delivery)s.delivery_starts.push_back(s.now);
   return s.mode=="connect_fail"?-1:0;
  };
  cloud.http_write=[](void*p,const void *data,size_t n,int){auto &s=*static_cast<Fixture*>(p);s.sent=true;size_t count=std::min<size_t>(701,n);s.request.append(static_cast<const char*>(data),count);if(s.mode=="audio_read_fail")fail_wave_read=true;return int(count);};
  cloud.http_headers=[](void*p,uint32_t *retry,int){
   auto &s=*static_cast<Fixture*>(p);s.status=200;*retry=0;
   if(s.delivery){s.posted++;s.response="{\"code\":0}";if(s.capture){std::ofstream f(s.root+"/delivery.json");f<<s.request;}if(s.mode=="delivery_500")s.status=500;if(s.mode=="delivery_429"){s.status=429;*retry=12;}if(s.mode=="delivery_reject")s.response="{\"code\":1}";
    if(s.mode=="crash_delivery"){drop_filter=".job.";drop_write=1;}
    if(s.mode=="delivery_lost"||s.mode=="cancel_lost"){if(s.mode=="cancel_lost")s.control=Control::Cancel;return -1;}
   }else{
    s.asr++;s.response="{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"content\":\"segment "+std::to_string(s.asr)+"\"}}]}";
    if(s.mode=="headers_disconnect"){s.control=Control::Media;return -1;}
    if(s.capture){std::ofstream f(s.root+"/asr-"+std::to_string(s.asr)+".json");f<<s.request;}
    if(s.mode=="asr_500")s.status=500;
    if(s.mode=="asr_401"||s.mode=="media_401"||s.mode=="media_headers_401")s.status=401;
    if(s.mode=="media_headers_403")s.status=403;
    if(s.mode=="timeout_401")s.status=401;
    if(s.mode=="timeout_403")s.status=403;
    if(s.mode=="timeout_400")s.status=400;
    if(s.mode=="clean")s.response="{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"content\":\"你好喵伴，正文。你好喵伴保存录音\"}}]}";
    if(s.mode=="clean_empty")s.response="{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"content\":\"你好喵伴，停止录音\"}}]}";
    if(s.mode=="asr_bad")s.response="{bad";
    if(s.mode=="asr_empty")s.response="{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"content\":\" \"}}]}";
    if(s.mode=="asr_429"||s.mode=="asr_429_long"||s.mode=="media_301"){s.status=429;*retry=s.mode=="asr_429"?90:301;}
    if(s.mode=="cancel_asr")s.control=Control::Cancel;
    if(s.mode=="pause_asr")s.control=Control::Pause;
    if(s.mode=="crash_asr"){drop_filter=".job.";drop_write=1;}
    if(s.mode=="text_fail"){drop_filter=".text";drop_write=1;}
   }
   if(s.mode=="headers_wait_429"||s.mode=="headers_wait_503"||s.mode=="headers_wait_408"){
    s.status=s.mode=="headers_wait_429"?429:s.mode=="headers_wait_503"?503:408;*retry=90;s.control=Control::Media;
   }
   if(s.mode=="media_headers_401"||s.mode=="media_headers_403")s.control=Control::Media;
   if(s.status!=200)s.response="{\"code\":\"synthetic_rejection\"}";
   return s.status;
  };
  cloud.http_read=[](void*p,void *out,size_t capacity,int){auto &s=*static_cast<Fixture*>(p);if(s.mode.rfind("timeout_",0)==0){s.now+=250000;return 0;}size_t n=std::min<size_t>(7,std::min(capacity,s.response.size()-s.cursor));memcpy(out,s.response.data()+s.cursor,n);s.cursor+=n;if(s.delivery&&s.mode=="cancel_success"&&s.cursor==s.response.size())s.control=Control::Cancel;return int(n);};
  cloud.http_complete=[](void*p){auto &s=*static_cast<Fixture*>(p);return s.cursor==s.response.size();};
  cloud.http_sent=[](void*p){return static_cast<Fixture*>(p)->sent;};
  cloud.http_close=[](void*p){auto &s=*static_cast<Fixture*>(p);if(s.mode=="media_401"||s.mode=="media_301")s.control=Control::Media;if(s.delivery&&s.status==200&&s.cursor==s.response.size()&&s.response=="{\"code\":0}")s.delivered++;};
  cloud.failure=[](void*){return memo_cloud::Code::Transport;};
 }
 ~Fixture(){assert(!leased&&files.empty());memo_cloud_config_clear(&config);}
 std::string wav()const{char p[64];snprintf(p,sizeof(p),"/memo_%010u.wav",unsigned(id));return root+p;}
 std::string text()const{char p[64];snprintf(p,sizeof(p),"/direct_r1_%010u.text",unsigned(id));return root+p;}
 std::string job(unsigned slot)const{char p[64];snprintf(p,sizeof(p),"/direct_r1_%010u.job.%u",unsigned(id),slot);return root+p;}
 Entry entry(){Entry e;assert(read(root.c_str(),id,&e)==MEMO_CACHE_OK);return e;}
 void wave(unsigned seconds=1){memo_store_t store;assert(!memo_store_init(&store,root.c_str()));assert(!memo_store_begin_reserved(&store,id));int16_t pcm[3200];uint32_t total=seconds*16000;
  for(uint32_t at=0;at<total;){size_t n=std::min<uint32_t>(3200,total-at);for(size_t i=0;i<n;i++){uint32_t s=at+i;pcm[i]=(s>=168*16000&&s<168*16000+320)?0:1000+(s%997);}assert(!memo_store_append(&store,pcm,n));at+=n;}
  memo_record_t record;assert(!memo_store_finish(&store,&record));
 }
 void start(bool automatic=true){assert(create(root.c_str(),id,config,automatic));}
 void tick(){step(root.c_str(),id,config,env,runtime,result);assert(!leased&&files.empty());}
 void finish(){for(int i=0;i<40;i++){tick();if(!result.pending)return;now+=std::max(1U,result.delay_ms);}assert(false);}
 void view(){memo_sd::view(root.c_str(),id,config,result);}
 void reboot(){runtime=Runtime{};control=Control::Run;}
};
static void scenario(const std::string &name,const std::string &root){
 Fixture f(root);f.wave(name=="segments"?367:1);
 if(name=="segments"){
  f.capture=true;f.start();f.finish();assert(f.entry().state==MEMO_JOB_SENT&&f.asr>=3&&f.posted==1&&f.delivered==1&&access(f.text().c_str(),F_OK));
  int calls=f.opens,writes=write_calls;for(int i=0;i<100;i++){f.view();f.tick();}assert(f.opens==calls&&write_calls==writes&&!submit(root.c_str(),1,f.config));
 }else if(name=="offline"){
  f.config.enabled=false;f.start(false);bool complete=false;uint32_t after=0;assert(!pending(root.c_str(),f.config,&after,&complete)&&complete);
  f.config.enabled=true;assert(submit(root.c_str(),1,f.config));assert(!submit(root.c_str(),1,f.config));f.online=false;f.tick();assert(f.result.pending&&!f.opens);
  f.reboot();f.online=true;f.finish();assert(f.entry().state==MEMO_JOB_SENT);
 }else if(name=="details"){
  f.start();f.finish();auto old=bytes(f.wav());wav_reads=0;int writes=write_calls,calls=f.opens;for(int i=0;i<100;i++)f.view();assert(!wav_reads&&write_calls==writes&&f.opens==calls);
  auto replacement=old;replacement[44]^=1;raw_write(f.wav(),replacement);f.view();assert(f.result.state==MEMO_JOB_SENT);replacement.push_back(0);raw_write(f.wav(),replacement);f.view();assert(f.result.error==Error::Audio&&!f.result.can_retry);raw_write(f.wav(),old);
 }else if(name=="legacy"){
  for(int state=MEMO_JOB_LOCAL;state<=MEMO_JOB_CANCELLING;state++)for(bool bound:{false,true}){
   f.id=uint32_t(2+state*2+bound);f.wave();legacy_sync_fixture_t e{};e.id=f.id;e.state=memo_job_state_t(state);e.bound=bound;e.automatic=true;
   if(bound){e.audio.bytes=32044;e.audio.sha256[0]=1;e.target.instance.bytes[0]=1;e.target.url_hash[0]=1;strcpy(e.target.device_id,"synthetic");}
   char cache[256];snprintf(cache,sizeof(cache),"%s/sync_r1_%010u.job",root.c_str(),unsigned(f.id));assert(legacy_fixture_write(cache,&e));auto original=bytes(std::string(cache)+".0");
   f.view();bool allowed=!bound&&(state==MEMO_JOB_LOCAL||state==MEMO_JOB_QUEUED);assert(f.result.can_retry==allowed);assert(submit(root.c_str(),f.id,f.config)==allowed);assert(bytes(std::string(cache)+".0")==original);
  }
  assert(!f.opens);
 }else if(name=="retry"||name=="clock"){
  f.start();f.mode="asr_500";if(name=="clock")f.clock=false;f.finish();assert(f.entry().state==MEMO_JOB_FAILED&&f.entry().asr_used==3);assert(name=="clock"?f.opens==0:f.asr==3);
  f.reboot();f.tick();assert(f.entry().asr_used==3);f.clock=true;f.mode="ok";assert(submit(root.c_str(),1,f.config));f.finish();assert(f.entry().state==MEMO_JOB_SENT);
 }else if(name=="limited"){
  f.mode="asr_429";f.start();f.tick();assert(f.result.delay_ms==90000);f.mode="ok";f.tick();assert(f.asr==1&&f.result.delay_ms>=90000);f.now+=f.result.delay_ms;f.finish();assert(f.entry().state==MEMO_JOB_SENT&&f.asr==2);
 }else if(name=="limited_clock"){
  f.mode="asr_429";f.start();f.tick();f.clock=false;f.tick();assert(f.result.delay_ms>=90000&&f.asr==1);
  f.clock=true;f.mode="ok";f.tick();assert(f.result.delay_ms>=90000&&f.asr==1);f.now+=f.result.delay_ms;f.finish();assert(f.entry().state==MEMO_JOB_SENT&&f.asr==2);
 }else if(name=="clean"||name=="clean_empty"){
  const char *const pairs[][2]={{"你好喵伴，正文。你好喵伴保存录音","正文"},{"  小鹿小鹿：正文。结束录音 ","正文"},{"甲你好喵伴乙停止录音丙","甲你好喵伴乙停止录音丙"},{"\r\n甲\r\n乙\r丙\r\n","甲\n乙\n丙"},{"\xe3\x80\x80喵喵同学，正文。你好喵伴","正文"},{"你好喵伴，停止录音",""},{"你好喵伴。你好喵伴。",""}};
  for(auto &pair:pairs){std::string value;auto sink=[](void *ctx,const char *s,size_t n){static_cast<std::string *>(ctx)->append(s,n);return true;};size_t n=memo_transcript_clean(pair[0],strlen(pair[0]),sink,&value);assert(value==pair[1]&&n==value.size()&&n==memo_transcript_clean(pair[0],strlen(pair[0]),nullptr,nullptr));}
  f.start();f.mode=name;f.tick();assert(f.entry().text_bytes==(name=="clean"?6:0));
  f.finish();assert(f.entry().state==(name=="clean"?MEMO_JOB_SENT:MEMO_JOB_REJECTED));assert(f.posted==(name=="clean"?1:0));
 }else if(name=="limited_boot"){
  f.mode="asr_429";f.start();f.tick();f.mode="ok";f.reboot();f.tick();assert(f.asr==1&&f.result.delay_ms==300000);f.now+=300000;f.finish();assert(f.entry().state==MEMO_JOB_SENT);
 }else if(name=="limited_long"||name=="credentials"||name=="invalid"){
  f.mode=name=="limited_long"?"asr_429_long":name=="credentials"?"asr_401":"asr_bad";f.start();f.finish();assert(f.entry().state==MEMO_JOB_FAILED&&f.asr==1);f.reboot();f.tick();assert(f.asr==1);
 }else if(name=="empty"){
  f.mode="asr_empty";f.start();f.finish();assert(f.entry().state==MEMO_JOB_REJECTED&&!f.posted);assert(submit(root.c_str(),1,f.config));f.mode="ok";f.finish();assert(f.entry().state==MEMO_JOB_SENT&&f.asr==2);
 }else if(name=="uncertain"||name=="lost"){
  f.start();f.tick();f.mode=name=="lost"?"delivery_lost":"delivery_500";f.finish();assert(f.entry().state==MEMO_JOB_UNCERTAIN&&!submit(root.c_str(),1,f.config));int calls=f.opens;f.reboot();f.tick();assert(f.opens==calls);
 }else if(name=="delivery_retry"){
  f.start();f.tick();f.mode="delivery_reject";f.finish();assert(f.entry().state==MEMO_JOB_FAILED&&f.asr==1);assert(submit(root.c_str(),1,f.config));f.mode="ok";f.finish();assert(f.entry().state==MEMO_JOB_SENT&&f.asr==1&&f.posted==2);assert(f.delivery_starts[1]-f.delivery_starts[0]>=10000);
 }else if(name=="crash_asr"){
  f.start();f.mode="crash_asr";f.tick();assert(f.result.error==Error::Storage&&f.entry().next_sample==0&&bytes(f.text()).size()>0);f.mode="ok";f.reboot();f.finish();assert(f.entry().state==MEMO_JOB_SENT&&f.asr==2);
 }else if(name=="crash_delivery"){
  f.start();f.tick();f.mode="crash_delivery";f.tick();assert(f.entry().state==MEMO_JOB_DELIVERING&&f.delivered==1);f.mode="ok";f.reboot();f.tick();assert(f.entry().state==MEMO_JOB_UNCERTAIN&&f.delivered==1&&f.posted==1);
 }else if(name=="cancel_offline"||name=="cancel_asr"){
  auto original=bytes(f.wav());f.start();if(name=="cancel_offline"){f.online=false;assert(cancel(root.c_str(),1));}else f.mode="cancel_asr";
  f.finish();assert(f.entry().state==MEMO_JOB_CANCELLED&&bytes(f.wav())==original&&access(f.text().c_str(),F_OK));f.control=Control::Run;f.online=true;f.mode="ok";assert(submit(root.c_str(),1,f.config));f.finish();assert(f.entry().state==MEMO_JOB_SENT);
 }else if(name=="cancel_success"||name=="cancel_lost"){
  f.start();f.tick();f.mode=name;f.finish();assert(f.entry().state==(name=="cancel_success"?MEMO_JOB_SENT:MEMO_JOB_UNCERTAIN));
 }else if(name=="config"){
  f.start();f.tick();f.config.siyuan_generation++;f.view();assert(f.result.needs_rebind&&f.result.can_cancel);f.tick();assert(f.entry().pause==Pause::Configuration&&f.asr==1&&!f.posted);
  assert(submit(root.c_str(),1,f.config));f.finish();assert(f.entry().state==MEMO_JOB_SENT&&f.asr==1);
 }else if(name=="qwen_config"){
  f.start();f.tick();f.config.qwen_generation++;assert(submit(root.c_str(),1,f.config));f.finish();assert(f.entry().state==MEMO_JOB_SENT&&f.asr==2);
 }else if(name=="failed_rebind"){
  f.start();f.tick();f.mode="delivery_reject";f.finish();f.config.siyuan_generation++;f.view();assert(f.result.needs_rebind&&f.result.can_retry&&!f.result.can_cancel);
  assert(submit(root.c_str(),1,f.config));f.mode="ok";f.finish();assert(f.entry().state==MEMO_JOB_SENT&&f.asr==1);
 }else if(name=="text_limit"){
  f.start();Entry e=f.entry();e.text_bytes=262140;assert(write(root.c_str(),e));raw_write(f.text(),std::vector<uint8_t>(e.text_bytes,'x'));
  f.finish();assert(f.entry().state==MEMO_JOB_FAILED&&f.result.error==Error::TooLarge&&!f.posted&&bytes(f.text()).size()==262140);
 }else if(name=="audio_read_fail"){
  auto original=bytes(f.wav());f.start();f.mode=name;f.finish();assert(f.entry().state==MEMO_JOB_FAILED&&f.entry().error==Error::Audio&&bytes(f.wav())==original&&!f.posted);
 }else if(name=="media_401"||name=="media_301"||name=="media_headers_401"||name=="media_headers_403"||name=="timeout_401"||name=="timeout_403"||name=="timeout_400"){
  f.start();f.mode=name;f.tick();assert(!f.result.pending&&f.entry().state==MEMO_JOB_FAILED&&f.asr==1);
  f.control=Control::Run;f.mode="ok";f.now+=400000;f.tick();assert(f.asr==1&&!f.result.pending);
 }else if(name.rfind("wait_",0)==0){
  f.start();bool reboot=name.find("reboot")!=std::string::npos;
  f.mode="headers_wait_"+name.substr(5,3);f.tick();assert(f.result.pending&&f.entry().limited&&f.entry().pause==Pause::Media&&f.asr==1);
  f.tick();assert(f.asr==1); // Still paused must retain the longer wait.
  if(reboot){f.reboot();}f.control=Control::Run;f.mode="ok";f.tick();
  assert(f.asr==1&&f.result.delay_ms==(reboot?300000U:90000U));f.now+=f.result.delay_ms;f.finish();assert(f.entry().state==MEMO_JOB_SENT&&f.asr==2);
 }else if(name=="id_floor"){
  f.start();assert(!unlink(f.wav().c_str()));memo_store_t store;assert(!memo_store_init(&store,f.root.c_str()));
  uint32_t floor=0;assert(!memo_store_max_id(&store,&floor)&&floor==1);assert(!memo_store_begin_after(&store,0)&&store.id==2);memo_store_abort(&store);
 }else if(name=="headers_disconnect"){
  f.start();f.mode=name;f.tick();
  assert(f.result.pending&&f.entry().pause==Pause::Media&&f.asr==1&&!f.posted);
  f.mode="ok";f.control=Control::Run;f.now+=f.result.delay_ms;
  f.finish();assert(f.entry().state==MEMO_JOB_SENT&&f.asr==2&&f.posted==1);
 }else if(name=="pause"){
  f.start();f.mode="pause_asr";f.tick();assert(f.result.pending&&f.entry().pause==Pause::Mode);f.config.enabled=false;f.reboot();f.tick();assert(f.asr==1);f.config.enabled=true;f.mode="ok";f.finish();assert(f.entry().state==MEMO_JOB_SENT);
 }else if(name=="storage"){
  f.start();drop_filter=".job.";drop_write=1;f.tick();assert(!f.opens&&f.result.error==Error::Storage);
  fail_open=".job.1";f.tick();assert(!f.opens);fail_open.clear();f.mode="text_fail";f.finish();assert(f.entry().state==MEMO_JOB_FAILED&&f.entry().text_bytes==0);
 }else if(name=="cleanup"){
  f.start();f.tick();fail_unlink=1;f.finish();assert(f.entry().state==MEMO_JOB_SENT&&!access(f.text().c_str(),F_OK));uint32_t after=0;bool complete=false;assert(pending(root.c_str(),f.config,&after,&complete)==1&&complete);
  int calls=f.opens;f.tick();assert(access(f.text().c_str(),F_OK)&&f.opens==calls);
 }else if(name=="codec"){
  f.start();auto first=bytes(f.job(0));assert(first.size()==256&&get32(first.data()+4)==1);Entry e=f.entry();int writes=write_calls;assert(write(root.c_str(),e)&&writes==write_calls);e.state=MEMO_JOB_WAITING;assert(write(root.c_str(),e));
  auto future=bytes(f.job(1));put32(future.data()+4,2);put32(future.data()+252,crc(future.data(),252));raw_write(f.job(1),future);assert(read(root.c_str(),1,&e)==MEMO_CACHE_INVALID&&!submit(root.c_str(),1,f.config));f.tick();assert(!f.opens);
 }else if(name=="scan"){
  uint32_t after=0;bool complete=true;assert(!pending((root+"/absent").c_str(),f.config,&after,&complete)&&!complete);
  assert(!pending(root.c_str(),f.config,&after,&complete)&&complete);f.start();assert(pending(root.c_str(),f.config,&after,&complete)==1&&complete);
  // One decode per ID with both slots; a missing or torn .0 still uses .1.
  auto e=f.entry();e.state=MEMO_JOB_WAITING;assert(write(root.c_str(),e));
  size_t before=metadata_reads;assert(pending(root.c_str(),f.config,&after,&complete)==1&&complete&&metadata_reads-before==2);
  assert(!rename(f.job(0).c_str(),(f.job(0)+".backup").c_str()));
  before=metadata_reads;assert(pending(root.c_str(),f.config,&after,&complete)==1&&complete&&metadata_reads-before==1);
  raw_write(f.job(0),{0});before=metadata_reads;
  assert(pending(root.c_str(),f.config,&after,&complete)==1&&complete&&metadata_reads-before==2);
  fail_open=".job.";assert(!pending(root.c_str(),f.config,&after,&complete)&&!complete);fail_open.clear();f.config.enabled=false;assert(!pending(root.c_str(),f.config,&after,&complete)&&complete);
 }else if(name=="scan_partial"){
  f.start();raw_write(root+"/direct_r1_0000000999.job.0",{0});uint32_t after=0;bool complete=true;
  assert(pending(root.c_str(),f.config,&after,&complete)==1&&!complete);f.finish();assert(f.entry().state==MEMO_JOB_SENT);
 }else if(name=="legacy_future"){
  legacy_sync_fixture_t e{};e.id=1;e.state=MEMO_JOB_LOCAL;std::string cache=root+"/sync_r1_0000000001.job";assert(legacy_fixture_write(cache.c_str(),&e));
  auto unknown=bytes(cache+".0");put32(unknown.data()+4,2);put32(unknown.data()+160,crc(unknown.data(),160));raw_write(cache+".1",unknown);
  f.view();assert(f.result.error==Error::Storage&&!f.result.can_retry&&!submit(root.c_str(),1,f.config)&&!f.opens);
 }else if(name=="cycles"){
  for(unsigned n=1;n<=20;n++){if(n>1){f.id=n;f.wave();}f.start();f.mode=n%3==1?"ok":n%3==2?"asr_401":"cancel_asr";f.control=Control::Run;f.finish();assert(!f.leased&&files.empty());}
 }else if(name=="config_blob"){
  memo_cloud_config_t cfg{};assert(memo_cloud_config_read(&cfg)&&!cfg.enabled&&!memo_cloud_config_ready(&cfg));nvs_mode=1;assert(memo_cloud_config_read(&cfg));nvs_mode=2;assert(!memo_cloud_config_read(&cfg));nvs_mode=0;
  nvs_data.assign(604,0);memcpy(nvs_data.data(),"R1CF",4);put32(nvs_data.data()+4,1);put32(nvs_data.data()+8,1);put32(nvs_data.data()+12,1);put32(nvs_data.data()+16,1);
  strcpy(reinterpret_cast<char*>(nvs_data.data()+20),"llm-test");strcpy(reinterpret_cast<char*>(nvs_data.data()+84),"synthetic-key");strcpy(reinterpret_cast<char*>(nvs_data.data()+341),"synthetic-token");put32(nvs_data.data()+600,crc(nvs_data.data(),600));assert(memo_cloud_config_read(&cfg)&&!cfg.enabled&&!cfg.qwen_key[0]&&cfg.siyuan_token[0]&&cfg.qwen_generation==1);
  uint8_t updated[604];cfg.enabled=true;strcpy(cfg.qwen_key,"platform-key");assert(memo_cloud_config_encode(&cfg,updated));nvs_data.assign(updated,updated+sizeof(updated));assert(memo_cloud_config_read(&cfg)&&memo_cloud_config_ready(&cfg));
  nvs_data[90]^=1;assert(!memo_cloud_config_read(&cfg)&&!cfg.qwen_key[0]);assert(!handles);memo_cloud_config_clear(&cfg);
 }else assert(false);
 assert(!f.leased&&files.empty()&&!conflicts&&!handles);
}
int main(int argc,char **argv){assert(argc==3);scenario(argv[1],argv[2]);printf("PASS %s\n",argv[1]);}
