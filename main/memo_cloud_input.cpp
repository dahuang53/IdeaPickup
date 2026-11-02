#include "memo_cloud_input.h"
#include "network_sync.h"
#include "memo_service_portal.h"
#include "memo_cloud_secret_json.h"
#include "memo_cloud_json_names.h"
#include "cJSON.h"
#include <cstring>
extern "C" void memo_cloud_input_reset(memo_cloud_input_t *s,int64_t now){if(!s)return;volatile uint8_t *p=reinterpret_cast<volatile uint8_t *>(s);for(size_t i=0;i<sizeof(*s);i++)p[i]=0;s->last_ms=now;}
extern "C" int memo_cloud_input_push(memo_cloud_input_t *s,int ch,int64_t now){
 if(!s)return -2;
 if(now-s->last_ms>=15000){memo_cloud_input_reset(s,now);return -1;}
 if(ch<0)return 0;
 s->last_ms=now;
 if(ch=='\r')return 0;
 if(ch=='\n')return s->overflow?-2:1;
 if(s->used==1024){s->overflow=true;return 0;}
 if(!s->overflow)s->bytes[s->used++]=char(ch);
 return 0;
}
namespace {
bool known(const char *s){return s&&(!strcmp(s,"workspace")||!strcmp(s,"qwen_key")||!strcmp(s,"siyuan_token")||!strcmp(s,"clear_qwen")||!strcmp(s,"clear_siyuan"));}
}
extern "C" memo_cloud_result_t memo_cloud_input_apply(const char *body,size_t n){
 if(!body||n>1024||!memo_service_json_text_safe(body,n))return MEMO_CLOUD_INVALID;
 return static_cast<memo_cloud_result_t>(memo_cloud::secret_configuration_json(body,n,MEMO_CLOUD_INVALID,[](void *,void *root)->int {
 cJSON *json=static_cast<cJSON *>(root);
 memo_cloud_patch_t patch{};bool valid=cJSON_IsObject(json);
 for(auto *p=json->child;p&&valid;p=p->next){
  valid=known(p->string);for(auto *q=json->child;q!=p&&valid;q=q->next)if(!strcmp(q->string,p->string))valid=false;
  if(!valid)break;
  if(!strcmp(p->string,"workspace")){valid=cJSON_IsString(p);patch.workspace=p->valuestring;}
  else if(!strcmp(p->string,"qwen_key")){valid=cJSON_IsString(p);patch.qwen_key=p->valuestring;}
  else if(!strcmp(p->string,"siyuan_token")){valid=cJSON_IsString(p);patch.siyuan_token=p->valuestring;}
  else if(!strcmp(p->string,"clear_qwen")){valid=cJSON_IsBool(p);patch.clear_qwen=cJSON_IsTrue(p);}
  else {valid=cJSON_IsBool(p);patch.clear_siyuan=cJSON_IsTrue(p);}
 }
 return valid?network_sync_cloud_input_save(&patch):MEMO_CLOUD_INVALID;
 },nullptr));
}
