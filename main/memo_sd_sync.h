#pragma once
#include "memo_sync_cache.h"
#include "memo_cloud_config.h"
#include "memo_store.h"
#ifdef __cplusplus
#include "memo_cloud.h"
namespace memo_sd {
enum class Phase:uint32_t {Asr,Delivery};
enum class Pause:uint32_t {None,Mode,Configuration,Media};
enum class Control {Run,Cancel,Pause,Media};
enum class Error:uint32_t {None,Storage,Audio,Legacy,Configuration,NoSpeech,Transport,Credentials,Limited,Response,TooLarge,Interrupted,Memory};
struct Entry {
 uint32_t id=0;memo_job_state_t state=MEMO_JOB_LOCAL;
 Phase phase=Phase::Asr;Pause pause=Pause::None;
 bool intent=false,may_deliver=false,limited=false;
 uint32_t submission=0,qwen_generation=0,siyuan_generation=0;
 uint64_t file_bytes=0;
 uint32_t next_sample=0,text_bytes=0,asr_used=0,delivery_used=0;
 Error error=Error::None;int http_status=0;char provider_code[48]{};
};
struct Result {
 memo_job_state_t state=MEMO_JOB_LOCAL;
 bool direct=false,pending=false,can_retry=false,can_cancel=false,needs_rebind=false;
 uint32_t delay_ms=0;Error error=Error::None;
};
/* One owner cache, not one timer/cache per recording. Attempts are consumed
 * durably before clock preparation and any supplier operation. */
struct Runtime {
 uint32_t id=0,submission=0,next_sample=0,attempt=0;
 Phase phase=Phase::Asr;bool reserved=false;
 int64_t due_ms=0,last_delivery_ms=0;
 void forget(){id=0;reserved=false;}
};
struct IO {
 void *context=nullptr;const memo_cloud::IO *cloud=nullptr;
 int64_t (*now_ms)(void *)=nullptr;
 int64_t (*unix_seconds)(void *)=nullptr;
 bool (*connected)(void *)=nullptr;
 bool (*clock_ready)(void *)=nullptr;
 Control (*control)(void *)=nullptr;
 void (*progress)(void *,memo_job_state_t)=nullptr;
};
memo_cache_result_t read(const char *root,uint32_t id,Entry *);
bool write(const char *root,const Entry &);
/* Views never write, hash/read WAV contents or call network APIs. */
void view(const char *root,uint32_t id,const memo_cloud_config_t &,Result &);
bool create(const char *root,uint32_t id,const memo_cloud_config_t &,bool automatic);
bool submit(const char *root,uint32_t id,const memo_cloud_config_t &);
bool cancel(const char *root,uint32_t id);
void step(const char *root,uint32_t id,const memo_cloud_config_t &,const IO &,Runtime &,Result &);
/* Streaming directory scan: direct intents and terminal cleanup only. */
uint32_t pending(const char *root,const memo_cloud_config_t &,uint32_t *after,bool *complete);
} // namespace memo_sd
#endif
