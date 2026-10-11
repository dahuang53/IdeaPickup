#include "memo_sd_sync.h"
#include "cJSON.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <dirent.h>
#include <inttypes.h>
namespace {
class Reader {
 const memo_sd_sync_io_t *io;
public:
 bool held;
 explicit Reader(const memo_sd_sync_io_t *p):io(p),held(!p->begin_io||p->begin_io(p->context)){}
 ~Reader(){if(held&&io->end_io)io->end_io(io->context);}
};
memo_job_state_t state(const char *s){
 if(!strcmp(s,"sent"))return MEMO_JOB_SENT;
 if(!strcmp(s,"transcription_rejected"))return MEMO_JOB_REJECTED;
 if(!strcmp(s,"uncertain"))return MEMO_JOB_UNCERTAIN;
 if(!strcmp(s,"received"))return MEMO_JOB_RECEIVED;
 if(!strcmp(s,"transcribing"))return MEMO_JOB_TRANSCRIBING;
 if(!strcmp(s,"sending")||!strcmp(s,"transcribed"))return MEMO_JOB_DELIVERING;
 if(!strcmp(s,"transcription_failed")||!strcmp(s,"delivery_failed"))return MEMO_JOB_FAILED;
 return MEMO_JOB_UNKNOWN;
}
bool receipt(const char *body,const memo_sync_entry_t &e,memo_job_state_t &s,char (&remote)[32]){
 cJSON *j=cJSON_Parse(body);if(!j)return false;
 auto str=[j](const char *key)->const char *{const auto *v=cJSON_GetObjectItemCaseSensitive(j,key);return cJSON_IsString(v)?v->valuestring:nullptr;};
 const auto *id=cJSON_GetObjectItemCaseSensitive(j,"recording_id"),*bytes=cJSON_GetObjectItemCaseSensitive(j,"audio_bytes");
 const char *uuid=str("server_instance_uuid"),*device=str("device_id"),*hash=str("audio_sha256"),*job=str("status");
 memo_server_uuid_t instance{};char hex[65];for(unsigned i=0;i<32;i++)snprintf(hex+i*2,3,"%02x",e.audio.sha256[i]);
 bool ok=cJSON_IsNumber(id)&&id->valuedouble==e.id&&cJSON_IsNumber(bytes)&&bytes->valuedouble==e.audio.bytes&&
  uuid&&memo_config_uuid_parse(uuid,&instance)&&memo_config_uuid_equal(&instance,&e.target.instance)&&device&&!strcmp(device,e.target.device_id)&&
  hash&&!strcmp(hash,hex)&&job&&strlen(job)<32;
 if(ok){snprintf(remote,sizeof(remote),"%s",job);s=state(job);ok=s!=MEMO_JOB_UNKNOWN;}
 cJSON_Delete(j);return ok;
}
}
extern "C" void memo_sd_sync_attempt(const char *wav,const char *cache,uint32_t id,const memo_sync_target_t *target,int mode,const memo_sd_sync_io_t *io,memo_sd_sync_result_t *out){
 if(!out)return;
 *out={};out->state=MEMO_JOB_LOCAL;
 auto error=[out](const char *e){snprintf(out->error,sizeof(out->error),"%s",e);};
 if(!wav||!cache||!target||!io||!io->hash_file||!io->status||!io->upload||mode<0||mode>2){error("invalid_request");return;}
 memo_record_t record{};memo_audio_identity_t audio{};memo_sync_entry_t e{};memo_cache_result_t r;
 {Reader reader(io);if(!reader.held){error("audio_busy");out->pending=true;return;}
 if(memo_store_validate(wav,&record)||record.id!=id||!io->hash_file(io->context,wav,&audio)||audio.bytes!=(uint64_t)record.data_bytes+44){error("recording_unreadable");return;}
 r=memo_sync_entry_read(cache,id,&e);
 if(r==MEMO_CACHE_IO_ERROR){error("sync_metadata_unreadable");return;}
 if(mode==0&&(r!=MEMO_CACHE_OK||!e.automatic)){error("no_pending_intent");return;}
 if(r==MEMO_CACHE_OK&&e.bound&&!memo_sync_audio_matches(&e.audio,&audio)){e.automatic=e.manual=false;memo_sync_entry_write(cache,&e);error("recording_changed");return;}
 if(r==MEMO_CACHE_OK&&!memo_sync_target_matches(&e.target,target,!e.bound)){
  if(mode!=1){error("different_service");return;}
  e={};r=MEMO_CACHE_MISSING;
 }
 if(r!=MEMO_CACHE_OK||!e.bound){
  const bool registered=r==MEMO_CACHE_OK&&e.automatic,manual_intent=r==MEMO_CACHE_OK&&e.manual;
  e.id=id;e.target=*target;e.audio=audio;e.bound=true;e.state=MEMO_JOB_LOCAL;e.automatic=mode==2?registered:true;e.manual=mode==2?manual_intent:false;
  if(!memo_sync_entry_write(cache,&e)){error("sync_metadata_write_failed");return;}
 }
 }
 out->state=e.state;
 if(e.state==MEMO_JOB_SENT||e.state==MEMO_JOB_REJECTED||e.state==MEMO_JOB_UNCERTAIN){return;}
 char body[2048]{};int code=io->status(io->context,id,body,sizeof(body));
 auto unchanged=[&](){Reader reader(io);if(!reader.held)return false;memo_audio_identity_t current{};return io->hash_file(io->context,wav,&current)&&memo_sync_audio_matches(&audio,&current);};
 auto save=[&](memo_job_state_t s,bool pending,bool retry,bool preserve_intent=false){
  Reader reader(io);if(!reader.held){error("audio_busy");out->pending=true;return;}
  memo_audio_identity_t current{};if(!io->hash_file(io->context,wav,&current)||!memo_sync_audio_matches(&audio,&current)){out->state=MEMO_JOB_LOCAL;error("recording_changed");return;}
  memo_sync_entry_t stored{};
  if(memo_sync_entry_read(cache,id,&stored)!=MEMO_CACHE_OK||!stored.bound||!memo_sync_audio_matches(&e.audio,&stored.audio)||!memo_sync_target_matches(&e.target,&stored.target,false)){out->state=MEMO_JOB_LOCAL;error("recording_changed");return;}
  e.state=s;if(!preserve_intent){e.automatic=pending;e.manual=false;}
  if(!memo_sync_entry_write(cache,&e)){error("sync_metadata_write_failed");out->state=MEMO_JOB_LOCAL;return;}
  out->state=s;out->pending=e.automatic;out->can_retry=retry;
 };
 auto stop_intent=[&](){
  Reader reader(io);memo_sync_entry_t stored{};memo_audio_identity_t current{};
  if(reader.held&&io->hash_file(io->context,wav,&current)&&memo_sync_audio_matches(&audio,&current)&&memo_sync_entry_read(cache,id,&stored)==MEMO_CACHE_OK&&stored.bound&&memo_sync_audio_matches(&e.audio,&stored.audio)&&memo_sync_target_matches(&e.target,&stored.target,false)){e.automatic=e.manual=false;memo_sync_entry_write(cache,&e);}
  out->state=MEMO_JOB_LOCAL;error("audio_identity_mismatch");
 };
 memo_job_state_t remote=MEMO_JOB_UNKNOWN;char name[32]{};
 if(code==200){
  if(!receipt(body,e,remote,name)){stop_intent();return;}
  if(remote==MEMO_JOB_SENT||remote==MEMO_JOB_REJECTED||remote==MEMO_JOB_UNCERTAIN){save(remote,false,false);return;}
  if(remote!=MEMO_JOB_FAILED){save(remote,mode==2?e.automatic:true,false,mode==2);return;}
  if(mode!=1){save(remote,mode==2?e.automatic:false,true,mode==2);return;}
  if(!strcmp(name,"delivery_failed")){
   if(!io->retry){save(remote,false,false);error("delivery_retry_unavailable");return;}
   if(!unchanged()){out->state=MEMO_JOB_LOCAL;error("recording_changed");return;}
   body[0]=0;code=io->retry(io->context,id,body,sizeof(body));
   if((code==200||code==202)&&receipt(body,e,remote,name))save(remote,true,false);
   else if(code==0){save(MEMO_JOB_WAITING,true,false);}
   else if(code==409){body[0]=0;code=io->status(io->context,id,body,sizeof(body));if(code==200&&receipt(body,e,remote,name))save(remote,remote==MEMO_JOB_RECEIVED||remote==MEMO_JOB_TRANSCRIBING||remote==MEMO_JOB_DELIVERING,remote==MEMO_JOB_FAILED);else error("retry_failed");}
   else {save(MEMO_JOB_FAILED,false,true);error(code==401?"credentials_invalid":"retry_failed");}
   return;
  }
 }else if(code!=404){error(code==401||code==403?"credentials_invalid":"status_query_failed");out->can_retry=e.state==MEMO_JOB_FAILED;return;}
 if(mode==2){save(e.automatic?MEMO_JOB_QUEUED:MEMO_JOB_LOCAL,e.automatic,true,true);return;}
 if(!unchanged()){out->state=MEMO_JOB_LOCAL;error("recording_changed");return;}
 // The durable binding remains intact across a lost upload response/restart.
 body[0]=0;code=io->upload(io->context,&record,&audio,body,sizeof(body));
 if((code==200||code==201||code==202)&&receipt(body,e,remote,name))save(remote,remote!=MEMO_JOB_SENT&&remote!=MEMO_JOB_REJECTED&&remote!=MEMO_JOB_UNCERTAIN,false);
 else if(code==0){save(MEMO_JOB_WAITING,true,false);}
 else if(code==409){body[0]=0;code=io->status(io->context,id,body,sizeof(body));if(code==200&&receipt(body,e,remote,name))save(remote,remote==MEMO_JOB_RECEIVED||remote==MEMO_JOB_TRANSCRIBING||remote==MEMO_JOB_DELIVERING,false);else stop_intent();}
 else {save(MEMO_JOB_FAILED,false,true);error(code==401||code==403?"credentials_invalid":"upload_failed");}
}

