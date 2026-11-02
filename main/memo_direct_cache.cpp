#include "memo_sd_sync.h"
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <climits>
#include <unistd.h>
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#else
#include <mutex>
#endif
namespace memo_sd {
namespace {
constexpr size_t SIZE=256;
#ifdef ESP_PLATFORM
StaticSemaphore_t mutex_storage;SemaphoreHandle_t mutex;
portMUX_TYPE init_mux=portMUX_INITIALIZER_UNLOCKED;
struct Lock {
 bool held;
 Lock(){portENTER_CRITICAL(&init_mux);if(!mutex)mutex=xSemaphoreCreateMutexStatic(&mutex_storage);portEXIT_CRITICAL(&init_mux);held=mutex&&xSemaphoreTake(mutex,portMAX_DELAY)==pdTRUE;}
 ~Lock(){if(held)xSemaphoreGive(mutex);}
};
#else
std::mutex mutex;
struct Lock {bool held=true;Lock(){mutex.lock();}~Lock(){mutex.unlock();}};
#endif
uint32_t u32(const uint8_t *p){uint32_t v=0;for(unsigned i=0;i<4;i++)v|=uint32_t(p[i])<<(8*i);return v;}
uint64_t u64(const uint8_t *p){return uint64_t(u32(p))|(uint64_t(u32(p+4))<<32);}
void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=v>>(8*i);}
void put64(uint8_t *p,uint64_t v){put32(p,v);put32(p+4,v>>32);}
uint32_t crc(const uint8_t *p,size_t n){uint32_t c=UINT32_MAX;while(n--){c^=*p++;for(unsigned i=0;i<8;i++)c=(c>>1)^(0xedb88320U&(0U-(c&1)));}return ~c;}
bool valid(const Entry &e){
 if(!e.id||e.id>INT32_MAX||e.state<MEMO_JOB_LOCAL||e.state>MEMO_JOB_CANCELLING||e.state==MEMO_JOB_CHECKING||e.state==MEMO_JOB_RECEIVED||
    unsigned(e.phase)>1||unsigned(e.pause)>3||unsigned(e.error)>unsigned(Error::Memory)||!e.submission||!e.qwen_generation||!e.siyuan_generation||
    e.file_bytes<46||e.file_bytes>INT32_MAX||((e.file_bytes-44)&1)||e.next_sample>(e.file_bytes-44)/2||
    e.text_bytes>262144||e.asr_used>3||e.delivery_used>3||e.http_status<0||e.http_status>599||!memchr(e.provider_code,0,48))return false;
 if((e.state==MEMO_JOB_SENT||e.state==MEMO_JOB_CANCELLED||e.state==MEMO_JOB_FAILED||e.state==MEMO_JOB_REJECTED||e.state==MEMO_JOB_UNCERTAIN)&&e.intent)return false;
 if(e.state==MEMO_JOB_UNCERTAIN&&!e.may_deliver)return false;
 if(e.state==MEMO_JOB_DELIVERING&&(!e.intent||!e.may_deliver||e.phase!=Phase::Delivery))return false;
 if(e.pause!=Pause::None&&!e.intent)return false;
 return true;
}
void encode(uint8_t *p,const Entry &e,uint64_t g){
 memset(p,0,SIZE);memcpy(p,"R1DC",4);put32(p+4,1);put64(p+8,g);put32(p+16,e.id);put32(p+20,e.state);
 put32(p+24,unsigned(e.phase));put32(p+28,unsigned(e.pause));put32(p+32,(e.intent?1:0)|(e.may_deliver?2:0)|(e.limited?4:0));
 put32(p+36,e.submission);put32(p+40,e.qwen_generation);put32(p+44,e.siyuan_generation);put64(p+48,e.file_bytes);
 put32(p+56,e.next_sample);put32(p+60,e.text_bytes);put32(p+64,e.asr_used);put32(p+68,e.delivery_used);put32(p+72,unsigned(e.error));
 put32(p+76,e.http_status);memcpy(p+80,e.provider_code,48);put32(p+252,crc(p,252));
}
enum {UNSUPPORTED=4};
int slot(const char *root,uint32_t id,unsigned number,Entry &e,uint64_t &g){
 char path[MEMO_PATH_MAX];int n=snprintf(path,sizeof(path),"%s/direct_r1_%010u.job.%u",root,unsigned(id),number);
 if(n<0||size_t(n)>=sizeof(path))return MEMO_CACHE_IO_ERROR;
 FILE *f=fopen(path,"rb");if(!f)return errno==ENOENT?MEMO_CACHE_MISSING:MEMO_CACHE_IO_ERROR;
 uint8_t bytes[SIZE];bool exact=fread(bytes,1,SIZE,f)==SIZE&&fgetc(f)==EOF;bool error=ferror(f);if(fclose(f))error=true;
 if(error)return MEMO_CACHE_IO_ERROR;
 if(!exact||memcmp(bytes,"R1DC",4)||u32(bytes+252)!=crc(bytes,252))return MEMO_CACHE_INVALID;
 if(u32(bytes+4)!=1)return UNSUPPORTED;
 if(u32(bytes+16)!=id||u32(bytes+32)>7)return MEMO_CACHE_INVALID;
 for(size_t i=128;i<252;i++)if(bytes[i])return MEMO_CACHE_INVALID;
 e={};g=u64(bytes+8);e.id=id;e.state=memo_job_state_t(u32(bytes+20));e.phase=Phase(u32(bytes+24));e.pause=Pause(u32(bytes+28));
 auto flags=u32(bytes+32);e.intent=flags&1;e.may_deliver=flags&2;e.limited=flags&4;
 e.submission=u32(bytes+36);e.qwen_generation=u32(bytes+40);e.siyuan_generation=u32(bytes+44);e.file_bytes=u64(bytes+48);
 e.next_sample=u32(bytes+56);e.text_bytes=u32(bytes+60);e.asr_used=u32(bytes+64);e.delivery_used=u32(bytes+68);e.error=Error(u32(bytes+72));
 e.http_status=u32(bytes+76);memcpy(e.provider_code,bytes+80,48);
 return g&&valid(e)?MEMO_CACHE_OK:MEMO_CACHE_INVALID;
}
memo_cache_result_t winner(const char *root,uint32_t id,Entry &e,uint64_t &g,unsigned &which){
 Entry a,b;uint64_t x=0,y=0;int ra=slot(root,id,0,a,x),rb=slot(root,id,1,b,y);
 if(ra==MEMO_CACHE_IO_ERROR||rb==MEMO_CACHE_IO_ERROR)return MEMO_CACHE_IO_ERROR;
 if(ra==UNSUPPORTED||rb==UNSUPPORTED)return MEMO_CACHE_INVALID;
 if(ra==MEMO_CACHE_OK&&rb==MEMO_CACHE_OK&&x==y){uint8_t pa[SIZE],pb[SIZE];encode(pa,a,x);encode(pb,b,y);if(memcmp(pa,pb,SIZE))return MEMO_CACHE_INVALID;}
 if(ra==MEMO_CACHE_OK&&(rb!=MEMO_CACHE_OK||x>=y)){e=a;g=x;which=0;return MEMO_CACHE_OK;}
 if(rb==MEMO_CACHE_OK){e=b;g=y;which=1;return MEMO_CACHE_OK;}
 return ra==MEMO_CACHE_MISSING&&rb==MEMO_CACHE_MISSING?MEMO_CACHE_MISSING:MEMO_CACHE_INVALID;
}
}
memo_cache_result_t read(const char *root,uint32_t id,Entry *e){
 if(!root||!e||!id||id>INT32_MAX)return MEMO_CACHE_INVALID;
 *e={};Lock lock;if(!lock.held)return MEMO_CACHE_IO_ERROR;
 uint64_t generation=0;unsigned which=0;return winner(root,id,*e,generation,which);
}
bool write(const char *root,const Entry &e){
 if(!root||!valid(e))return false;
 Lock lock;if(!lock.held)return false;
 Entry previous;uint64_t generation=0;unsigned which=1;auto r=winner(root,e.id,previous,generation,which);
 if(r!=MEMO_CACHE_OK&&r!=MEMO_CACHE_MISSING)return false;
 uint8_t bytes[SIZE],old[SIZE];encode(bytes,e,generation);encode(old,previous,generation);
 if(r==MEMO_CACHE_OK&&!memcmp(bytes,old,SIZE))return true;
 if(generation==UINT64_MAX)return false;
 unsigned next=r==MEMO_CACHE_OK?1-which:0;encode(bytes,e,generation+1);
 char path[MEMO_PATH_MAX];int n=snprintf(path,sizeof(path),"%s/direct_r1_%010u.job.%u",root,unsigned(e.id),next);
 if(n<0||size_t(n)>=sizeof(path))return false;
 FILE *f=fopen(path,"wb");if(!f)return false;bool ok=fwrite(bytes,1,SIZE,f)==SIZE;
 if(fflush(f)||fsync(fileno(f)))ok=false;
 if(fclose(f))ok=false;
 Entry actual;uint64_t g=0;if(!ok||slot(root,e.id,next,actual,g)!=MEMO_CACHE_OK||g!=generation+1)return false;
 encode(old,actual,g);return !memcmp(bytes,old,SIZE);
}
} // namespace memo_sd
