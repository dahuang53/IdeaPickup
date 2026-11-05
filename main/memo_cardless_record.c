/* Narrow extraction of DONOR's explicit LE fields/checksum, without v1 import. */
#include "memo_cardless_record.h"
#include <string.h>
static uint32_t get(const uint8_t *p){uint32_t v=0;for(unsigned i=0;i<4;i++)v|=(uint32_t)p[i]<<(8*i);return v;}
static uint32_t sum(const uint8_t *p,size_t n){uint32_t h=2166136261U;for(size_t i=0;i<n;i++)h=(h^p[i])*16777619U;return h;}
static bool nz(const uint8_t *p,size_t n){uint8_t v=0;for(size_t i=0;i<n;i++)v|=p[i];return v!=0;}
bool memo_cardless_record_decode(const void *data,size_t n,memo_cardless_record_t *r){
 if(!r)return false;
 memset(r,0,sizeof(*r));if(!data||n!=MEMO_CARDLESS_RECORD_SIZE)return false;const uint8_t *p=data;
 if(memcmp(p,"R1CL",4)||get(p+4)!=1||get(p+32)>7||get(p+224)!=sum(p,224)||nz(p+204,20)||!nz(p+116,16)||!nz(p+132,32)||!p[164]||!memchr(p+164,0,40))return false;
 r->id=get(p+8);r->duration_ms=get(p+12);r->encoded_bytes=get(p+16);r->state=(memo_job_state_t)get(p+20);
 r->interrupted=(get(p+32)&2)!=0;r->may_have_remote_audio=(get(p+32)&4)!=0;memcpy(r->error,p+36,80);
 if((r->id&MEMO_CARDLESS_ID_FLAG)&&MEMO_CARDLESS_DISPLAY_ID(r->id)&&(unsigned)r->state<=MEMO_JOB_CANCELLING&&memchr(r->error,0,80))return true;
 memset(r,0,sizeof(*r));return false;
}

static void put(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
static bool direct_valid(const memo_cardless_record_t *r){return r&&(r->id&MEMO_CARDLESS_ID_FLAG)&&MEMO_CARDLESS_DISPLAY_ID(r->id)&&r->qwen_generation&&r->siyuan_generation&&(unsigned)r->state<=MEMO_JOB_CANCELLING&&r->delivery_attempts<=3&&memchr(r->error,0,80);}
bool memo_cardless_direct_encode(const memo_cardless_record_t *r,uint8_t p[MEMO_CARDLESS_DIRECT_SIZE]){
 if(!p||!direct_valid(r))return false;
 memset(p,0,MEMO_CARDLESS_DIRECT_SIZE);memcpy(p,"R1CN",4);put(p+4,1);put(p+8,r->id);put(p+12,r->duration_ms);put(p+16,r->encoded_bytes);put(p+20,r->state);
 put(p+24,(r->interrupted?1U:0U)|(r->may_have_remote_audio?2U:0U));put(p+28,r->qwen_generation);put(p+32,r->siyuan_generation);put(p+36,r->delivery_attempts);memcpy(p+40,r->error,strlen(r->error));put(p+124,sum(p,124));return true;
}
bool memo_cardless_direct_decode(const void *data,size_t n,memo_cardless_record_t *r){
 if(!r)return false;
 memset(r,0,sizeof(*r));if(!data||n!=MEMO_CARDLESS_DIRECT_SIZE)return false;const uint8_t *p=data;
 if(memcmp(p,"R1CN",4)||get(p+4)!=1||get(p+24)>3||get(p+120)||get(p+124)!=sum(p,124))return false;
 r->id=get(p+8);r->duration_ms=get(p+12);r->encoded_bytes=get(p+16);r->state=(memo_job_state_t)get(p+20);r->interrupted=(get(p+24)&1)!=0;r->may_have_remote_audio=(get(p+24)&2)!=0;r->qwen_generation=get(p+28);r->siyuan_generation=get(p+32);r->delivery_attempts=get(p+36);r->direct=true;memcpy(r->error,p+40,80);
 if(direct_valid(r))return true;
 memset(r,0,sizeof(*r));return false;
}