extern "C" uint32_t memo_sd_sync_pending(const char *root,const memo_sync_target_t *target,uint32_t *after,bool *manual){
 if(!root||!target||!after||!manual)return 0;
 *manual=false;uint32_t first=0,next=0;DIR *dir=opendir(root);if(!dir)return 0;
 struct dirent *item;
 while((item=readdir(dir))){
  unsigned long value=0;unsigned number=0;int end=0;
  if(sscanf(item->d_name,"sync_r1_%10lu.job.%u%n",&value,&number,&end)!=2||item->d_name[end]||!value||value>0x7fffffffUL||number>1)continue;
  uint32_t id=(uint32_t)value;char cache[MEMO_PATH_MAX],wav[MEMO_PATH_MAX];
  int a=snprintf(cache,sizeof(cache),"%s/sync_r1_%010" PRIu32 ".job",root,id),b=snprintf(wav,sizeof(wav),"%s/memo_%010" PRIu32 ".wav",root,id);
  if(a<0||b<0||(size_t)a>=sizeof(cache)||(size_t)b>=sizeof(wav))continue;
  memo_sync_entry_t e{};memo_record_t record{};
  if(memo_sync_entry_read(cache,id,&e)!=MEMO_CACHE_OK||!e.automatic||e.state==MEMO_JOB_SENT||e.state==MEMO_JOB_REJECTED||e.state==MEMO_JOB_UNCERTAIN||!memo_sync_target_matches(&e.target,target,!e.bound)||memo_store_validate(wav,&record))continue;
  if(!first||id<first)first=id;
  if(id>*after&&(!next||id<next))next=id;
 }
 closedir(dir);if(!next)next=first;
 if(next){char cache[MEMO_PATH_MAX];snprintf(cache,sizeof(cache),"%s/sync_r1_%010" PRIu32 ".job",root,next);memo_sync_entry_t e{};
  if(memo_sync_entry_read(cache,next,&e)!=MEMO_CACHE_OK)return 0;
  *manual=e.manual;*after=next;
 }
 return next;
}
