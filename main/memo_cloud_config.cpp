#include "memo_cloud_config.h"
#include "memo_cloud.h"
#include <cstring>
#ifdef ESP_PLATFORM
#include "nvs.h"
#endif
namespace {
uint32_t u32(const uint8_t *p){uint32_t v=0;for(unsigned i=0;i<4;i++)v|=uint32_t(p[i])<<(8*i);return v;}
uint32_t crc(const uint8_t *p,size_t n){uint32_t c=UINT32_MAX;while(n--){c^=*p++;for(unsigned i=0;i<8;i++)c=(c>>1)^(0xedb88320U&(0U-(c&1)));}return ~c;}
bool secret(const char *p){size_t n=strnlen(p,257);if(n==257)return false;for(size_t i=0;i<n;i++)if(p[i]<33||p[i]>126)return false;return true;}
}
extern "C" void memo_cloud_config_clear(memo_cloud_config_t *v){if(v){volatile uint8_t *p=reinterpret_cast<volatile uint8_t *>(v);for(size_t i=0;i<sizeof(*v);i++)p[i]=0;}}
extern "C" bool memo_cloud_config_ready(const memo_cloud_config_t *v){
 return v&&v->qwen_generation&&v->siyuan_generation&&memo_cloud::valid_workspace(v->workspace)&&v->qwen_key[0]&&v->siyuan_token[0]&&secret(v->qwen_key)&&secret(v->siyuan_token);
}
extern "C" bool memo_cloud_config_decode(const void *bytes,size_t n,memo_cloud_config_t *v){
 if(!v)return false;
 memo_cloud_config_clear(v);if(!bytes||n!=604)return false;
 const auto *p=static_cast<const uint8_t *>(bytes);
 if(memcmp(p,"R1CF",4)||u32(p+4)!=1||u32(p+8)>1||p[598]||p[599]||u32(p+600)!=crc(p,600))return false;
 v->enabled=u32(p+8);v->qwen_generation=u32(p+12);v->siyuan_generation=u32(p+16);
 memcpy(v->workspace,p+20,64);memcpy(v->qwen_key,p+84,257);memcpy(v->siyuan_token,p+341,257);
 bool valid=memchr(v->workspace,0,64)&&secret(v->qwen_key)&&secret(v->siyuan_token)&&
     (!v->workspace[0]||memo_cloud::valid_workspace(v->workspace))&&
     ((!v->workspace[0]&&!v->qwen_key[0])||v->qwen_generation)&&(!v->siyuan_token[0]||v->siyuan_generation);
 if(!valid)memo_cloud_config_clear(v);
 return valid;
}
extern "C" bool memo_cloud_config_read(memo_cloud_config_t *v){
 if(!v)return false;
 memo_cloud_config_clear(v);
#ifdef ESP_PLATFORM
 nvs_handle_t h;esp_err_t error=nvs_open("memo_cloud1",NVS_READONLY,&h);
 if(error==ESP_ERR_NVS_NOT_FOUND)return true;
 if(error!=ESP_OK)return false;
 uint8_t bytes[604]{};size_t n=sizeof(bytes);error=nvs_get_blob(h,"config",bytes,&n);nvs_close(h);
 bool ok=error==ESP_ERR_NVS_NOT_FOUND||(error==ESP_OK&&memo_cloud_config_decode(bytes,n,v));
 volatile uint8_t *p=bytes;for(size_t i=0;i<sizeof(bytes);i++)p[i]=0;
 return ok;
#else
 return true; // Native config persistence uses an explicit NVS fixture.
#endif
}

extern "C" bool memo_cloud_config_encode(const memo_cloud_config_t *v,uint8_t out[604]){
 if(!v||!out)return false;
 memset(out,0,604);memcpy(out,"R1CF",4);
 auto put=[](uint8_t *p,uint32_t n){for(unsigned i=0;i<4;i++)p[i]=n>>(8*i);};
 put(out+4,1);put(out+8,v->enabled);put(out+12,v->qwen_generation);put(out+16,v->siyuan_generation);
 if(!memchr(v->workspace,0,64)||!memchr(v->qwen_key,0,257)||!memchr(v->siyuan_token,0,257))return false;
 memcpy(out+20,v->workspace,strlen(v->workspace));memcpy(out+84,v->qwen_key,strlen(v->qwen_key));memcpy(out+341,v->siyuan_token,strlen(v->siyuan_token));put(out+600,crc(out,600));
 memo_cloud_config_t decoded{};bool ok=memo_cloud_config_decode(out,604,&decoded);memo_cloud_config_clear(&decoded);return ok;
}
