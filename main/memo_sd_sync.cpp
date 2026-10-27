#include "memo_sd_sync.h"
#include "cJSON.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <dirent.h>
#include <inttypes.h>
#include <cerrno>
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
 if(!strcmp(s,"cancelled"))return MEMO_JOB_CANCELLED;
 if(!strcmp(s,"received"))return MEMO_JOB_RECEIVED;
 if(!strcmp(s,"transcribing"))return MEMO_JOB_TRANSCRIBING;
 if(!strcmp(s,"sending")||!strcmp(s,"transcribed"))return MEMO_JOB_DELIVERING;
 if(!strcmp(s,"transcription_failed")||!strcmp(s,"delivery_failed"))return MEMO_JOB_FAILED;
 return MEMO_JOB_UNKNOWN;
}
bool terminal(memo_job_state_t s){return s==MEMO_JOB_SENT||s==MEMO_JOB_REJECTED||s==MEMO_JOB_UNCERTAIN;}
bool received(memo_job_state_t s){return s==MEMO_JOB_RECEIVED||s==MEMO_JOB_TRANSCRIBING||s==MEMO_JOB_DELIVERING;}
void control_token(const char *body,char (&token)[33]){
 cJSON *j=cJSON_Parse(body);const auto *value=cJSON_GetObjectItemCaseSensitive(j,"cancel_token");
 if(cJSON_IsString(value)&&strlen(value->valuestring)==32&&strspn(value->valuestring,"0123456789abcdef")==32)
  snprintf(token,sizeof(token),"%s",value->valuestring);
 cJSON_Delete(j);
}
bool receipt(const char *body,memo_sync_entry_t &e,uint64_t local_bytes,memo_job_state_t &s,char (&remote)[32]){
 cJSON *j=cJSON_Parse(body);if(!j)return false;
 auto str=[j](const char *key)->const char *{const auto *v=cJSON_GetObjectItemCaseSensitive(j,key);return cJSON_IsString(v)?v->valuestring:nullptr;};
 const auto *id=cJSON_GetObjectItemCaseSensitive(j,"recording_id"),*bytes=cJSON_GetObjectItemCaseSensitive(j,"audio_bytes");
 const char *uuid=str("server_instance_uuid"),*device=str("device_id"),*hash=str("audio_sha256"),*job=str("status");
 memo_server_uuid_t instance{};
 bool ok=cJSON_IsNumber(id)&&id->valuedouble==e.id&&uuid&&memo_config_uuid_parse(uuid,&instance)&&
  memo_config_uuid_equal(&instance,&e.target.instance)&&device&&!strcmp(device,e.target.device_id)&&job&&strlen(job)<32;
 if(ok){snprintf(remote,sizeof(remote),"%s",job);s=state(job);ok=s!=MEMO_JOB_UNKNOWN;}
 if(ok && s!=MEMO_JOB_CANCELLED){
  memo_audio_identity_t actual{};
  ok=cJSON_IsNumber(bytes)&&bytes->valuedouble>=46&&bytes->valuedouble<=UINT32_MAX&&
   floor(bytes->valuedouble)==bytes->valuedouble&&hash&&strlen(hash)==64;
  if(ok){
   actual.bytes=(uint64_t)bytes->valuedouble;
   for(unsigned i=0;i<32&&ok;i++){
    unsigned value=0;char pair[3]={hash[i*2],hash[i*2+1],0};
    ok=strspn(pair,"0123456789abcdefABCDEF")==2&&sscanf(pair,"%2x",&value)==1;
    actual.sha256[i]=(uint8_t)value;
   }
   if(e.bound)ok=ok&&memo_sync_audio_matches(&e.audio,&actual);
   else if(local_bytes)ok=ok&&actual.bytes==local_bytes;
   if(ok){e.audio=actual;e.bound=true;}
  }
 }
 cJSON_Delete(j);return ok;
}
}
extern "C" void memo_sd_sync_attempt(const char *wav,const char *cache,uint32_t id,const memo_sync_target_t *target,int mode,const memo_sd_sync_io_t *io,memo_sd_sync_result_t *out){
 if(!out)return;
 *out={};out->state=MEMO_JOB_LOCAL;
 auto error=[out](const char *e){snprintf(out->error,sizeof(out->error),"%s",e);};
 if(!wav||!cache||!target||!io||mode<0||mode>4){error("invalid_request");return;}
 memo_record_t record{};memo_sync_entry_t e{},durable{};memo_cache_result_t r;
 {Reader reader(io);if(!reader.held){error("audio_busy");out->pending=true;return;}
  r=memo_sync_entry_read(cache,id,&e);
  if(r==MEMO_CACHE_IO_ERROR){error("sync_metadata_unreadable");return;}
  if(mode!=4 && (memo_store_validate(wav,&record)||record.id!=id)){error("recording_unreadable");return;}
  if(mode==3){
   out->can_retry=true;
   if(r!=MEMO_CACHE_OK)return;
   if(e.bound && e.audio.bytes!=(uint64_t)record.data_bytes+44){error("recording_changed");return;}
   out->state=e.state;out->pending=e.automatic;
   out->can_retry=!e.automatic&&(e.state==MEMO_JOB_FAILED||e.state==MEMO_JOB_REJECTED||e.state==MEMO_JOB_LOCAL||e.state==MEMO_JOB_CANCELLED);
   out->can_cancel=(!terminal(e.state)||(e.state==MEMO_JOB_REJECTED&&e.automatic&&e.manual))&&e.state!=MEMO_JOB_CANCELLED&&e.state!=MEMO_JOB_CANCELLING&&
    (e.automatic||received(e.state)||e.state==MEMO_JOB_WAITING);
   return;
  }
  if(!io->status||!io->upload){error("invalid_request");return;}
  if(mode==0&&(r!=MEMO_CACHE_OK||!e.automatic)){error("no_pending_intent");return;}
  if(mode==4&&r!=MEMO_CACHE_OK){error("no_pending_intent");return;}
  if(mode==4 && (!terminal(e.state)||(e.state==MEMO_JOB_REJECTED&&e.automatic&&e.manual))) {
   if(!e.bound && (e.state==MEMO_JOB_LOCAL || e.state==MEMO_JOB_QUEUED)) {
    e.state=MEMO_JOB_CANCELLED;e.automatic=e.manual=false;
    if(!memo_sync_entry_write(cache,&e)){error("sync_metadata_write_failed");return;}
    out->state=e.state;out->can_retry=true;return;
   }
   e.state=MEMO_JOB_CANCELLING;e.automatic=true;e.manual=false;
   if(!memo_sync_entry_write(cache,&e)){error("sync_metadata_write_failed");return;}
   out->state=e.state;out->pending=true;
  }
  if(r==MEMO_CACHE_OK&&!memo_sync_target_matches(&e.target,target,!e.bound)){
   if(mode!=1){error("different_service");return;}
   e={};r=MEMO_CACHE_MISSING;
  }
  if(r!=MEMO_CACHE_OK){e.id=id;e.state=MEMO_JOB_LOCAL;}
  if(!e.bound)e.target=*target;
  if(!memo_sync_entry_write(cache,&e)){error("sync_metadata_write_failed");return;}
  durable=e;
 }
 auto present=[&](){out->state=e.state;out->pending=e.automatic;out->can_retry=!e.automatic&&(e.state==MEMO_JOB_FAILED||e.state==MEMO_JOB_REJECTED||e.state==MEMO_JOB_LOCAL||e.state==MEMO_JOB_CANCELLED);out->can_cancel=e.automatic&&e.state!=MEMO_JOB_CANCELLING;};
 auto save=[&](memo_job_state_t s,bool pending,bool manual=false){
  Reader reader(io);if(!reader.held){error("audio_busy");out->pending=true;return false;}
  memo_sync_entry_t current{};
  if(memo_sync_entry_read(cache,id,&current)!=MEMO_CACHE_OK||!memo_sync_entry_equal(&current,&durable)){
   error("recording_changed");return false;
  }
  e.state=s;e.automatic=pending;e.manual=manual;
  if(!memo_sync_entry_write(cache,&e)){error("sync_metadata_write_failed");return false;}
  durable=e;present();return true;
 };
 present();
 if(terminal(e.state) && !(e.state==MEMO_JOB_REJECTED && (mode==1||e.manual)))return;
 char body[2048]{},name[32]{},cancel_token[33]{};memo_job_state_t remote=MEMO_JOB_UNKNOWN;
 const uint64_t local_bytes=record.id?(uint64_t)record.data_bytes+44:0;
 if(mode==4||e.state==MEMO_JOB_CANCELLING){
  if(e.state==MEMO_JOB_CANCELLED)return;
  if(!e.bound&&(e.state==MEMO_JOB_LOCAL||e.state==MEMO_JOB_QUEUED)){
   save(MEMO_JOB_CANCELLED,false);return;
  }
  if(!save(MEMO_JOB_CANCELLING,true))return;
  int code=io->status(io->context,id,body,sizeof(body));
  if(code==200){
   if(!receipt(body,e,local_bytes,remote,name)){error("cancel_pending");return;}
   if(remote==MEMO_JOB_CANCELLED){save(MEMO_JOB_CANCELLED,false);return;}
   control_token(body,cancel_token);
  }else if(code!=404){error("cancel_pending");return;}
  body[0]=0;code=io->cancel?io->cancel(io->context,id,cancel_token,body,sizeof(body)):0;
  if(code==200&&receipt(body,e,local_bytes,remote,name)&&remote==MEMO_JOB_CANCELLED){
   save(MEMO_JOB_CANCELLED,false);return;
  }
  if(code==409){
   body[0]=0;code=io->status(io->context,id,body,sizeof(body));
   if(code==200&&receipt(body,e,local_bytes,remote,name)){
    if(save(remote,received(remote)))error("cancel_too_late");
    return;
   }
  }
  error("cancel_pending");return;
 }
 if(e.state==MEMO_JOB_CANCELLED&&mode!=1&&!e.manual)return;
 const bool already_submitted=e.state==MEMO_JOB_WAITING;
 int code=io->status(io->context,id,body,sizeof(body));
 if(code==200){
  if(!receipt(body,e,local_bytes,remote,name)){error("audio_identity_mismatch");save(MEMO_JOB_UNCERTAIN,false);return;}
  if(mode==1||e.manual){
   control_token(body,cancel_token);
   if(remote==MEMO_JOB_CANCELLED && !cancel_token[0]){error("cancel_pending");return;}
  }
  if(terminal(remote) && !(remote==MEMO_JOB_REJECTED && (mode==1||e.manual)&&!already_submitted)){save(remote,false);return;}
  if(received(remote)){save(remote,true);return;}
  if(remote==MEMO_JOB_CANCELLED&&mode!=1&&!e.manual){save(remote,false);return;}
  if(remote==MEMO_JOB_FAILED&&(already_submitted||(mode!=1&&!e.manual))){save(remote,false);return;}
  if((mode==1||e.manual)&&(remote==MEMO_JOB_FAILED||remote==MEMO_JOB_REJECTED)){
   // Establish a receipt distinct from the old failure before a deliberate retry.
   // Lost cancellation ACK leaves the existing manual intent; a subsequent GET
   // sees either that old failure or the durable cancellation, never guesses.
   body[0]=0;code=io->cancel?io->cancel(io->context,id,cancel_token,body,sizeof(body)):0;
   if(code!=200||!receipt(body,e,local_bytes,remote,name)||remote!=MEMO_JOB_CANCELLED){error("status_query_failed");return;}
   control_token(body,cancel_token);
   if(!cancel_token[0]){error("cancel_pending");return;}
  }
  cJSON *j=cJSON_Parse(body);
  const bool delivery=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j,"delivery_retry_available"));
  cJSON_Delete(j);
  if((mode==1||e.manual)&&remote==MEMO_JOB_CANCELLED&&delivery){
   if(!io->retry){error("delivery_retry_unavailable");return;}
   if(!save(MEMO_JOB_WAITING,true,true))return;
   body[0]=0;code=io->retry(io->context,id,cancel_token,body,sizeof(body));
   if((code==200||code==202)&&receipt(body,e,local_bytes,remote,name))save(remote,!terminal(remote));
   else if(code<=0||code==409||code==429||code>=500)error("status_query_failed");
   else if(code==401||code==403)error("credentials_invalid");
   else {save(MEMO_JOB_FAILED,false);error("retry_failed");}
   return;
  }
 }else if(code==404){
  // A known accepted job cannot be recreated merely because its GET disappeared.
  if(received(e.state)){
   save(MEMO_JOB_UNCERTAIN,false);error("server_task_missing");return;
  }
 }else {error(code==401||code==403?"credentials_invalid":"status_query_failed");return;}
 if(mode==2)return;
 // Persist the same task/target before sending, including a lost first response.
 if(!save(MEMO_JOB_WAITING,true,cancel_token[0]))return;
 body[0]=0;code=io->upload(io->context,&record,cancel_token,body,sizeof(body));
 if((code==200||code==201||code==202)&&receipt(body,e,local_bytes,remote,name))save(remote,!terminal(remote)&&remote!=MEMO_JOB_CANCELLED);
 else if(code<=0){error("status_query_failed");}
 else if(code==409){
  body[0]=0;code=io->status(io->context,id,body,sizeof(body));
  if(code==200&&receipt(body,e,local_bytes,remote,name)){
   if(remote==MEMO_JOB_CANCELLED && cancel_token[0]){save(MEMO_JOB_WAITING,true,true);error("status_query_failed");}
   else save(remote,received(remote));
  }
  else {save(MEMO_JOB_UNCERTAIN,false);error("audio_identity_mismatch");}
 }else if(code==401||code==403||code==429||code>=500){error(code==401||code==403?"credentials_invalid":"upload_failed");}
 else {save(MEMO_JOB_FAILED,false);error("upload_failed");}
}

