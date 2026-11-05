#include "memo_record_ids.h"
#include "legacy_cardless_fixture.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
typedef struct {uint32_t value;bool exists;int fault;unsigned writes;} disk_t;
static memo_ids_result_t read(void *p,uint32_t *v){disk_t *d=p;if(d->fault==3)return MEMO_IDS_IO_ERROR;if(d->fault==4)return MEMO_IDS_INVALID;if(!d->exists)return MEMO_IDS_MISSING;*v=d->value;return MEMO_IDS_OK;}
static memo_ids_result_t write(void *p,uint32_t v){disk_t *d=p;d->writes++;if(d->fault==1)return MEMO_IDS_IO_ERROR;d->value=v;d->exists=true;return d->fault==2?MEMO_IDS_IO_ERROR:MEMO_IDS_OK;}
static void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=v>>(8*i);}
static uint32_t checksum(const uint8_t *p,size_t n){uint32_t h=2166136261U;for(size_t i=0;i<n;i++)h=(h^p[i])*16777619U;return h;}
int main(void){
 disk_t d={0};memo_ids_storage_t io={&d,read,write};memo_ids_t ids;uint32_t id=99;
 assert(memo_ids_load(&ids,&io)==MEMO_IDS_OK&&d.writes==1);
 for(unsigned i=1;i<=10;i++){assert(memo_ids_reserve(&ids,0,&id)==MEMO_IDS_OK&&id==i);}
 memo_ids_t reboot;assert(memo_ids_load(&reboot,&io)==MEMO_IDS_OK);
 assert(memo_ids_reserve(&reboot,0,&id)==MEMO_IDS_OK&&id==11); // empty replacement SD
 assert(memo_ids_reserve(&reboot,50,&id)==MEMO_IDS_OK&&id==51); // existing names never overwritten
 assert(memo_ids_reserve(&reboot,0,&id)==MEMO_IDS_OK&&id==52); // cardless shares low-31 bit allocator
 for(int fault=1;fault<=2;fault++){
  d.fault=fault;assert(memo_ids_reserve(&reboot,0,&id)==MEMO_IDS_IO_ERROR&&id==0);
  d.fault=3;assert(memo_ids_reserve(&reboot,0,&id)==MEMO_IDS_IO_ERROR&&id==0);
  uint32_t attempted=reboot.highwater;d.fault=0;assert(memo_ids_reserve(&reboot,0,&id)==MEMO_IDS_OK&&id>attempted);
 }
 d.fault=4;unsigned writes=d.writes;assert(memo_ids_load(&reboot,&io)==MEMO_IDS_INVALID);assert(memo_ids_reserve(&reboot,0,&id)==MEMO_IDS_INVALID&&id==0&&d.writes==writes);
 d.fault=0;d.value=MEMO_RECORD_ID_MAX;assert(memo_ids_load(&reboot,&io)==MEMO_IDS_OK);assert(memo_ids_reserve(&reboot,0,&id)==MEMO_IDS_EXHAUSTED&&id==0);
 uint8_t bytes[16];assert(memo_record_ids_encode_fact(123,bytes,sizeof(bytes)));assert(memo_record_ids_decode_fact(bytes,sizeof(bytes),&id)&&id==123);bytes[8]^=1;assert(!memo_record_ids_decode_fact(bytes,sizeof(bytes),&id)&&id==0);
 legacy_cardless_fixture_t r={0};memo_cardless_record_t out;r.id=0x80000001U;r.state=MEMO_JOB_SENT;r.may_have_remote_audio=true;r.duration_ms=2000;r.received_samples=32000;r.target.instance.bytes[0]=1;r.target.url_hash[0]=2;strcpy(r.target.device_id,"future-board");
 uint8_t record[MEMO_CARDLESS_RECORD_SIZE];assert(memo_cardless_record_encode(&r,record));assert(memo_cardless_record_decode(record,sizeof(record),&out)&&out.id==r.id&&out.state==MEMO_JOB_SENT&&out.may_have_remote_audio);
 uint8_t original[MEMO_CARDLESS_RECORD_SIZE];memcpy(original,record,sizeof(record));
 for(unsigned kind=0;kind<4;kind++){
  memcpy(record,original,sizeof(record));
  if(kind==0)memset(record+116,0,16);
  else if(kind==1)memset(record+132,0,32);
  else if(kind==2)memset(record+164,'x',40);
  else record[164]=0;
  put32(record+224,checksum(record,224));assert(!memo_cardless_record_decode(record,sizeof(record),&out)&&!out.id);
 }
 memcpy(record,original,sizeof(record));record[20]^=1;assert(!memo_cardless_record_decode(record,sizeof(record),&out)&&!out.id);assert(!memo_cardless_record_decode(record,120,&out));
 puts("PASS persistent IDs: offline multiple/reboot/empty-card/mode switching/faults/exhaustion; first-public cardless codec rejects old/corrupt data");
}
