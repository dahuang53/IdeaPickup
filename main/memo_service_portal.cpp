#include "memo_service_portal.h"
#include "network_sync.h"
#include <cJSON.h>
#include <cstdio>
#include <cstring>

namespace {
bool encode(cJSON *json,char *out,size_t capacity) {
    char *text=cJSON_PrintUnformatted(json);cJSON_Delete(json);
    if(!text)return false;
    bool fits=strlen(text)<capacity;
    if(fits)memcpy(out,text,strlen(text)+1);
    cJSON_free(text);return fits;
}
const char *message(memo_config_result_t result) {
    switch(result) {
    case MEMO_CONFIG_OK:return "";
    case MEMO_CONFIG_BUSY:return "录放或任务正在处理，请稍后再保存";
    case MEMO_CONFIG_INVALID:return "请检查 HTTPS 服务地址和配对码";
    case MEMO_CONFIG_NOT_CONFIGURED:return "请填写服务地址和新的配对码";
    case MEMO_CONFIG_UNSUPPORTED:return "设置版本暂不支持，本地录音仍可用";
    case MEMO_CONFIG_CONFLICT:return "设置已变化，请刷新后重试";
    default:return "设置保存结果未确认，请稍后重试；本地录音仍可用";
    }
}
}
extern "C" bool memo_portal_status(char *out,size_t capacity) {
    memo_config_public_t config{};network_sync_get_service_config(&config);
    memo_network_status_t network{};network_sync_get_status(&network);
    cJSON *json=cJSON_CreateObject();if(!json)return false;
    cJSON_AddBoolToObject(json,"success",true);
    cJSON_AddStringToObject(json,"server_url",config.base_url);
    cJSON_AddBoolToObject(json,"local_only",!config.enabled);
    cJSON_AddBoolToObject(json,"configured",config.configured);
    cJSON_AddBoolToObject(json,"pairing",config.pairing);
    cJSON_AddBoolToObject(json,"connected",network.connected);
    const char *notice=config.result!=MEMO_CONFIG_OK?message(config.result):
        config.pairing?(network.provisioning?"设置已保存，关闭热点后联网配对；未完成会自动重试":
            (network.error[0]?network.error:"设置已保存，正在配对；未完成时会自动重试")):
        config.configured?"服务已配对":!config.enabled?"仅本地使用，有卡录音和回放可用":
        (network.error[0]?network.error:"服务尚未配对，请填写新的配对码");
    cJSON_AddStringToObject(json,"message",notice);
    return encode(json,out,capacity);
}
extern "C" bool memo_portal_apply(const char *body,size_t length,char *out,size_t capacity) {
    memo_config_result_t result=MEMO_CONFIG_INVALID;
    cJSON *json=memo_service_json_text_safe(body,length)?cJSON_ParseWithLengthOpts(body,length+1,nullptr,true):nullptr;
    if(json) {
        auto *url=cJSON_GetObjectItemCaseSensitive(json,"server_url");
        auto *code=cJSON_GetObjectItemCaseSensitive(json,"pairing_code");
        auto *local=cJSON_GetObjectItemCaseSensitive(json,"local_only");
        if(cJSON_IsObject(json) && cJSON_IsString(url) && cJSON_IsString(code) && cJSON_IsBool(local))
            result=network_sync_configure_service(url->valuestring,code->valuestring,cJSON_IsTrue(local));
        cJSON_Delete(json);
    }
    json=cJSON_CreateObject();if(!json)return false;
    cJSON_AddBoolToObject(json,"success",result==MEMO_CONFIG_OK);
    cJSON_AddStringToObject(json,result==MEMO_CONFIG_OK?"message":"error",
        result==MEMO_CONFIG_OK?"设置已保存；配对与联网状态请看下方提示":message(result));
    bool encoded=encode(json,out,capacity);return encoded && result==MEMO_CONFIG_OK;
}
