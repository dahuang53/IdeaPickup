#include "memo_network_config.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t durable[MEMO_CONFIG_BLOB_MAX], pending[MEMO_CONFIG_BLOB_MAX];
    size_t durable_size,pending_size;
    unsigned writes,commits;
    int fault;
} disk_t;
static memo_config_io_t read_disk(void *ctx,const char *key,uint8_t *out,size_t *length) {
    disk_t *d=ctx;assert(!strcmp(key,"service"));
    if(d->fault==5)return MEMO_CONFIG_IO_ERROR;
    if(!d->durable_size)return MEMO_CONFIG_IO_NOT_FOUND;
    if(*length<d->durable_size)return MEMO_CONFIG_IO_ERROR;
    memcpy(out,d->durable,d->durable_size);*length=d->durable_size;
    if(d->fault==6)out[*length-1]^=1;
    return MEMO_CONFIG_IO_OK;
}
static memo_config_io_t write_disk(void *ctx,const char *key,const uint8_t *bytes,size_t length) {
    disk_t *d=ctx;assert(!strcmp(key,"service"));assert(length<=sizeof(d->pending));++d->writes;
    if(d->fault==1)return MEMO_CONFIG_IO_ERROR;
    memcpy(d->pending,bytes,length);d->pending_size=length;
    if(d->fault==2){memcpy(d->durable,bytes,length);d->durable_size=length;return MEMO_CONFIG_IO_ERROR;}
    return MEMO_CONFIG_IO_OK;
}
static memo_config_io_t commit_disk(void *ctx) {
    disk_t *d=ctx;++d->commits;if(d->fault==3)return MEMO_CONFIG_IO_ERROR;
    memcpy(d->durable,d->pending,d->pending_size);d->durable_size=d->pending_size;
    return d->fault==4?MEMO_CONFIG_IO_ERROR:MEMO_CONFIG_IO_OK;
}
static memo_config_storage_t io(disk_t *d){memo_config_storage_t s={d,read_disk,write_disk,commit_disk};return s;}
static const char *identifier="11111111-2222-4333-8444-555555555555";
static const char *token="0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static memo_config_result_t stage(memo_config_t *c,const char *url) {
    return memo_config_set_service(c,"future-board-abc",url,"23456789AB",identifier,token,false);
}
static void url_tests(void) {
    const char *bad[]={"http://memo.example","https://","https://user@memo.example","https://x:0","https://x:65536","https://x:0443","https://01.2.3.4","https://256.2.3.4","https://[::1%eth0]","https://[1:2:3]","https://x/a/../b","https://x/a/%2e%2e/b","https://x/a//b","https://x/a%2fb","https://x/hello%00","https://x/hello%20","https://x/hello%5c","https://x/a?b=1","https://x/a#part","https://x/\\a","https://x/ a"};
    char normalized[241],endpoint[512];
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i)assert(memo_config_validate_url(bad[i],normalized,sizeof(normalized))==MEMO_CONFIG_INVALID);
    assert(memo_config_validate_url("HTTPS://Memo.Example:443/memo/",normalized,sizeof(normalized))==MEMO_CONFIG_OK);
    assert(!strcmp(normalized,"https://memo.example/memo"));
    assert(memo_config_join_url(normalized,"/v1/enrollments",endpoint,sizeof(endpoint))==MEMO_CONFIG_OK);
    assert(!strcmp(endpoint,"https://memo.example/memo/v1/enrollments"));
    assert(memo_config_join_url("https://memo.example/语音","/v1/info",endpoint,sizeof(endpoint))==MEMO_CONFIG_OK);
    assert(!strcmp(endpoint,"https://memo.example/%E8%AF%AD%E9%9F%B3/v1/info"));
    assert(memo_config_join_url("https://memo.example/%61","/v1/info",endpoint,sizeof(endpoint))==MEMO_CONFIG_OK);
    assert(!strcmp(endpoint,"https://memo.example/%61/v1/info"));
    assert(memo_config_validate_url("https://192.168.1.20:9443/memo",normalized,sizeof(normalized))==MEMO_CONFIG_OK);
    assert(memo_config_validate_url("https://[2001:DB8:0:0:0:0:0:1]:9443/memo",normalized,sizeof(normalized))==MEMO_CONFIG_OK);
    assert(!strcmp(normalized,"https://[2001:db8::1]:9443/memo"));
    assert(memo_config_join_url(normalized,"/v1/memos",endpoint,12)==MEMO_CONFIG_INVALID && !endpoint[0]);
    assert(memo_config_join_url(normalized,"//evil.example/a",endpoint,sizeof(endpoint))==MEMO_CONFIG_INVALID);
}
int main(void) {
    url_tests();disk_t d={0};memo_config_storage_t storage=io(&d);memo_config_t c;
    assert(memo_config_load(&c,&storage)==MEMO_CONFIG_OK && !d.writes);
    memo_config_public_t public;memo_config_snapshot_t secret;
    memo_config_public_status(&c,&public);assert(!public.configured && !public.enabled);
    assert(memo_config_credentials(&c,&secret)==MEMO_CONFIG_NOT_CONFIGURED && !secret.token[0]);
    assert(stage(&c,"https://memo.example:9443/memo/")==MEMO_CONFIG_OK);
    assert(c.value.phase==MEMO_SERVICE_PAIRING && !strcmp(c.value.base_url,"https://memo.example:9443/memo"));
    assert(!strcmp(c.value.enrollment_id,identifier) && !strcmp(c.value.token,token));
    memo_config_t reboot;assert(memo_config_load(&reboot,&storage)==MEMO_CONFIG_OK);
    assert(!memcmp(&c.value,&reboot.value,sizeof(c.value)));
    unsigned writes=d.writes;assert(stage(&reboot,"https://memo.example:9443/memo")==MEMO_CONFIG_OK && d.writes==writes);
    memo_server_uuid_t instance;assert(memo_config_uuid_parse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee",&instance));
    assert(memo_config_bind_instance(&reboot,reboot.value.generation,&instance)==MEMO_CONFIG_OK);
    assert(memo_config_credentials(&reboot,&secret)==MEMO_CONFIG_NOT_CONFIGURED);
    assert(memo_config_finish_pairing(&reboot,reboot.value.generation,&instance)==MEMO_CONFIG_OK);
    assert(memo_config_credentials(&reboot,&secret)==MEMO_CONFIG_OK && !strcmp(secret.token,token));
    assert(!reboot.value.pairing_code[0] && !reboot.value.enrollment_id[0]);
    writes=d.writes;
    assert(memo_config_set_service(&reboot,"future-board-abc",secret.base_url,"",NULL,NULL,false)==MEMO_CONFIG_OK && d.writes==writes);
    assert(memo_config_set_service(&reboot,"future-board-abc",NULL,NULL,NULL,NULL,true)==MEMO_CONFIG_OK);
    assert(memo_config_credentials(&reboot,&secret)==MEMO_CONFIG_NOT_CONFIGURED);
    assert(memo_config_load(&c,&storage)==MEMO_CONFIG_OK && !c.value.enabled);
    assert(memo_config_set_service(&c,"future-board-abc",c.value.base_url,"",NULL,NULL,false)==MEMO_CONFIG_OK);
    assert(memo_config_credentials(&c,&secret)==MEMO_CONFIG_OK);
    assert(memo_config_set_service(&c,"future-board-abc","https://new.example/memo","",NULL,NULL,false)==MEMO_CONFIG_OK);
    assert(c.value.phase==MEMO_SERVICE_UNPAIRED && !c.value.token[0]);
    // Every write/commit/readback fault can mean old or new durable bytes, never
    // successful acknowledgement or credential access until an exact reload.
    for(int fault=1;fault<=6;++fault){
        disk_t cut={0};memo_config_storage_t cutio=io(&cut);memo_config_t failed;
        assert(memo_config_load(&failed,&cutio)==MEMO_CONFIG_OK);cut.fault=fault;
        assert(stage(&failed,"https://fault.example")==MEMO_CONFIG_STORAGE_ERROR && failed.needs_reload);
        unsigned attempts=cut.writes;cut.fault=0;
        assert(stage(&failed,"https://later.example")==MEMO_CONFIG_STORAGE_ERROR && cut.writes==attempts);
        assert(memo_config_credentials(&failed,&secret)==MEMO_CONFIG_STORAGE_ERROR && !secret.token[0]);
        assert(memo_config_load(&failed,&cutio)==MEMO_CONFIG_OK);
        assert(failed.value.phase==MEMO_SERVICE_UNPAIRED || failed.value.phase==MEMO_SERVICE_PAIRING);
        assert(stage(&failed,"https://after-reload.example")==MEMO_CONFIG_OK);
    }
    d.durable[25]^=1;assert(memo_config_load(&c,&storage)==MEMO_CONFIG_STORAGE_ERROR);
    writes=d.writes;assert(stage(&c,"https://x")==MEMO_CONFIG_STORAGE_ERROR && d.writes==writes);
    d.durable[4]=2;assert(memo_config_load(&c,&storage)==MEMO_CONFIG_UNSUPPORTED);
    assert(stage(&c,"https://x")==MEMO_CONFIG_UNSUPPORTED && d.writes==writes);
    memo_config_activity_gate_t gate={0};
    assert(memo_config_activity_acquire(&gate,MEMO_ACTIVITY_AUDIO));
    assert(!memo_config_activity_acquire(&gate,MEMO_ACTIVITY_COMMIT));
    assert(!memo_config_activity_release(&gate,MEMO_ACTIVITY_COMMIT));
    assert(memo_config_activity_release(&gate,MEMO_ACTIVITY_AUDIO));
    assert(memo_config_activity_acquire(&gate,MEMO_ACTIVITY_COMMIT));
    assert(!memo_config_activity_acquire(&gate,MEMO_ACTIVITY_AUDIO));
    assert(memo_config_activity_release(&gate,MEMO_ACTIVITY_COMMIT));
    assert(memo_config_activity_acquire(&gate,MEMO_ACTIVITY_MEDIA));
    assert(!memo_config_activity_acquire(&gate,MEMO_ACTIVITY_AUDIO));
    assert(!memo_config_activity_acquire(&gate,MEMO_ACTIVITY_COMMIT));
    assert(!memo_config_activity_release(&gate,MEMO_ACTIVITY_AUDIO));
    assert(memo_config_activity_release(&gate,MEMO_ACTIVITY_MEDIA));
    puts("PASS current-service config: URL, reload, local-only, no secret on uncertainty, all six persistence cuts, schema/corruption, audio exclusion");
}
