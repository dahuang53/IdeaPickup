"""Execute actual NVS adapter, SSID persistence and runtime admission bodies.

Only ESP/NVS/RTOS/RF and unrelated UI/transport types are host shims. No hardware
claims. Small runtime methods are extracted unchanged, as in the donor harness.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HEADERS = {
    'esp_err.h': '#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL 2\n#define ESP_ERR_NO_MEM 3\n#define ESP_ERR_NVS_NOT_FOUND 1\n',
    'esp_log.h': '#pragma once\n#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGW(...) ((void)0)\n#define ESP_LOGE(...) ((void)0)\n',
    'freertos/FreeRTOS.h': '#pragma once\n#define portMAX_DELAY 10000\n#define pdTRUE 1\n#define pdMS_TO_TICKS(v) (v)\n',
    'freertos/semphr.h': '''#pragma once
#ifdef __cplusplus
extern "C" {
#endif
typedef void *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void);
int xSemaphoreTakeRecursive(SemaphoreHandle_t,int);
int xSemaphoreGiveRecursive(SemaphoreHandle_t);
int xSemaphoreTake(SemaphoreHandle_t,int);
int xSemaphoreGive(SemaphoreHandle_t);
#ifdef __cplusplus
}
#endif
''',
    'nvs_flash.h': '''#pragma once
#include "nvs.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t nvs_flash_init(void);
#ifdef __cplusplus
}
#endif
''',
    'nvs.h': '''#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#define NVS_READONLY 0
#define NVS_READWRITE 1
typedef int nvs_handle_t;
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t nvs_open(const char *,int,nvs_handle_t *);
void nvs_close(nvs_handle_t);
esp_err_t nvs_get_blob(nvs_handle_t,const char *,void *,size_t *);
esp_err_t nvs_set_blob(nvs_handle_t,const char *,const void *,size_t);
esp_err_t nvs_get_str(nvs_handle_t,const char *,char *,size_t *);
esp_err_t nvs_set_str(nvs_handle_t,const char *,const char *);
esp_err_t nvs_get_u8(nvs_handle_t,const char *,uint8_t *);
esp_err_t nvs_set_u8(nvs_handle_t,const char *,uint8_t);
esp_err_t nvs_erase_key(nvs_handle_t,const char *);
esp_err_t nvs_commit(nvs_handle_t);
#ifdef __cplusplus
}
#endif
'''
}

def extract(text, marker):
    start=text.index(marker);brace=text.index('{',start)
    # Selected functions contain no literal braces; balance their actual body.
    depth=1;end=brace+1
    while depth:
        depth += (text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]

HARNESS=r'''
#include "network_sync.h"
#include "memo_network_config_nvs.h"
#include "network_sync_state.h"
#include "ssid_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "esp_log.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <functional>
#include <inttypes.h>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

static SemaphoreHandle_t http_mutex=(void *)1,status_mutex=(void *)2;
static void *sync_task_handle=nullptr;
static memo_config_activity_gate_t activity_gate{};
static std::atomic<bool> file_read_active{false},stream_http_active{false};
static std::atomic<uint32_t> active_record{0};
static bool unique_audio=false,other_http_owner=false;
static memo_network_status_t status_value{};
static memo_sync_queue_t sync_queue{};
bool audio_busy(){return memo_config_activity(&activity_gate)==MEMO_ACTIVITY_AUDIO;}
bool memo_cardless_busy(){return unique_audio;}
int64_t esp_timer_get_time(){static int64_t now=0;return now+=1000;}
void vTaskDelay(int){}
void xTaskNotifyGive(void *){}
void esp_fill_random(void *out,size_t n){for(size_t i=0;i<n;++i)((uint8_t *)out)[i]=(uint8_t)(i+3);}
static std::map<int,std::map<std::string,std::vector<uint8_t>>> durable,pending;
static unsigned writes=0,commits=0,opens=0;
static int fault=0,fault_namespace=0;
static bool after_commit=false;
static bool nvs_ready=false,init_fault=false;
static int http_claims=0;
static unsigned wifi_reads=0,first_write_reads=0;
static bool mark_first_write=false;
extern "C" {
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(){return (void *)3;}
int xSemaphoreTakeRecursive(SemaphoreHandle_t h,int){if(h==http_mutex){if(other_http_owner)return 0;++http_claims;}return 1;}
int xSemaphoreGiveRecursive(SemaphoreHandle_t h){if(h==http_mutex){assert(http_claims>0);--http_claims;}return 1;}
int xSemaphoreTake(SemaphoreHandle_t,int){return 1;}
int xSemaphoreGive(SemaphoreHandle_t){return 1;}
esp_err_t nvs_flash_init(){if(init_fault)return ESP_FAIL;nvs_ready=true;return ESP_OK;}
esp_err_t nvs_open(const char *ns,int mode,nvs_handle_t *h){
    ++opens;assert(!strcmp(ns,"memo_cfg1") || !strcmp(ns,"wifi"));
    assert(nvs_ready); // saved Wi-Fi must never load before NVS initialization
    *h=!strcmp(ns,"wifi")?2:1;
    if(*h==fault_namespace && fault==1 && mode==NVS_READWRITE)return ESP_FAIL;
    if(mode==NVS_READWRITE)pending[*h]=durable[*h];return ESP_OK;
}
void nvs_close(nvs_handle_t){}
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *n){
    if(h==2)++wifi_reads;
    if(h==fault_namespace && (fault==5 || fault==7) && after_commit)return ESP_FAIL;
    auto &d=durable[h];auto found=d.find(key);if(found==d.end())return ESP_ERR_NVS_NOT_FOUND;
    if(*n<found->second.size())return ESP_FAIL;
    *n=found->second.size();memcpy(out,found->second.data(),*n);return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *bytes,size_t n){
    ++writes;
    if(h==2 && mark_first_write){first_write_reads=wifi_reads;mark_first_write=false;}
    // Real configuration mutations must own the short audio exclusion.
    assert(memo_config_activity(&activity_gate)==MEMO_ACTIVITY_COMMIT);
    assert(!network_sync_prepare_audio(0));
    if(h==fault_namespace && fault==2)return ESP_FAIL;
    pending[h][key]=std::vector<uint8_t>((const uint8_t *)bytes,(const uint8_t *)bytes+n);return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t h,const char *key,const char *s){return nvs_set_blob(h,key,s,strlen(s)+1);}
esp_err_t nvs_get_str(nvs_handle_t h,const char *key,char *s,size_t *n){return nvs_get_blob(h,key,s,n);}
esp_err_t nvs_set_u8(nvs_handle_t h,const char *key,uint8_t v){return nvs_set_blob(h,key,&v,1);}
esp_err_t nvs_get_u8(nvs_handle_t h,const char *key,uint8_t *v){size_t n=1;return nvs_get_blob(h,key,v,&n);}
esp_err_t nvs_erase_key(nvs_handle_t h,const char *key){assert(h==2);return pending[h].erase(key)?ESP_OK:ESP_ERR_NVS_NOT_FOUND;}
esp_err_t nvs_commit(nvs_handle_t h){
    ++commits;after_commit=true;
    if(h==fault_namespace && (fault==3 || fault==7))return ESP_FAIL;
    if(!(h==fault_namespace && fault==6))durable[h]=pending[h];
    return h==fault_namespace && fault==4?ESP_FAIL:ESP_OK;
}
}
struct wifi_ap_record_t {uint8_t ssid[33];uint8_t primary;};
class WifiConfigurationAp {
    uint8_t last_connected_channel_=6;
    std::mutex mutex_;
    std::vector<wifi_ap_record_t> ap_records_;
public: bool Save(const std::string &,const std::string &);
};
struct WifiManagerConfig {std::function<bool()> config_commit_begin;std::function<void()> config_commit_end;};
class WifiStation {};
class WifiManager {
    std::mutex mutex_;bool initialized_=false;WifiManagerConfig config_;
    std::unique_ptr<WifiStation> station_;std::unique_ptr<WifiConfigurationAp> config_ap_;
public: bool Initialize(const WifiManagerConfig &);
};
struct wifi_init_config_t {bool nvs_enable=true;};
#define WIFI_INIT_CONFIG_DEFAULT() wifi_init_config_t{}
#define ESP_ERR_INVALID_STATE 4
int esp_netif_init(){return ESP_OK;}
int esp_event_loop_create_default(){return ESP_OK;}
int esp_wifi_init(wifi_init_config_t *cfg){assert(!cfg->nvs_enable);return ESP_OK;}

// __PRODUCTION_BODIES__

int main(int argc,char **argv){
    auto wifi_begin=// __WIFI_BEGIN__
    ;
    auto wifi_end=// __WIFI_END__
    ;
    durable[2]["ssid"]={'o','l','d','-','w','i','f','i',0};durable[2]["password"]={'f','a','k','e',0};durable[2]["channel"]={6};
    WifiManager manager;WifiManagerConfig options{wifi_begin,wifi_end};
    init_fault=true;assert(!manager.Initialize(options));assert(opens==0);
    init_fault=false;assert(manager.Initialize(options));
    assert(SsidManager::GetInstance().GetSsidList().size()==1);
    assert(SsidManager::GetInstance().GetSsidList()[0].ssid=="old-wifi");
    assert(memo_network_config_nvs_init()==ESP_OK);
    snprintf(status_value.device_id,sizeof(status_value.device_id),"future-model-unit");
    WifiConfigurationAp portal;
    assert(portal.Save("synthetic-home","synthetic-password"));
    auto wifi_saved=durable[2];
    assert(network_sync_configure_service("https://one.example:9443/memo","23456789AB",false)==MEMO_CONFIG_OK);
    assert(durable[2]==wifi_saved);
    memo_config_public_t view{};network_sync_get_service_config(&view);
    assert(view.pairing && !view.configured);
    if(argc==2 && !strcmp(argv[1],"F1")){
        fault_namespace=1;fault=2;
        assert(network_sync_configure_service("https://failed-write.example","23456789AB",false)==MEMO_CONFIG_STORAGE_ERROR);
        fault=5;after_commit=true;
        assert(network_sync_configure_service("https://failed-write.example","23456789AB",false)==MEMO_CONFIG_STORAGE_ERROR);
        fault=0;after_commit=false;
        auto recovered=network_sync_configure_service("https://failed-write.example","23456789AB",false);
        fprintf(stderr,"F1 recovery result=%d (expected OK=0)\n",(int)recovered);
        assert(recovered==MEMO_CONFIG_OK);return 0;
    }
    if(argc==2 && !strcmp(argv[1],"F2")){
        sync_queue.pending_id=88;fault_namespace=1;fault=4;
        assert(network_sync_configure_service("https://ack-lost.example","23456789AB",false)==MEMO_CONFIG_STORAGE_ERROR);
        fault=0;after_commit=false;
        assert(network_sync_configure_service("https://ack-lost.example","23456789AB",false)==MEMO_CONFIG_OK);
        fprintf(stderr,"F2 old pending_id=%u (expected 0)\n",sync_queue.pending_id);
        assert(sync_queue.pending_id==0);return 0;
    }
    if(argc==2 && !strcmp(argv[1],"F3")){
        auto prior=durable[2];fault_namespace=2;fault=7;after_commit=false;
        assert(!portal.Save("not-durable","synthetic-only"));assert(durable[2]==prior);
        fault=0;after_commit=false;wifi_reads=0;mark_first_write=true;
        SsidManager::GetInstance().UpdateSsidChannel("synthetic-home",11);
        bool copied_pending=durable[2]["ssid"]==std::vector<uint8_t>({'n','o','t','-','d','u','r','a','b','l','e',0});
        fprintf(stderr,"F3 reads before first write=%u, unconfirmed SSID published=%d\n",first_write_reads,copied_pending);
        assert(first_write_reads>0 && !copied_pending);return 0;
    }
    auto before=durable[1];unsigned prior_writes=writes;
    other_http_owner=true;
    assert(network_sync_configure_service("https://two.example","23456789AB",false)==MEMO_CONFIG_BUSY);
    assert(network_sync_prepare_audio(0)); // long HTTP does not hold the short audio gate
    assert(network_sync_prepare_audio(0)); // emergency audio close re-entry
    assert(!portal.Save("during-audio","not-saved"));
    assert(network_sync_configure_service("https://two.example","23456789AB",false)==MEMO_CONFIG_BUSY);
    network_sync_audio_done();other_http_owner=false;
    assert(writes==prior_writes && durable[1]==before && durable[2]==wifi_saved);
    for(int cut=0;cut<4;++cut){
        file_read_active=cut==0;stream_http_active=cut==1;active_record=cut==2?1:0;unique_audio=cut==3;
        assert(network_sync_configure_service("https://two.example","23456789AB",false)==MEMO_CONFIG_BUSY);
        assert(durable[1]==before);
        file_read_active=false;stream_http_active=false;active_record=0;unique_audio=false;
    }
    file_read_active=true;assert(!network_sync_prepare_audio(0));assert(!audio_busy());file_read_active=false;
    // No-op retries neither rewrite flash nor cancel an unstarted SD request.
    sync_queue.pending_id=88;
    assert(network_sync_configure_service("https://one.example:9443/memo","23456789AB",false)==MEMO_CONFIG_OK);
    assert(sync_queue.pending_id==88 && writes==prior_writes);
    assert(network_sync_configure_service("http://bad.example","23456789AB",false)==MEMO_CONFIG_INVALID);
    assert(sync_queue.pending_id==88);
    assert(network_sync_configure_service("https://two.example","23456789AB",false)==MEMO_CONFIG_OK);
    assert(sync_queue.pending_id==0 && durable[2]==wifi_saved);
    // Default-NVS failures cannot acknowledge settings; next request reloads
    // actual durable facts. No erases or unrelated namespace access.
    fault_namespace=1;fault=2;
    assert(network_sync_configure_service("https://three.example","23456789AB",false)==MEMO_CONFIG_STORAGE_ERROR);
    assert(!audio_busy());assert(network_sync_prepare_audio(0));network_sync_audio_done();
    fault=0;
    assert(network_sync_configure_service("https://three.example","23456789AB",false)==MEMO_CONFIG_OK);
    auto service_saved=durable[1];
    // Wi-Fi can repair connectivity while unique cardless audio is retained;
    // changing the service cannot. Neither operation deletes that audio.
    unique_audio=true;assert(portal.Save("recovery-network","synthetic-recovery-password"));
    assert(network_sync_configure_service("https://four.example","23456789AB",false)==MEMO_CONFIG_BUSY);
    unique_audio=false;
    for(int cut=1;cut<=6;++cut){
        fault_namespace=2;fault=cut;after_commit=false;
        assert(!portal.Save("wifi-fault-"+std::to_string(cut),"synthetic-only"));
        assert(memo_config_activity(&activity_gate)==MEMO_ACTIVITY_IDLE);
        assert(http_claims==0);
        assert(durable[1]==service_saved);
        fault=0;after_commit=false;
        assert(portal.Save("wifi-retry-"+std::to_string(cut),"synthetic-only"));
        assert(http_claims==0);
    }
    puts("PASS actual runtime/NVS/SSID bodies: short audio gate, busy HTTP/file/stream/unique audio, no-op retention, target change, uncertain reload, six Wi-Fi persistence cuts, namespace isolation");
}
'''

class RuntimeTests(unittest.TestCase):
    def test_actual_runtime_and_nvs_and_wifi_faults(self):
        (ROOT/'private/host-tests').mkdir(mode=0o700,exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='service-runtime-',dir=ROOT/'private/host-tests') as directory:
            temp=Path(directory)
            for name,text in HEADERS.items():
                file=temp/name;file.parent.mkdir(parents=True,exist_ok=True);file.write_text(text)
            source=(ROOT/'main/network_sync.cpp').read_text()
            markers=['class ConfigGuard','class ConfigCommitGuard','bool current_service','void status_update','void refresh_service_status',
                     'extern "C" void network_sync_get_status','extern "C" void network_sync_get_service_config',
                     'extern "C" memo_config_result_t network_sync_configure_service',
                     'extern "C" bool network_sync_prepare_audio','extern "C" void network_sync_audio_done']
            bodies='\n'.join(extract(source,marker)+(';' if marker.startswith('class ') else '') for marker in markers)
            ap=(ROOT/'components/esp-wifi-connect/wifi_configuration_ap.cc').read_text()
            bodies+='\n'+extract(ap,'bool WifiConfigurationAp::Save')
            manager=(ROOT/'components/esp-wifi-connect/wifi_manager.cc').read_text()
            bodies+='\n'+extract(manager,'bool WifiManager::Initialize')
            begin=extract(source,'config.config_commit_begin=');begin=begin.split('=',1)[1]
            end=extract(source,'config.config_commit_end=');end=end.split('=',1)[1]
            harness=HARNESS.replace('// __PRODUCTION_BODIES__',bodies).replace('// __WIFI_BEGIN__',begin).replace('// __WIFI_END__',end)
            file=temp/'runtime.cpp';file.write_text(harness)
            includes=['-I',str(temp),'-I',str(ROOT/'main'),'-I',str(ROOT/'components/esp-wifi-connect/include')]
            objects=[]
            for name in ('memo_network_config.c','memo_network_config_nvs.c'):
                obj=temp/(name+'.o');objects.append(str(obj))
                subprocess.run(['cc','-std=c11','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',*includes,'-c',str(ROOT/'main'/name),'-o',str(obj)],check=True)
            binary=temp/'runtime'
            subprocess.run(['c++','-std=c++17','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-DMEMO_HOST_TEST',*includes,str(file),str(ROOT/'components/esp-wifi-connect/ssid_manager.cc'),*objects,'-o',str(binary)],check=True)
            results={}
            for scenario in ('','F1','F2','F3'):
                results[scenario or 'baseline']=subprocess.run([str(binary),*([scenario] if scenario else [])]).returncode
            self.assertEqual(results,{name:0 for name in results})

if __name__=='__main__': unittest.main(verbosity=2)
