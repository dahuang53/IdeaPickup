#include "memo_cloud_config.h"
#include "memo_cloud.h"
#include "nvs.h"
#include "esp_heap_caps.h"
#include <new>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <initializer_list>
namespace {
struct Config {memo_cloud_config_t value{};~Config(){memo_cloud_config_clear(&value);}};
struct Bytes {uint8_t value[604]{};~Bytes(){volatile uint8_t *p=value;for(size_t i=0;i<sizeof(value);i++)p[i]=0;}};
bool secret(const char *s){size_t n=strnlen(s,257);if(n>256)return false;for(size_t i=0;i<n;i++)if(s[i]<33||s[i]>126)return false;return true;}
}
extern "C" void memo_cloud_settings_public(memo_cloud_public_t *out){
 if(!out)return;
 *out={};Config c;if(!memo_cloud_config_read(&c.value)){out->result=MEMO_CLOUD_STORAGE;return;}
 out->enabled=c.value.enabled;out->configured=memo_cloud_config_ready(&c.value);out->qwen_set=c.value.qwen_key[0];out->siyuan_set=c.value.siyuan_token[0];out->qwen_generation=c.value.qwen_generation;out->siyuan_generation=c.value.siyuan_generation;
}
extern "C" memo_cloud_result_t memo_cloud_settings_apply(const memo_cloud_patch_t *patch,bool *changed){
 if(changed)*changed=false;
 if(!patch)return MEMO_CLOUD_INVALID;
 struct Work {Config old,next,actual;Bytes before,bytes,confirmed;};
 void *memory=heap_caps_malloc(sizeof(Work),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
 if(!memory)return MEMO_CLOUD_STORAGE;
 struct Owned {Work *p;~Owned(){p->~Work();free(p);}} owned{new(memory) Work};
 Config &old=owned.p->old,&next=owned.p->next,&actual=owned.p->actual;
 Bytes &before=owned.p->before,&bytes=owned.p->bytes,&confirmed=owned.p->confirmed;
 if(!memo_cloud_config_read(&old.value))return MEMO_CLOUD_STORAGE;
 next.value=old.value;
 if(patch->check_generation&&(patch->expected_qwen!=old.value.qwen_generation||patch->expected_siyuan!=old.value.siyuan_generation))return MEMO_CLOUD_CONFLICT;
 if((patch->clear_qwen&&(patch->qwen_key&&patch->qwen_key[0]))||(patch->clear_siyuan&&patch->siyuan_token&&patch->siyuan_token[0]))return MEMO_CLOUD_INVALID;
 for(auto pair:{0,1}){const char *s=pair?patch->siyuan_token:patch->qwen_key;if(s&&s[0]){if(!secret(s))return MEMO_CLOUD_INVALID;strcpy(pair?next.value.siyuan_token:next.value.qwen_key,s);}}
 if(patch->clear_qwen){memset(next.value.qwen_key,0,sizeof(next.value.qwen_key));}
 if(patch->clear_siyuan)memset(next.value.siyuan_token,0,sizeof(next.value.siyuan_token));
 if(patch->clear_qwen||patch->clear_siyuan)next.value.enabled=false;
 bool q=strcmp(old.value.qwen_key,next.value.qwen_key),s=strcmp(old.value.siyuan_token,next.value.siyuan_token);
 if((q&&old.value.qwen_generation==UINT32_MAX)||(s&&old.value.siyuan_generation==UINT32_MAX))return MEMO_CLOUD_INVALID;
 next.value.qwen_generation+=q;next.value.siyuan_generation+=s;
 if(patch->set_mode)next.value.enabled=patch->enabled;
 if(next.value.enabled&&!memo_cloud_config_ready(&next.value))return MEMO_CLOUD_NOT_CONFIGURED;
 if(!memo_cloud_config_encode(&old.value,before.value)||!memo_cloud_config_encode(&next.value,bytes.value))return MEMO_CLOUD_INVALID;
 if(!memcmp(before.value,bytes.value,604))return MEMO_CLOUD_OK;
 nvs_stats_t stats{};if(nvs_get_stats(nullptr,&stats)!=ESP_OK)return MEMO_CLOUD_STORAGE;
 nvs_handle_t h;int existing=nvs_open("memo_cloud1",NVS_READONLY,&h);bool stored=false;if(existing==ESP_OK){size_t n=0;int found=nvs_get_blob(h,"config",nullptr,&n);nvs_close(h);if(found!=ESP_OK&&found!=ESP_ERR_NVS_NOT_FOUND)return MEMO_CLOUD_STORAGE;stored=found==ESP_OK;}else if(existing!=ESP_ERR_NVS_NOT_FOUND)return MEMO_CLOUD_STORAGE;
 if(stats.free_entries<size_t(stored?162:197))return MEMO_CLOUD_NO_SPACE;
 int rc=nvs_open("memo_cloud1",NVS_READWRITE,&h);if(rc==ESP_OK){rc=nvs_set_blob(h,"config",bytes.value,604);if(rc==ESP_OK)rc=nvs_commit(h);nvs_close(h);}
 // Always reload a fresh handle, including uncertain set/commit outcomes.
 bool loaded=memo_cloud_config_read(&actual.value);
 bool encoded=loaded&&memo_cloud_config_encode(&actual.value,confirmed.value);
 if(changed&&encoded)*changed=memcmp(before.value,confirmed.value,604)!=0;
 bool exact=encoded&&!memcmp(bytes.value,confirmed.value,604);
 if(rc!=ESP_OK||!exact)return MEMO_CLOUD_STORAGE;
 if(changed)*changed=true;
 return MEMO_CLOUD_OK;
}
