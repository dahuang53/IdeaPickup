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
static uint32_t get32(const uint8_t *p){uint32_t v=0;for(unsigned i=0;i<4;i++)v|=(uint32_t)p[i]<<(8*i);return v;}
static uint64_t get64(const uint8_t *p){uint64_t v=0;for(unsigned i=0;i<8;i++)v|=(uint64_t)p[i]<<(8*i);return v;}
static bool nonzero(const void *p,size_t n){const uint8_t *b=p;uint8_t v=0;for(size_t i=0;i<n;i++)v|=b[i];return v!=0;}

/* Validate the original bytes without retaining unused hashes, remote target
 * or intent flags in each current runtime record. No old format is rewritten. */
static bool decode(const uint8_t *p,uint32_t id,memo_sync_entry_t *e,uint64_t *g){
 if(memcmp(p,"R1SJ",4)||get32(p+4)!=1||get32(p+16)!=id||get32(p+24)>3||get32(p+28)>1||get32(p+160)!=crc(p,160))return false;
 if(!id||(id&0x80000000U)||get32(p+20)>MEMO_JOB_CANCELLING||!memchr(p+80,0,40))return false;
 bool bound=get32(p+28)!=0;uint64_t audio_bytes=get64(p+120);
 if(bound){if(audio_bytes<46||!nonzero(p+128,32)||!nonzero(p+32,16)||!nonzero(p+48,32)||!p[80])return false;}
 else if(audio_bytes||nonzero(p+128,32))return false;
 *g=get64(p+8);e->id=id;e->state=(memo_job_state_t)get32(p+20);e->bound=bound;return *g!=0;
}
static memo_cache_result_t slot(const char *base,unsigned number,uint32_t id,memo_sync_entry_t *e,uint64_t *g,uint8_t p[ENTRY_SIZE]){
 char path[305] /* original legacy metadata path bound */;int n=snprintf(path,sizeof(path),"%s.%u",base,number);if(n<0||(size_t)n>=sizeof(path))return MEMO_CACHE_IO_ERROR;
 FILE *f=fopen(path,"rb");if(!f)return errno==ENOENT?MEMO_CACHE_MISSING:MEMO_CACHE_IO_ERROR;
 bool exact=fread(p,1,ENTRY_SIZE,f)==ENTRY_SIZE&&fgetc(f)==EOF;bool io=ferror(f);if(fclose(f))io=true;
 if(io)return MEMO_CACHE_IO_ERROR;
 if(exact&&!memcmp(p,"R1SJ",4)&&get32(p+160)==crc(p,160)&&get32(p+4)!=1)return (memo_cache_result_t)4;
 return exact&&decode(p,id,e,g)?MEMO_CACHE_OK:MEMO_CACHE_INVALID;
}
static memo_cache_result_t entry_read_unlocked(const char *p,uint32_t id,memo_sync_entry_t *e){
 if(!p||!e)return MEMO_CACHE_INVALID;
 memset(e,0,sizeof(*e));memo_sync_entry_t a={0},b={0};uint64_t x=0,y=0;
 uint8_t pa[ENTRY_SIZE],pb[ENTRY_SIZE];
 memo_cache_result_t ra=slot(p,0,id,&a,&x,pa),rb=slot(p,1,id,&b,&y,pb);
 if(ra==MEMO_CACHE_IO_ERROR||rb==MEMO_CACHE_IO_ERROR)return MEMO_CACHE_IO_ERROR;
 if(ra==(memo_cache_result_t)4||rb==(memo_cache_result_t)4)return MEMO_CACHE_INVALID;
 if(ra==MEMO_CACHE_OK&&rb==MEMO_CACHE_OK&&x==y&&memcmp(pa,pb,ENTRY_SIZE))return MEMO_CACHE_INVALID;
 if(ra==MEMO_CACHE_OK&&(rb!=MEMO_CACHE_OK||x>=y)){*e=a;return MEMO_CACHE_OK;}
 if(rb==MEMO_CACHE_OK){*e=b;return MEMO_CACHE_OK;}
 return ra==MEMO_CACHE_MISSING&&rb==MEMO_CACHE_MISSING?MEMO_CACHE_MISSING:MEMO_CACHE_INVALID;
}
memo_cache_result_t memo_sync_entry_read(const char *p,uint32_t id,memo_sync_entry_t *e){
 if(!metadata_lock())return MEMO_CACHE_IO_ERROR;
 memo_cache_result_t result=entry_read_unlocked(p,id,e);metadata_unlock();return result;
}
