/* Narrow extraction of DONOR fact byte/CRC and monotonically reserved floor. */
#include "memo_record_ids.h"
#include <string.h>
static uint32_t crc(const uint8_t *p,size_t n){uint32_t c=UINT32_MAX;for(size_t i=0;i<n;i++){c^=p[i];for(unsigned b=0;b<8;b++)c=(c>>1)^(0xedb88320U&(0U-(c&1)));}return ~c;}
static uint32_t u32(const uint8_t *p){uint32_t v=0;for(unsigned i=0;i<4;i++)v|=(uint32_t)p[i]<<(i*8);return v;}
static void put(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(i*8));}
bool memo_record_ids_encode_fact(uint32_t v,uint8_t *p,size_t n){if(!p||n!=16||v>MEMO_RECORD_ID_MAX)return false;memcpy(p,"R1ID",4);put(p+4,1);put(p+8,v);put(p+12,crc(p,12));return true;}
bool memo_record_ids_decode_fact(const uint8_t *p,size_t n,uint32_t *v){if(!v)return false;*v=0;if(!p||n!=16||memcmp(p,"R1ID",4)||u32(p+4)!=1||u32(p+12)!=crc(p,12)||u32(p+8)>MEMO_RECORD_ID_MAX)return false;*v=u32(p+8);return true;}
memo_ids_result_t memo_ids_load(memo_ids_t *s,const memo_ids_storage_t *io){
 if(!s||!io||!io->read||!io->write)return MEMO_IDS_IO_ERROR;
 memset(s,0,sizeof(*s));s->storage=*io;uint32_t v=0;memo_ids_result_t r=io->read(io->context,&v);
 if(r==MEMO_IDS_MISSING)r=io->write(io->context,0);
 if(r==MEMO_IDS_OK&&v<=MEMO_RECORD_ID_MAX){s->highwater=v;s->loaded=true;return r;}
 return r==MEMO_IDS_OK?MEMO_IDS_INVALID:r;
}
memo_ids_result_t memo_ids_reserve(memo_ids_t *s,uint32_t floor,uint32_t *id){
 if(!id)return MEMO_IDS_IO_ERROR;
 *id=0;if(!s||!s->storage.read||!s->storage.write)return MEMO_IDS_IO_ERROR;
 if(floor>MEMO_RECORD_ID_MAX)return MEMO_IDS_EXHAUSTED;
 {uint32_t v=0;memo_ids_result_t r=s->storage.read(s->storage.context,&v);
  if(r==MEMO_IDS_MISSING){if(s->highwater)return MEMO_IDS_IO_ERROR;v=0;}
  else if(r!=MEMO_IDS_OK)return r;
  if(v>MEMO_RECORD_ID_MAX)return MEMO_IDS_INVALID;
  if(v>s->highwater)s->highwater=v;
  s->loaded=true;
 }
 uint32_t maximum=s->highwater>floor?s->highwater:floor;if(maximum==MEMO_RECORD_ID_MAX)return MEMO_IDS_EXHAUSTED;
 s->highwater=maximum+1;memo_ids_result_t r=s->storage.write(s->storage.context,s->highwater);
 if(r!=MEMO_IDS_OK){s->loaded=false;return r;}*id=s->highwater;return MEMO_IDS_OK;
}
