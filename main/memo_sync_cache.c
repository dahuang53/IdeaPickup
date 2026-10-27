#include "memo_sync_cache.h"
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
/* FatFs rejects a writer while another descriptor reads the same sidecar.
 * Serialize only these tiny metadata operations, never WAV hashing or HTTP. */
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static StaticSemaphore_t metadata_mutex_storage;
static SemaphoreHandle_t metadata_mutex;
static portMUX_TYPE metadata_init_lock=portMUX_INITIALIZER_UNLOCKED;
static bool metadata_lock(void){
 portENTER_CRITICAL(&metadata_init_lock);
 if(!metadata_mutex)metadata_mutex=xSemaphoreCreateMutexStatic(&metadata_mutex_storage);
 portEXIT_CRITICAL(&metadata_init_lock);
 return metadata_mutex && xSemaphoreTake(metadata_mutex,portMAX_DELAY)==pdTRUE;
}
static void metadata_unlock(void){xSemaphoreGive(metadata_mutex);}
#else
static bool metadata_lock(void){return true;}
static void metadata_unlock(void){}
#endif
#define ENTRY_SIZE 164U
static uint32_t crc(const uint8_t *p,size_t n){uint32_t c=UINT32_MAX;for(size_t i=0;i<n;i++){c^=p[i];for(unsigned b=0;b<8;b++)c=(c>>1)^(0xedb88320U&(0U-(c&1)));}return ~c;}
static void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
static uint32_t get32(const uint8_t *p){uint32_t v=0;for(unsigned i=0;i<4;i++)v|=(uint32_t)p[i]<<(8*i);return v;}
static void put64(uint8_t *p,uint64_t v){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(v>>(8*i));}
static uint64_t get64(const uint8_t *p){uint64_t v=0;for(unsigned i=0;i<8;i++)v|=(uint64_t)p[i]<<(8*i);return v;}
static bool nonzero(const void *p,size_t n){const uint8_t *b=p;uint8_t v=0;for(size_t i=0;i<n;i++)v|=b[i];return v!=0;}
bool memo_sync_audio_matches(const memo_audio_identity_t *a,const memo_audio_identity_t *b){return a&&b&&a->bytes==b->bytes&&!memcmp(a->sha256,b->sha256,32);}
bool memo_sync_target_matches(const memo_sync_target_t *a,const memo_sync_target_t *b,bool partial){
 if(!a||!b)return false;
 return (!partial||nonzero(a->instance.bytes,16)?!memcmp(a->instance.bytes,b->instance.bytes,16):true)&&
        (!partial||nonzero(a->url_hash,32)?!memcmp(a->url_hash,b->url_hash,32):true)&&
        (!partial||a->device_id[0]?!strcmp(a->device_id,b->device_id):true);
}
static bool valid(const memo_sync_entry_t *e){
 if(!e||!e->id||(e->id&0x80000000U)||(unsigned)e->state>MEMO_JOB_CANCELLING||!memchr(e->target.device_id,0,40))return false;
 if(e->bound)return e->audio.bytes>=46&&nonzero(e->audio.sha256,32)&&nonzero(e->target.instance.bytes,16)&&nonzero(e->target.url_hash,32)&&e->target.device_id[0];
 return !e->audio.bytes&&!nonzero(e->audio.sha256,32);
}
static void encode(uint8_t *p,const memo_sync_entry_t *e,uint64_t generation){
 memset(p,0,ENTRY_SIZE);memcpy(p,"R1SJ",4);put32(p+4,1);put64(p+8,generation);put32(p+16,e->id);put32(p+20,e->state);
 put32(p+24,(e->automatic?1U:0U)|(e->manual?2U:0U));put32(p+28,e->bound);memcpy(p+32,e->target.instance.bytes,16);memcpy(p+48,e->target.url_hash,32);
 memcpy(p+80,e->target.device_id,40);put64(p+120,e->audio.bytes);memcpy(p+128,e->audio.sha256,32);put32(p+160,crc(p,160));
}
bool memo_sync_entry_equal(const memo_sync_entry_t *a,const memo_sync_entry_t *b){
 if(!a||!b)return false;
 uint8_t left[ENTRY_SIZE],right[ENTRY_SIZE];encode(left,a,1);encode(right,b,1);
 return !memcmp(left,right,sizeof(left));
}
static bool decode(const uint8_t *p,uint32_t id,memo_sync_entry_t *e,uint64_t *g){
 if(memcmp(p,"R1SJ",4)||get32(p+4)!=1||get32(p+16)!=id||get32(p+24)>3||get32(p+28)>1||get32(p+160)!=crc(p,160))return false;
 memset(e,0,sizeof(*e));*g=get64(p+8);e->id=id;e->state=(memo_job_state_t)get32(p+20);e->automatic=(get32(p+24)&1)!=0;e->manual=(get32(p+24)&2)!=0;e->bound=get32(p+28)!=0;
 memcpy(e->target.instance.bytes,p+32,16);memcpy(e->target.url_hash,p+48,32);memcpy(e->target.device_id,p+80,40);
 e->audio.bytes=get64(p+120);memcpy(e->audio.sha256,p+128,32);return *g&&valid(e);
}
static memo_cache_result_t slot(const char *base,unsigned number,uint32_t id,memo_sync_entry_t *e,uint64_t *g){
 char path[MEMO_CONFIG_URL_MAX+64];int n=snprintf(path,sizeof(path),"%s.%u",base,number);if(n<0||(size_t)n>=sizeof(path))return MEMO_CACHE_IO_ERROR;
 FILE *f=fopen(path,"rb");if(!f)return errno==ENOENT?MEMO_CACHE_MISSING:MEMO_CACHE_IO_ERROR;
 uint8_t p[ENTRY_SIZE];bool exact=fread(p,1,sizeof(p),f)==sizeof(p)&&fgetc(f)==EOF;bool io=ferror(f);if(fclose(f))io=true;
 if(io)return MEMO_CACHE_IO_ERROR;
 return exact&&decode(p,id,e,g)?MEMO_CACHE_OK:MEMO_CACHE_INVALID;
}
static memo_cache_result_t winner(const char *p,uint32_t id,memo_sync_entry_t *e,uint64_t *g,unsigned *which){
 memo_sync_entry_t a={0},b={0};uint64_t x=0,y=0;memo_cache_result_t ra=slot(p,0,id,&a,&x),rb=slot(p,1,id,&b,&y);
 if(ra==MEMO_CACHE_IO_ERROR||rb==MEMO_CACHE_IO_ERROR)return MEMO_CACHE_IO_ERROR;
 if(ra==MEMO_CACHE_OK&&rb==MEMO_CACHE_OK&&x==y){uint8_t pa[ENTRY_SIZE],pb[ENTRY_SIZE];encode(pa,&a,x);encode(pb,&b,y);if(memcmp(pa,pb,sizeof(pa)))return MEMO_CACHE_INVALID;}
 if(ra==MEMO_CACHE_OK&&(rb!=MEMO_CACHE_OK||x>=y)){*e=a;*g=x;*which=0;return MEMO_CACHE_OK;}
 if(rb==MEMO_CACHE_OK){*e=b;*g=y;*which=1;return MEMO_CACHE_OK;}
 return ra==MEMO_CACHE_MISSING&&rb==MEMO_CACHE_MISSING?MEMO_CACHE_MISSING:MEMO_CACHE_INVALID;
}
static memo_cache_result_t entry_read_unlocked(const char *p,uint32_t id,memo_sync_entry_t *e){
 if(!p||!e)return MEMO_CACHE_INVALID;
 memset(e,0,sizeof(*e));uint64_t g=0;unsigned which=0;return winner(p,id,e,&g,&which);
}
static bool entry_write_unlocked(const char *p,const memo_sync_entry_t *e){
 if(!p||!valid(e))return false;
 memo_sync_entry_t previous;uint64_t g=0;unsigned which=1;memo_cache_result_t r=winner(p,e->id,&previous,&g,&which);
 if(r==MEMO_CACHE_IO_ERROR)return false;
 if(r==MEMO_CACHE_OK && memo_sync_entry_equal(&previous,e))return true;
 if(g==UINT64_MAX)return false;
 /* An explicit submission may recreate a missing/corrupt cache. */
 unsigned next=r==MEMO_CACHE_OK?1-which:0;uint8_t bytes[ENTRY_SIZE];encode(bytes,e,g+1);
 char path[MEMO_CONFIG_URL_MAX+64];int n=snprintf(path,sizeof(path),"%s.%u",p,next);if(n<0||(size_t)n>=sizeof(path))return false;
 FILE *f=fopen(path,"wb");if(!f)return false;bool ok=fwrite(bytes,1,sizeof(bytes),f)==sizeof(bytes);
 if(fflush(f)||fsync(fileno(f)))ok=false;
 if(fclose(f))ok=false;
 memo_sync_entry_t actual;uint64_t generation=0;
 if(!ok||slot(p,next,e->id,&actual,&generation)!=MEMO_CACHE_OK||generation!=g+1)return false;
 uint8_t readback[ENTRY_SIZE];encode(readback,&actual,generation);return !memcmp(readback,bytes,sizeof(bytes));
}

memo_cache_result_t memo_sync_entry_read(const char *p,uint32_t id,memo_sync_entry_t *e){
 if(!metadata_lock())return MEMO_CACHE_IO_ERROR;
 memo_cache_result_t result=entry_read_unlocked(p,id,e);metadata_unlock();return result;
}
bool memo_sync_entry_write(const char *p,const memo_sync_entry_t *e){
 if(!metadata_lock())return false;
 bool result=entry_write_unlocked(p,e);metadata_unlock();return result;
}