extern "C" uint32_t memo_sd_sync_pending(const char *root,const memo_sync_target_t *target,uint32_t *after,bool *manual,bool *complete){
 if(complete)*complete=false;
 if(!root||!target||!after||!manual)return 0;
 *manual=false;uint32_t first=0,next=0;DIR *dir=opendir(root);if(!dir)return 0;
 struct dirent *item;bool read_ok=true;
 for(;;){
  errno=0;item=readdir(dir);if(!item){if(errno)read_ok=false;break;}
  unsigned long value=0;unsigned number=0;int end=0;
  if(sscanf(item->d_name,"sync_r1_%10lu.job.%u%n",&value,&number,&end)!=2||item->d_name[end]||!value||value>0x7fffffffUL||number>1)continue;
  uint32_t id=(uint32_t)value;char cache[MEMO_PATH_MAX],wav[MEMO_PATH_MAX];
  int a=snprintf(cache,sizeof(cache),"%s/sync_r1_%010" PRIu32 ".job",root,id),b=snprintf(wav,sizeof(wav),"%s/memo_%010" PRIu32 ".wav",root,id);
  if(a<0||b<0||(size_t)a>=sizeof(cache)||(size_t)b>=sizeof(wav))continue;
  memo_sync_entry_t e{};memo_record_t record{};
  memo_cache_result_t cached=memo_sync_entry_read(cache,id,&e);
  if(cached==MEMO_CACHE_IO_ERROR){read_ok=false;continue;}
  if(cached!=MEMO_CACHE_OK||!e.automatic||e.state==MEMO_JOB_SENT||(e.state==MEMO_JOB_REJECTED&&!e.manual)||e.state==MEMO_JOB_UNCERTAIN||!memo_sync_target_matches(&e.target,target,!e.bound))continue;
  if(memo_store_validate(wav,&record)) {
   if(errno!=ENOENT && errno!=EINVAL)read_ok=false;
   continue;
  }
  if(!first||id<first)first=id;
  if(id>*after&&(!next||id<next))next=id;
 }
 if(closedir(dir))read_ok=false;
 if(!read_ok)return 0;
 if(!next)next=first;
 if(next){char cache[MEMO_PATH_MAX];snprintf(cache,sizeof(cache),"%s/sync_r1_%010" PRIu32 ".job",root,next);memo_sync_entry_t e{};
  if(memo_sync_entry_read(cache,next,&e)!=MEMO_CACHE_OK)return 0;
  *manual=e.manual;*after=next;
 }
 if(complete)*complete=true;
 return next;
}
