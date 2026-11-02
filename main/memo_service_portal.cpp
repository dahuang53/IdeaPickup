#include "memo_service_portal.h"
#include "network_sync.h"
#include <cJSON.h>
#include <cstdio>
#include <cstring>
#include <cmath>
namespace {
bool encode(cJSON *json,char *out,size_t capacity){char *text=cJSON_PrintUnformatted(json);cJSON_Delete(json);if(!text)return false;bool fits=strlen(text)<capacity;if(fits)memcpy(out,text,strlen(text)+1);cJSON_free(text);return fits;}
const char *message(memo_cloud_result_t r){switch(r){case MEMO_CLOUD_OK:return "设置已保存；凭据未在此验证";case MEMO_CLOUD_BUSY:return "录放或任务正在处理，请稍后保存";case MEMO_CLOUD_INVALID:return "设置无效";case MEMO_CLOUD_CONFLICT:return "设置已变化，请刷新后重试";case MEMO_CLOUD_NOT_CONFIGURED:return "请先通过USB配置千问和思源";case MEMO_CLOUD_NO_SPACE:return "配置存储空间不足，原设置保留";default:return "保存结果未确认，请刷新后核对";}}
cJSON *field(cJSON *j,const char *s){cJSON *found=nullptr;for(auto *p=j?j->child:nullptr;p;p=p->next)if(p->string&&!strcmp(p->string,s)){if(found)return nullptr;found=p;}return found;}
bool generation(cJSON *p,uint32_t &out){if(!cJSON_IsNumber(p)||!std::isfinite(p->valuedouble)||p->valuedouble<0||p->valuedouble>UINT32_MAX||floor(p->valuedouble)!=p->valuedouble)return false;out=uint32_t(p->valuedouble);return true;}
}
extern "C" bool memo_portal_status(char *out,size_t capacity){
 memo_cloud_public_t c{};network_sync_get_cloud_config(&c);memo_network_status_t n{};network_sync_get_status(&n);
 cJSON *j=cJSON_CreateObject();if(!j)return false;cJSON_AddBoolToObject(j,"success",true);
 cJSON_AddBoolToObject(j,"local_only",!c.enabled);cJSON_AddBoolToObject(j,"configured",c.configured);cJSON_AddBoolToObject(j,"qwen_set",c.qwen_set&&c.workspace_set);cJSON_AddBoolToObject(j,"siyuan_set",c.siyuan_set);
 cJSON_AddNumberToObject(j,"qwen_generation",c.qwen_generation);cJSON_AddNumberToObject(j,"siyuan_generation",c.siyuan_generation);
 cJSON_AddBoolToObject(j,"connected",n.connected);
 const char *phase=c.result!=MEMO_CLOUD_OK?"error":!c.enabled?"local":!c.configured?"unconfigured":"ready";
 cJSON_AddStringToObject(j,"phase",phase);cJSON_AddStringToObject(j,"message",c.result!=MEMO_CLOUD_OK?message(c.result):c.configured?"凭据已保存，尚未验证":"通过USB配置千问和思源");
 cJSON_AddNumberToObject(j,"task_id",n.last_task_id);cJSON_AddNumberToObject(j,"task_state",n.last_task_state);cJSON_AddStringToObject(j,"task_error",n.last_task_error);
 return encode(j,out,capacity);
}
extern "C" bool memo_portal_apply(const char *body,size_t length,char *out,size_t capacity){
 auto result=MEMO_CLOUD_INVALID;
 cJSON *j=body&&length<=1024&&memo_service_json_text_safe(body,length)?cJSON_ParseWithLengthOpts(body,length+1,nullptr,true):nullptr;
 if(cJSON_IsObject(j)){
  auto *local=field(j,"local_only"),*q=field(j,"clear_qwen"),*s=field(j,"clear_siyuan");memo_cloud_patch_t patch{};patch.check_generation=true;
  bool valid=cJSON_IsBool(local)&&generation(field(j,"qwen_generation"),patch.expected_qwen)&&generation(field(j,"siyuan_generation"),patch.expected_siyuan)&&(!q||cJSON_IsBool(q))&&(!s||cJSON_IsBool(s));
  for(auto *p=j->child;p&&valid;p=p->next)valid=p->string&&(!strcmp(p->string,"local_only")||!strcmp(p->string,"qwen_generation")||!strcmp(p->string,"siyuan_generation")||!strcmp(p->string,"clear_qwen")||!strcmp(p->string,"clear_siyuan"))&&field(j,p->string)==p;
  patch.set_mode=true;patch.enabled=!cJSON_IsTrue(local);patch.clear_qwen=cJSON_IsTrue(q);patch.clear_siyuan=cJSON_IsTrue(s);
  if(valid)result=network_sync_configure_cloud(&patch);
 }
 cJSON_Delete(j);j=cJSON_CreateObject();if(!j)return false;cJSON_AddBoolToObject(j,"success",result==MEMO_CLOUD_OK);cJSON_AddStringToObject(j,result==MEMO_CLOUD_OK?"message":"error",message(result));return encode(j,out,capacity)&&result==MEMO_CLOUD_OK;
}
