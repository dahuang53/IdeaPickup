#include "memo_sd_sync.h"
#include "memo_transcript.h"
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

namespace memo_sd {
namespace {
constexpr uint32_t TEXT_MAX=262144,SAMPLES=180*16000;
struct Paths {
 char wav[MEMO_PATH_MAX]{},text[MEMO_PATH_MAX]{},legacy[MEMO_PATH_MAX]{};bool valid=false;
 Paths(const char *root,uint32_t id){
  if(!root)return;
  int a=snprintf(wav,sizeof(wav),"%s/memo_%010u.wav",root,unsigned(id));
  int b=snprintf(text,sizeof(text),"%s/direct_r1_%010u.text",root,unsigned(id));
  int c=snprintf(legacy,sizeof(legacy),"%s/sync_r1_%010u.job",root,unsigned(id));
  valid=root&&id&&id<=INT32_MAX&&a>0&&b>0&&c>0&&size_t(a)<sizeof(wav)&&size_t(b)<sizeof(text)&&size_t(c)<sizeof(legacy);
 }
};
struct File {
 FILE *p=nullptr;char *cache=nullptr;
 File(const char *path,const char *mode,bool dma=false):p(fopen(path,mode)){
  // Retain the old SD upload's DMA-safe buffering policy for sequential WAV I/O.
  if(p&&dma){
#ifdef ESP_PLATFORM
   cache=static_cast<char *>(heap_caps_malloc(4096,MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA));
#else
   cache=static_cast<char *>(malloc(4096));
#endif
   if(cache&&setvbuf(p,cache,_IOFBF,4096)){free(cache);cache=nullptr;}
  }
 }
 bool close(){bool ok=true;if(p){FILE *f=p;p=nullptr;ok=fclose(f)==0;}free(cache);cache=nullptr;return ok;}
 ~File(){close();}File(const File &)=delete;
};
bool same_config(const Entry &e,const memo_cloud_config_t &c){return e.qwen_generation==c.qwen_generation&&e.siyuan_generation==c.siyuan_generation;}
bool terminal(const Entry &e){return e.state==MEMO_JOB_SENT||e.state==MEMO_JOB_CANCELLED||e.state==MEMO_JOB_UNCERTAIN||e.state==MEMO_JOB_FAILED||e.state==MEMO_JOB_REJECTED;}
bool cleanup(const char *text){return unlink(text)==0||errno==ENOENT;}
bool text_length(const char *path,uint32_t confirmed){
 File f(path,"r+b");if(!f.p)return confirmed==0&&errno==ENOENT;
 struct stat st{};if(fstat(fileno(f.p),&st)||st.st_size<confirmed||st.st_size>TEXT_MAX)return false;
 if(uint64_t(st.st_size)==confirmed)return true;
 return !fflush(f.p)&&!ftruncate(fileno(f.p),confirmed)&&!fsync(fileno(f.p));
}
bool legacy_allowed(const Paths &p,uint32_t id,memo_job_state_t &state,Error &error){
 memo_sync_entry_t old{};auto r=memo_sync_entry_read(p.legacy,id,&old);
 state=MEMO_JOB_LOCAL;error=Error::None;
 if(r==MEMO_CACHE_MISSING)return true;
 if(r!=MEMO_CACHE_OK){error=Error::Storage;state=MEMO_JOB_FAILED;return false;}
 state=old.state;
 bool allowed=!old.bound&&(old.state==MEMO_JOB_LOCAL||old.state==MEMO_JOB_QUEUED);
 if(!allowed&&state!=MEMO_JOB_SENT)error=Error::Legacy;
 return allowed;
}
void present(const Entry &e,const memo_cloud_config_t &c,Result &r){
 r={};r.direct=true;r.state=e.state;r.error=e.error;r.pending=e.intent;
 r.can_cancel=e.intent&&e.state!=MEMO_JOB_CANCELLING;
 r.needs_rebind=!same_config(e,c)&&e.state!=MEMO_JOB_SENT&&e.state!=MEMO_JOB_UNCERTAIN&&e.state!=MEMO_JOB_CANCELLING;
 r.can_retry=!e.intent&&e.state!=MEMO_JOB_SENT&&e.state!=MEMO_JOB_UNCERTAIN;
 if(r.needs_rebind)r.error=Error::Configuration;
}
void storage_error(Result &r){r.error=Error::Storage;r.can_retry=false;r.can_cancel=false;}
bool save(const char *root,Entry &e,Result &r,const memo_cloud_config_t &c){
 if(!write(root,e)){storage_error(r);return false;}
 present(e,c,r);return true;
}
void fail(const char *root,Entry &e,Error error,Result &r,const memo_cloud_config_t &c){
 e.state=MEMO_JOB_FAILED;e.intent=false;e.pause=Pause::None;e.error=error;save(root,e,r,c);
}
bool cancelled(const char *root,Entry &e,const Paths &p,Result &r,const memo_cloud_config_t &c){
 e.intent=false;e.pause=Pause::None;
 e.state=e.may_deliver?MEMO_JOB_UNCERTAIN:MEMO_JOB_CANCELLED;
 if(!save(root,e,r,c))return false;
 if(e.state==MEMO_JOB_CANCELLED&&!cleanup(p.text)){storage_error(r);return false;}
 return true;
}
void put16(uint8_t *p,uint16_t v){p[0]=v;p[1]=v>>8;}
void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=v>>(8*i);}
struct Segment {
 FILE *file;uint32_t first,count;uint8_t header[44]{};
 Segment(FILE *f,uint32_t begin,uint32_t end):file(f),first(begin),count(end-begin){
  memcpy(header,"RIFF",4);put32(header+4,count*2+36);memcpy(header+8,"WAVEfmt ",8);put32(header+16,16);
  put16(header+20,1);put16(header+22,1);put32(header+24,16000);put32(header+28,32000);put16(header+32,2);put16(header+34,16);
  memcpy(header+36,"data",4);put32(header+40,count*2);
 }
 static int read(void *p,uint64_t offset,void *out,size_t n){
  auto &s=*static_cast<Segment *>(p);auto *bytes=static_cast<uint8_t *>(out);size_t copied=0;
  if(offset<44){copied=std::min<uint64_t>(n,44-offset);memcpy(bytes,s.header+offset,copied);offset+=copied;}
  if(copied<n){if(fseek(s.file,44+int64_t(s.first)*2+offset-44,SEEK_SET))return -1;
   size_t got=fread(bytes+copied,1,n-copied,s.file);if(got!=n-copied)return -1;copied+=got;}
  return int(copied);
 }
 memo_cloud::Source source(){return {this,uint64_t(count)*2+44,read};}
};
/* Same contiguous 180s / last-12s / 20ms absolute-amplitude rule as
 * server-relay/app.py:iter_wav_chunks; only the boundary window is read. */
bool boundary(FILE *f,uint32_t first,uint32_t total,uint32_t &end){
 end=uint32_t(std::min<uint64_t>(uint64_t(first)+SAMPLES,total));if(end==total)return true;
 uint32_t search=std::max(first+16000,end-12*16000);uint64_t best=UINT64_MAX;uint8_t pcm[640];
 if(fseek(f,44+int64_t(search)*2,SEEK_SET))return false;
 for(uint32_t at=search;at+320<=std::min<uint64_t>(uint64_t(first)+SAMPLES,total);at+=320){
  if(fread(pcm,1,sizeof(pcm),f)!=sizeof(pcm))return false;
  uint64_t energy=0;for(unsigned i=0;i<320;i++){int v=int16_t(uint16_t(pcm[i*2])|(uint16_t(pcm[i*2+1])<<8));energy+=v<0?-v:v;}
  if(energy<best){best=energy;end=at+320;}
 }
 return end>first;
}
struct Append {
 const char *path;uint32_t base,added=0;bool failed=false,too_large=false;
 static bool text(void *p,const char *s,size_t n){
  auto &a=*static_cast<Append *>(p);size_t cleaned=memo_transcript_clean(s,n,nullptr,nullptr);
  if(!cleaned)return true;
  size_t separator=a.base?2:0;
  if(cleaned==SIZE_MAX||cleaned+separator>TEXT_MAX-a.base){a.too_large=true;return false;}
  File f(a.path,"r+b");if(!f.p&&errno==ENOENT)f.p=fopen(a.path,"w+b");
  if(!f.p||fseek(f.p,a.base,SEEK_SET)){a.failed=true;return false;}
  bool ok=(!separator||fwrite("\n\n",1,2,f.p)==2)&&memo_transcript_clean(s,n,[](void *ctx,const char *part,size_t len){return fwrite(part,1,len,static_cast<FILE *>(ctx))==len;},f.p)==cleaned;
  if(fflush(f.p)||fsync(fileno(f.p)))ok=false;
  if(!f.close())ok=false;
  if(ok)a.added=uint32_t(cleaned+separator);else a.failed=true;
  return ok;
 }
};
struct Body {
 FILE *file;const char *prefix;size_t prefix_bytes;
 static int read(void *p,uint64_t offset,void *out,size_t n){
  auto &b=*static_cast<Body *>(p);size_t copied=0;auto *bytes=static_cast<char *>(out);
  if(offset<b.prefix_bytes){copied=std::min<uint64_t>(n,b.prefix_bytes-offset);memcpy(bytes,b.prefix+offset,copied);offset+=copied;}
  if(copied<n){if(fseek(b.file,offset-b.prefix_bytes,SEEK_SET))return -1;size_t got=fread(bytes+copied,1,n-copied,b.file);if(got!=n-copied)return -1;copied+=got;}
  return int(copied);
 }
};
Error error_for(memo_cloud::Code code){
 using memo_cloud::Code;
 switch(code){case Code::Credentials:return Error::Credentials;case Code::Limited:return Error::Limited;
 case Code::Response:case Code::Rejected:case Code::Invalid:return Error::Response;
 case Code::TooLarge:return Error::TooLarge;case Code::Memory:return Error::Memory;case Code::Source:return Error::Storage;
 default:return Error::Transport;}
}
}
void view(const char *root,uint32_t id,const memo_cloud_config_t &cfg,Result &r){
 r={};Entry e;auto status=read(root,id,&e);Paths p(root,id);
 if(!p.valid){storage_error(r);return;}
 struct stat file{};
 if(stat(p.wav,&file)||file.st_size<46){r.state=MEMO_JOB_FAILED;r.error=Error::Audio;return;}
 if(status==MEMO_CACHE_OK){
  present(e,cfg,r);
  if(uint64_t(file.st_size)!=e.file_bytes){r.state=MEMO_JOB_FAILED;r.error=Error::Audio;r.can_retry=r.can_cancel=r.needs_rebind=false;}
  else if(e.intent&&(!cfg.enabled||!memo_cloud_config_ready(&cfg)))r.state=MEMO_JOB_WAITING;
  return;
 }
 if(status!=MEMO_CACHE_MISSING){storage_error(r);r.state=MEMO_JOB_FAILED;return;}
 r.can_retry=legacy_allowed(p,id,r.state,r.error);
}
namespace {
bool create_validated(const char *root,uint32_t id,const memo_cloud_config_t &cfg,
                      const Paths &p,const memo_record_t &record){
 memo_job_state_t state;Error error;
 if(!legacy_allowed(p,id,state,error))return false;
 Entry e;e.id=id;e.file_bytes=uint64_t(record.data_bytes)+44;e.submission=1;e.qwen_generation=cfg.qwen_generation;e.siyuan_generation=cfg.siyuan_generation;
 e.state=MEMO_JOB_QUEUED;e.intent=true;return write(root,e);
}
}
bool create(const char *root,uint32_t id,const memo_cloud_config_t &cfg,bool automatic){
 if(!automatic)return true; // Local-only recordings create no cloud intent.
 if(!cfg.enabled||!memo_cloud_config_ready(&cfg))return false;
 Entry current;if(read(root,id,&current)!=MEMO_CACHE_MISSING)return false;
 Paths p(root,id);memo_record_t record{};
 if(!p.valid||memo_store_validate(p.wav,&record)||record.id!=id)return false;
 return create_validated(root,id,cfg,p,record);
}
bool submit(const char *root,uint32_t id,const memo_cloud_config_t &cfg){
 if(!cfg.enabled||!memo_cloud_config_ready(&cfg))return false;
 Entry e;auto status=read(root,id,&e);Paths p(root,id);memo_record_t record{};
 if(!p.valid||memo_store_validate(p.wav,&record)||record.id!=id)return false;
 if(status==MEMO_CACHE_MISSING)return create_validated(root,id,cfg,p,record);
 if(status!=MEMO_CACHE_OK||e.state==MEMO_JOB_SENT||e.state==MEMO_JOB_UNCERTAIN||e.state==MEMO_JOB_CANCELLING||
    (e.intent&&same_config(e,cfg))||e.submission==UINT32_MAX||e.file_bytes!=uint64_t(record.data_bytes)+44)return false;
 bool reset=e.state==MEMO_JOB_CANCELLED||e.error==Error::NoSpeech||e.qwen_generation!=cfg.qwen_generation;
 if(!reset&&!text_length(p.text,e.text_bytes))return false;
 if(reset){
  // Clear metadata BEFORE truncating; a crash cannot leave an old checkpoint
  // claiming text that a new submission has already removed.
  e.next_sample=e.text_bytes=0;e.phase=Phase::Asr;
 }
 e.submission++;e.qwen_generation=cfg.qwen_generation;e.siyuan_generation=cfg.siyuan_generation;
 e.state=MEMO_JOB_QUEUED;e.intent=true;e.pause=Pause::None;e.may_deliver=e.limited=false;e.error=Error::None;
 e.asr_used=e.delivery_used=0;e.http_status=0;e.provider_code[0]=0;
 return write(root,e);
}
bool cancel(const char *root,uint32_t id){
 Entry e;if(read(root,id,&e)!=MEMO_CACHE_OK||!e.intent)return false;
 if(e.state==MEMO_JOB_CANCELLING)return true;
 e.state=MEMO_JOB_CANCELLING;e.pause=Pause::None;return write(root,e);
}
void step(const char *root,uint32_t id,const memo_cloud_config_t &cfg,const IO &io,Runtime &rt,Result &r){
 r={};Entry e;Paths p(root,id);
 if(!p.valid||read(root,id,&e)!=MEMO_CACHE_OK){storage_error(r);return;}
 present(e,cfg,r);
 if(e.state==MEMO_JOB_DELIVERING||e.state==MEMO_JOB_CANCELLING){
  cancelled(root,e,p,r,cfg);rt.forget();return;
 }
 if(terminal(e)){
  if((e.state==MEMO_JOB_SENT||e.state==MEMO_JOB_CANCELLED)&&!cleanup(p.text))storage_error(r);
  rt.forget();return;
 }
 if(!e.intent)return;
 Control control=io.control?io.control(io.context):Control::Run;
 if(control==Control::Cancel){if(cancel(root,id)){read(root,id,&e);cancelled(root,e,p,r,cfg);}else storage_error(r);rt.forget();return;}
 Pause pause=!same_config(e,cfg)?Pause::Configuration:(!cfg.enabled||!memo_cloud_config_ready(&cfg)||control==Control::Pause)?Pause::Mode:control==Control::Media?Pause::Media:Pause::None;
 if(pause!=Pause::None){e.state=MEMO_JOB_WAITING;e.pause=pause;save(root,e,r,cfg);rt.reserved=false;return;}
 if(e.pause!=Pause::None){e.pause=Pause::None;if(!save(root,e,r,cfg))return;}
 if(!io.cloud||!io.now_ms||!io.connected||!io.clock_ready){fail(root,e,Error::Configuration,r,cfg);return;}
 if(!io.connected(io.context))return;
 memo_record_t record{};
 if(memo_store_validate(p.wav,&record)||record.id!=id||uint64_t(record.data_bytes)+44!=e.file_bytes){fail(root,e,Error::Audio,r,cfg);return;}
 if(!text_length(p.text,e.text_bytes)){fail(root,e,Error::Storage,r,cfg);return;}
 const uint32_t total=record.data_bytes/2;
 if(e.next_sample==total&&e.phase==Phase::Asr){
  if(!e.text_bytes){e.state=MEMO_JOB_REJECTED;e.intent=false;e.error=Error::NoSpeech;save(root,e,r,cfg);return;}
  e.phase=Phase::Delivery;e.state=MEMO_JOB_WAITING;e.limited=false;if(!save(root,e,r,cfg))return;
 }
 uint32_t &used=e.phase==Phase::Asr?e.asr_used:e.delivery_used;
 int64_t now=io.now_ms(io.context);
 bool reserved=rt.reserved&&rt.id==id&&rt.submission==e.submission&&rt.next_sample==e.next_sample&&rt.phase==e.phase&&rt.attempt==used;
 if(!reserved){
  if(used>=3){fail(root,e,e.error==Error::None?Error::Transport:e.error,r,cfg);rt.forget();return;}
  uint32_t prior=used;
  bool same_step=rt.id==id&&rt.submission==e.submission&&rt.next_sample==e.next_sample&&rt.phase==e.phase&&rt.attempt==used;
  uint32_t wait=same_step?uint32_t(std::max<int64_t>(0,rt.due_ms-now)):
      prior?(e.limited?300000:e.phase==Phase::Asr?(prior==1?5000:30000):(prior==1?10000:60000)):0;
  used++;e.state=e.phase==Phase::Asr?MEMO_JOB_TRANSCRIBING:MEMO_JOB_DELIVERING;
  // Persist possible delivery only immediately before the HTTP operation,
  // not while waiting for clock/backoff.
  if(e.phase==Phase::Delivery)e.state=MEMO_JOB_WAITING;
  if(!save(root,e,r,cfg))return;
  if(io.progress)io.progress(io.context,e.state);
  rt.id=id;rt.submission=e.submission;rt.next_sample=e.next_sample;rt.phase=e.phase;rt.attempt=used;rt.reserved=true;
  if(!io.clock_ready(io.context)){
   rt.reserved=false;e.state=MEMO_JOB_WAITING;e.error=Error::Transport;
   if(used==3)fail(root,e,e.error,r,cfg);else save(root,e,r,cfg);
   r.delay_ms=std::max<uint32_t>(wait,e.phase==Phase::Asr?(used==1?5000U:30000U):(used==1?10000U:60000U));rt.due_ms=io.now_ms(io.context)+r.delay_ms;return;
  }
  rt.due_ms=io.now_ms(io.context)+wait;
  if(e.phase==Phase::Delivery)rt.due_ms=std::max(rt.due_ms,rt.last_delivery_ms+10000);
 }
 now=io.now_ms(io.context);
 if(now<rt.due_ms){r.delay_ms=uint32_t(std::min<int64_t>(300000,rt.due_ms-now));return;}
 // Cancellation/mode/media may have changed while preparing clock or waiting.
 control=io.control?io.control(io.context):Control::Run;
 if(control!=Control::Run){r.delay_ms=1;return;}
 memo_cloud::Result result;Error local_error=Error::None;
 if(e.phase==Phase::Asr){
  File wav(p.wav,"rb",true);uint32_t end=0;
  if(!wav.p||!boundary(wav.p,e.next_sample,total,end)){fail(root,e,Error::Audio,r,cfg);rt.forget();return;}
  Segment segment(wav.p,e.next_sample,end);Append append{p.text,e.text_bytes};
  result=memo_cloud::file_asr(*io.cloud,cfg.qwen_key,segment.source(),{&append,Append::text});
  control=io.control?io.control(io.context):Control::Run;
  if(control==Control::Cancel){if(!cancel(root,id)||read(root,id,&e)!=MEMO_CACHE_OK)storage_error(r);else cancelled(root,e,p,r,cfg);rt.forget();return;}
  if(result.code==memo_cloud::Code::Ok||result.code==memo_cloud::Code::Empty){
   e.next_sample=end;e.text_bytes+=append.added;e.asr_used=0;e.limited=false;e.error=Error::None;e.http_status=0;e.provider_code[0]=0;e.state=MEMO_JOB_WAITING;
   save(root,e,r,cfg);rt.forget();return;
  }
  if(!text_length(p.text,e.text_bytes)){fail(root,e,Error::Storage,r,cfg);rt.forget();return;}
  if(append.too_large)result.code=memo_cloud::Code::TooLarge;
  else if(append.failed)result.code=memo_cloud::Code::Source;
  else if(result.code==memo_cloud::Code::Source)local_error=Error::Audio;
 }else{
  File text(p.text,"rb");if(!text.p){fail(root,e,Error::Storage,r,cfg);rt.forget();return;}
  char imported[32]{},prefix[160],title[128];time_t t=io.unix_seconds?io.unix_seconds(io.context):0;tm utc{};
  if(!gmtime_r(&t,&utc)||!strftime(imported,sizeof(imported),"%Y-%m-%dT%H:%M:%SZ",&utc)){fail(root,e,Error::Response,r,cfg);rt.forget();return;}
  int prefix_bytes=snprintf(prefix,sizeof(prefix),"**喵伴 #%u** · 云端导入时间 %s\n\n",unsigned(id),imported);
  snprintf(title,sizeof(title),"喵伴语音笔记 #%u",unsigned(id));
  if(prefix_bytes<0||size_t(prefix_bytes)>=sizeof(prefix)||e.text_bytes>TEXT_MAX-size_t(prefix_bytes)){fail(root,e,Error::TooLarge,r,cfg);rt.forget();return;}
  e.state=MEMO_JOB_DELIVERING;e.may_deliver=true;if(!save(root,e,r,cfg)){rt.forget();return;}
  if(io.progress)io.progress(io.context,e.state);
  rt.last_delivery_ms=io.now_ms(io.context);Body body{text.p,prefix,size_t(prefix_bytes)};
  result=memo_cloud::deliver(*io.cloud,cfg.siyuan_token,title,{&body,e.text_bytes+size_t(prefix_bytes),Body::read});
  text.close();
  control=io.control?io.control(io.context):Control::Run;
  if(result.code==memo_cloud::Code::Ok){e.state=MEMO_JOB_SENT;e.intent=false;e.pause=Pause::None;e.error=Error::None;if(save(root,e,r,cfg)&&!cleanup(p.text))storage_error(r);rt.forget();return;}
  if(result.code==memo_cloud::Code::Uncertain){e.state=MEMO_JOB_UNCERTAIN;e.intent=false;e.pause=Pause::None;e.error=Error::Transport;save(root,e,r,cfg);rt.forget();return;}
  e.may_deliver=false;
  if(control==Control::Cancel){cancelled(root,e,p,r,cfg);rt.forget();return;}
 }
 e.error=local_error==Error::None?error_for(result.code):local_error;e.http_status=std::max(0,result.status);snprintf(e.provider_code,sizeof(e.provider_code),"%s",result.provider_code);
 bool retry=e.phase==Phase::Asr?(result.code==memo_cloud::Code::Timeout||result.code==memo_cloud::Code::Transport||result.code==memo_cloud::Code::Tls||result.code==memo_cloud::Code::Limited||result.status==408||result.status>=500):
     (result.code==memo_cloud::Code::Limited||(!result.may_have_sent&&(result.code==memo_cloud::Code::Timeout||result.code==memo_cloud::Code::Transport||result.code==memo_cloud::Code::Tls)));
 e.limited=result.code==memo_cloud::Code::Limited||result.status==429||result.retry_after>0;
 rt.reserved=false;
 // A media/mode preemption may preserve an interrupted request, but must
 // never revive an explicit rejection, exhausted budget or excessive wait.
 bool known_stop=result.status && !(result.status>=200&&result.status<300) &&
     result.status!=408&&result.status!=429&&!(result.status>=500&&result.status<600);
 if(known_stop && (result.status==401||result.status==403))e.error=Error::Credentials;
 bool interrupted=!known_stop&&(control==Control::Pause||control==Control::Media)&&result.code==memo_cloud::Code::Cancelled;
 if(known_stop||(!retry&&!interrupted)||used>=3||result.retry_after>300){fail(root,e,e.error,r,cfg);rt.forget();return;}
 // Preserve header retry facts even when the body was interrupted. The
 // one-owner runtime keeps the exact deadline; the checkpoint flag retains
 // a conservative 300s wait if that runtime is lost or another job runs.
 uint32_t wait=e.phase==Phase::Asr?(used==1?5000:30000):(used==1?10000:60000);
 if(result.retry_after)wait=std::max(wait,result.retry_after*1000);
 rt.due_ms=io.now_ms(io.context)+wait;
 if(control==Control::Pause||control==Control::Media){e.pause=control==Control::Pause?Pause::Mode:Pause::Media;e.state=MEMO_JOB_WAITING;save(root,e,r,cfg);r.delay_ms=wait;return;}
 e.state=MEMO_JOB_WAITING;save(root,e,r,cfg);r.delay_ms=wait;
}
uint32_t pending(const char *root,const memo_cloud_config_t &cfg,uint32_t *after,bool *complete){
 if(complete)*complete=false;
 if(!root||!after)return 0;
 DIR *dir=opendir(root);if(!dir)return 0;
 uint32_t first=0,next=0;bool healthy=true;
 for(;;){
  errno=0;dirent *item=readdir(dir);if(!item){if(errno)healthy=false;break;}
  unsigned long value=0;unsigned slot=0;int end=0;
  if(sscanf(item->d_name,"direct_r1_%10lu.job.%u%n",&value,&slot,&end)!=2||item->d_name[end]||!value||value>INT32_MAX||slot>1)continue;
  uint32_t id=value;
  // Both directory entries name the same checkpoint. If .0 exists, it will
  // drive the one read of both slots; .1 alone still recovers a single slot.
  if(slot==1){
   char primary[MEMO_PATH_MAX];int n=snprintf(primary,sizeof(primary),"%s/direct_r1_%010u.job.0",root,unsigned(id));
   if(n<0||size_t(n)>=sizeof(primary)){healthy=false;continue;}
   struct stat st{};if(!stat(primary,&st))continue;
   if(errno!=ENOENT)healthy=false;
  }
  Entry e;auto status=read(root,id,&e);
  if(status!=MEMO_CACHE_OK){healthy=false;continue;}
  bool work=e.intent&&((cfg.enabled&&memo_cloud_config_ready(&cfg)&&same_config(e,cfg))||e.state==MEMO_JOB_CANCELLING||e.state==MEMO_JOB_DELIVERING);
  if(!work&&(e.state==MEMO_JOB_SENT||e.state==MEMO_JOB_CANCELLED)){
   Paths p(root,id);struct stat s{};if(!stat(p.text,&s))work=true;else if(errno!=ENOENT)healthy=false;
  }
  if(!work)continue;
  if(!first||id<first)first=id;
  if(id>*after&&(!next||id<next))next=id;
 }
 if(closedir(dir))healthy=false;
 if(!next)next=first;
 if(next)*after=next;
 if(complete)*complete=healthy;
 return next;
}
} // namespace memo_sd
