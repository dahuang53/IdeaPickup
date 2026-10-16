#include "memo_service_pairing.h"
#include "cJSON.h"
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {
// Bound external JSON before cJSON recursion. No migration grant/parser framework.
bool json_safe(const char *text, size_t length) {
    unsigned depth=0; bool quoted=false,escape=false;
    for(size_t i=0;i<length;++i){unsigned char c=(unsigned char)text[i];
        if(!c)return false;
        if(quoted){
            if(escape){if(c=='u' && i+4<length && !memcmp(text+i+1,"0000",4))return false;escape=false;}
            else if(c=='\\')escape=true;
            else if(c=='"')quoted=false;
            continue;
        }
        if(c=='"')quoted=true;
        else if(c=='{'||c=='['){if(++depth>16)return false;}
        else if(c=='}'||c==']'){if(!depth--)return false;}
    }
    return !quoted && !depth;
}
struct Json {
    cJSON *root=nullptr;
    explicit Json(const char *text){size_t n=strlen(text);if(!json_safe(text,n))return;
        const char *end=nullptr;root=cJSON_ParseWithLengthOpts(text,n+1,&end,true);
        if(!cJSON_IsObject(root)){cJSON_Delete(root);root=nullptr;}}
    ~Json(){cJSON_Delete(root);}
};
const char *field(cJSON *root,const char *name){const cJSON *v=cJSON_GetObjectItemCaseSensitive(root,name);return cJSON_IsString(v)?v->valuestring:nullptr;}
bool identity(cJSON *root,const memo_config_snapshot_t &p,memo_server_uuid_t &instance){
    const char *id=field(root,"enrollment_id"),*device=field(root,"device_id"),*uuid=field(root,"server_instance_uuid");
    return id && device && uuid && !strcmp(id,p.enrollment_id) && !strcmp(device,p.device_id) && memo_config_uuid_parse(uuid,&instance);
}
bool terminal_error(const char *s){return s && (!strcmp(s,"invalid_pairing_code") || !strcmp(s,"enrollment_expired") ||
    !strcmp(s,"enrollment_revoked") || !strcmp(s,"enrollment_superseded"));}
memo_pair_result_t failure(int code,const char *body,memo_pairing_receipt_t *out){
    if(!code)return MEMO_PAIR_NETWORK;
    Json json(body);const char *error=field(json.root,"error");
    if(error && strlen(error)<sizeof(out->error_code))snprintf(out->error_code,sizeof(out->error_code),"%s",error);
    // A server rejecting our fixed request schema is incompatible, not a
    // transient connection failure or an invalid user-entered pairing code.
    if(code==400 && error && !strcmp(error,"invalid_fields"))return MEMO_PAIR_INVALID_RESPONSE;
    return terminal_error(error)?MEMO_PAIR_REJECTED:MEMO_PAIR_NETWORK;
}
}

extern "C" memo_pair_result_t memo_pairing_run(memo_config_snapshot_t p,const memo_pairing_io_t *io,memo_pairing_receipt_t *out)
{
    if(!io || !io->request || !io->bind || !out)return MEMO_PAIR_INVALID_RESPONSE;
    *out={}; if(!p.enabled || p.phase!=MEMO_SERVICE_PAIRING)return MEMO_PAIR_BUSY;
    char body[2049]{};
    memo_server_uuid_t zero{};
    if(!memo_config_uuid_equal(&p.instance,&zero)){
        int code=io->request(io->context,p.base_url,"GET","/v1/info",nullptr,nullptr,body,sizeof(body));
        if(code!=200)return failure(code,body,out);
        Json json(body);memo_server_uuid_t actual{};const char *service=field(json.root,"service"),*uuid=field(json.root,"server_instance_uuid");
        const cJSON *version=cJSON_GetObjectItemCaseSensitive(json.root,"protocol_version");
        if(!service || strcmp(service,"voice-memo-relay") || !cJSON_IsNumber(version) || version->valuedouble!=2 || !uuid || !memo_config_uuid_parse(uuid,&actual))return MEMO_PAIR_INVALID_RESPONSE;
        if(!memo_config_uuid_equal(&actual,&p.instance))return MEMO_PAIR_INSTANCE_CHANGED;
    }
    cJSON *request=cJSON_CreateObject();
    bool encoded=request && cJSON_AddStringToObject(request,"device_id",p.device_id) && cJSON_AddStringToObject(request,"pairing_code",p.pairing_code) &&
        cJSON_AddStringToObject(request,"enrollment_id",p.enrollment_id) && cJSON_AddStringToObject(request,"candidate_token",p.token);
    char *payload=encoded?cJSON_PrintUnformatted(request):nullptr;cJSON_Delete(request);
    if(!payload)return MEMO_PAIR_NETWORK;
    int code=io->request(io->context,p.base_url,"POST","/v1/enrollments",nullptr,payload,body,sizeof(body));cJSON_free(payload);
    if(code!=201)return failure(code,body,out);
    Json created(body);memo_server_uuid_t instance{};const char *phase=field(created.root,"phase");
    if(!identity(created.root,p,instance) || !phase || (strcmp(phase,"created") && strcmp(phase,"activated") && strcmp(phase,"finalized")))return MEMO_PAIR_INVALID_RESPONSE;
    if(!memo_config_uuid_equal(&p.instance,&zero) && !memo_config_uuid_equal(&p.instance,&instance))return MEMO_PAIR_INSTANCE_CHANGED;
    memo_config_result_t bound=io->bind(io->context,&p,&instance);
    if(bound!=MEMO_CONFIG_OK)return bound==MEMO_CONFIG_BUSY?MEMO_PAIR_BUSY:MEMO_PAIR_STORAGE;
    for(const char *action:{"activate","finalize"}){
        char path[96];snprintf(path,sizeof(path),"/v1/enrollments/%s/%s",p.enrollment_id,action);
        code=io->request(io->context,p.base_url,"POST",path,p.token,"{}",body,sizeof(body));
        if(code!=200)return failure(code,body,out);
        Json response(body);memo_server_uuid_t actual{};phase=field(response.root,"phase");
        if(!identity(response.root,p,actual) || !memo_config_uuid_equal(&actual,&instance) || !phase ||
            (strcmp(action,"finalize")?strcmp(phase,"activated") && strcmp(phase,"finalized"):strcmp(phase,"finalized")))return MEMO_PAIR_INVALID_RESPONSE;
    }
    out->instance=instance;return MEMO_PAIR_OK;
}
