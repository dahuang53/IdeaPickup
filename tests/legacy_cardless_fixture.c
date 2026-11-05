/* Old records1 producer for synthetic compatibility fixtures only. */
/* Narrow extraction of DONOR's explicit LE fields/checksum, without v1 import. */
#include "legacy_cardless_fixture.h"
#include <string.h>
static void put(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
static uint32_t sum(const uint8_t *p,size_t n){uint32_t h=2166136261U;for(size_t i=0;i<n;i++)h=(h^p[i])*16777619U;return h;}
static bool nz(const uint8_t *p,size_t n){uint8_t v=0;for(size_t i=0;i<n;i++)v|=p[i];return v!=0;}
static bool valid(const legacy_cardless_fixture_t *r){return r&&(r->id&MEMO_CARDLESS_ID_FLAG)&&MEMO_CARDLESS_DISPLAY_ID(r->id)&&
 (unsigned)r->state<=MEMO_JOB_CANCELLING&&memchr(r->error,0,80)&&nz(r->target.instance.bytes,16)&&nz(r->target.url_hash,32)&&r->target.device_id[0]&&memchr(r->target.device_id,0,40);}
bool memo_cardless_record_encode(const legacy_cardless_fixture_t *r,uint8_t p[MEMO_CARDLESS_RECORD_SIZE]){
 if(!p||!valid(r))return false;
 memset(p,0,MEMO_CARDLESS_RECORD_SIZE);memcpy(p,"R1CL",4);put(p+4,1);put(p+8,r->id);put(p+12,r->duration_ms);put(p+16,r->encoded_bytes);put(p+20,r->state);
 put(p+24,(uint32_t)r->received_samples);put(p+28,(uint32_t)(r->received_samples>>32));put(p+32,(r->can_retry?1U:0U)|(r->interrupted?2U:0U)|(r->may_have_remote_audio?4U:0U));
 memcpy(p+36,r->error,strlen(r->error));memcpy(p+116,r->target.instance.bytes,16);memcpy(p+132,r->target.url_hash,32);memcpy(p+164,r->target.device_id,strlen(r->target.device_id));put(p+224,sum(p,224));return true;
}
