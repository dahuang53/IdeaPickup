#include "memo_sync_cache.h"
#include "legacy_sync_fixture.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static uint32_t crc(const uint8_t *p,size_t n){uint32_t c=UINT32_MAX;while(n--){c^=*p++;for(unsigned i=0;i<8;i++)c=(c>>1)^(0xedb88320U&(0U-(c&1)));}return ~c;}
static void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=v>>(i*8);}
static void raw(const char *base,unsigned slot,uint8_t p[164]){
 char path[320];snprintf(path,sizeof(path),"%s.%u",base,slot);
 put32(p+160,crc(p,160));FILE *f=fopen(path,"wb");assert(f);assert(fwrite(p,1,164,f)==164);assert(!fclose(f));
}
static void legacy_byte_checks(const char *base){
 char path[320];snprintf(path,sizeof(path),"%s.0",base);FILE *f=fopen(path,"rb");assert(f);
 uint8_t original[164],p[164];assert(fread(original,1,164,f)==164);assert(!fclose(f));memo_sync_entry_t out;
 // Equal generations with different flags/hash/remote fields still conflict,
 // even though those fields no longer occupy the current runtime record.
 const unsigned changed[]={24,33,49,81,120,129};
 for(size_t i=0;i<sizeof(changed)/sizeof(changed[0]);i++){
  memcpy(p,original,164);p[changed[i]]^=2;raw(base,1,p);
  assert(memo_sync_entry_read(base,42,&out)==MEMO_CACHE_INVALID);
 }
 for(unsigned kind=0;kind<8;kind++){
  memcpy(p,original,164);
  switch(kind){case 0:put32(p+24,4);break;case 1:put32(p+28,2);break;
   case 2:memset(p+80,'x',40);break;case 3:memset(p+120,0,8);break;
   case 4:memset(p+128,0,32);break;case 5:memset(p+32,0,16);break;
   case 6:memset(p+48,0,32);break;case 7:put32(p+20,MEMO_JOB_CANCELLING+1);break;}
  raw(base,0,p);raw(base,1,p);assert(memo_sync_entry_read(base,42,&out)==MEMO_CACHE_INVALID);
 }
 raw(base,0,original);snprintf(path,sizeof(path),"%s.1",base);assert(!unlink(path));
}
int main(int argc,char **argv){
 assert(argc==2);const char *p=argv[1];legacy_sync_fixture_t e={0};memo_sync_entry_t out;
 assert(memo_sync_entry_read(p,42,&out)==MEMO_CACHE_MISSING);
 e.id=42;e.state=MEMO_JOB_SENT;e.bound=true;e.target.instance.bytes[0]=1;e.target.url_hash[0]=2;
 strcpy(e.target.device_id,"future-board");e.audio.bytes=64044;e.audio.sha256[0]=3;
 assert(legacy_fixture_write(p,&e));assert(memo_sync_entry_read(p,42,&out)==MEMO_CACHE_OK&&out.state==MEMO_JOB_SENT);
 assert(out.bound&&out.id==42);legacy_byte_checks(p);
 e.state=MEMO_JOB_FAILED;e.automatic=e.manual=true;assert(legacy_fixture_write(p,&e));
 assert(memo_sync_entry_read(p,42,&out)==MEMO_CACHE_OK&&out.state==MEMO_JOB_FAILED&&out.bound);
 char path[320];snprintf(path,sizeof(path),"%s.1",p);FILE *f=fopen(path,"wb");assert(f);fputs("torn",f);fclose(f);
 assert(memo_sync_entry_read(p,42,&out)==MEMO_CACHE_OK&&out.state==MEMO_JOB_SENT); // last complete slot survives
 snprintf(path,sizeof(path),"%s.0",p);f=fopen(path,"r+b");assert(f);fseek(f,128,SEEK_SET);fputc(99,f);fclose(f);
 assert(memo_sync_entry_read(p,42,&out)==MEMO_CACHE_INVALID);
 assert(!legacy_fixture_write("/no-such-parent/memo",&e));
 e.bound=false;e.audio=(memo_audio_identity_t){0};e.target=(memo_sync_target_t){0};assert(legacy_fixture_write(p,&e));
 assert(memo_sync_entry_read(p,42,&out)==MEMO_CACHE_OK&&!out.bound);
 puts("PASS legacy metadata decoding: saved state/binding, independent durable slots, corrupt input rejection; no runtime audio identity");
}
