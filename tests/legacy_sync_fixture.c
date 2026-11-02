#include "legacy_sync_fixture.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static uint32_t crc(const uint8_t *p,size_t n){uint32_t c=UINT32_MAX;while(n--){c^=*p++;for(unsigned i=0;i<8;i++)c=(c>>1)^(0xedb88320U&(0U-(c&1)));}return ~c;}
static void put32(uint8_t*p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
static void put64(uint8_t*p,uint64_t v){put32(p,(uint32_t)v);put32(p+4,(uint32_t)(v>>32));}
static uint64_t generation(const char *base,unsigned slot){char path[320];snprintf(path,sizeof(path),"%s.%u",base,slot);FILE*f=fopen(path,"rb");if(!f)return 0;uint8_t p[164];bool ok=fread(p,1,sizeof(p),f)==sizeof(p);fclose(f);if(!ok||memcmp(p,"R1SJ",4))return 0;uint32_t c=0;for(unsigned i=0;i<4;i++)c|=(uint32_t)p[160+i]<<(8*i);if(c!=crc(p,160))return 0;uint64_t g=0;for(unsigned i=0;i<8;i++)g|=(uint64_t)p[8+i]<<(8*i);return g;}
bool legacy_fixture_write(const char *base,const memo_sync_entry_t *e){
 uint64_t a=generation(base,0),b=generation(base,1),g=a>b?a:b;unsigned slot=a||b?(a>=b?1:0):0;
 uint8_t p[164]={0};memcpy(p,"R1SJ",4);put32(p+4,1);put64(p+8,g+1);put32(p+16,e->id);put32(p+20,e->state);
 put32(p+24,(e->automatic?1U:0U)|(e->manual?2U:0U));put32(p+28,e->bound);memcpy(p+32,e->target.instance.bytes,16);memcpy(p+48,e->target.url_hash,32);
 memcpy(p+80,e->target.device_id,40);put64(p+120,e->audio.bytes);memcpy(p+128,e->audio.sha256,32);put32(p+160,crc(p,160));
 char path[320];snprintf(path,sizeof(path),"%s.%u",base,slot);FILE*f=fopen(path,"wb");if(!f)return false;bool ok=fwrite(p,1,sizeof(p),f)==sizeof(p);if(fflush(f)||fsync(fileno(f)))ok=false;if(fclose(f))ok=false;return ok;
}
