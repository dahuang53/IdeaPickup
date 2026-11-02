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

HEADERS['nvs.h']+='\ntypedef struct {size_t free_entries;} nvs_stats_t;\n#ifdef __cplusplus\nextern "C" {\n#endif\nesp_err_t nvs_get_stats(const char *,nvs_stats_t *);\n#ifdef __cplusplus\n}\n#endif\n'
HEADERS['esp_heap_caps.h']='#pragma once\n#include <stddef.h>\n#define MALLOC_CAP_INTERNAL 1\n#define MALLOC_CAP_8BIT 2\n#ifdef __cplusplus\nextern "C" {\n#endif\nvoid *heap_caps_malloc(size_t,int);\n#ifdef __cplusplus\n}\n#endif\n'
HARNESS=r'''

#include "network_sync.h"
#include "memo_cloud_config.h"
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
static std::atomic<bool> sd_scan_requested{true},sd_abort{false},sd_media_abort{false},sd_resume_after_media{false};
static memo_config_activity_gate_t activity_gate{};
static std::atomic<uint32_t> file_read_active{0};
static std::atomic<memo_job_state_t> active_state{MEMO_JOB_UNKNOWN};
static std::atomic<uint32_t> active_record{0};
static std::map<void *,size_t> json_allocations;
static bool fail_secret_arena=false;
static unsigned wiped_arenas=0;
extern "C" void *memo_cloud_test_allocate(size_t n){if(fail_secret_arena&&n==16384)return nullptr;void *p=malloc(n);if(p)json_allocations[p]=n;return p;}
extern "C" void memo_cloud_test_free(void *p){if(!p)return;auto i=json_allocations.find(p);assert(i!=json_allocations.end());if(i->second==16384){for(size_t n=0;n<i->second;n++)assert(static_cast<unsigned char *>(p)[n]==0);wiped_arenas++;}json_allocations.erase(i);free(p);}
static bool unique_audio=false,other_http_owner=false;
[[maybe_unused]] static std::atomic<bool> power_suspended{false};
static std::atomic<void *> cloud_input_owner{nullptr};
static std::atomic<bool> cloud_reload_needed{false};
static bool configuration_bootstrap=true,cardless_ready_cache=false;
static uint32_t shown_qwen=0,shown_siyuan=0;
using TaskHandle_t=void *;
static void *xTaskGetCurrentTaskHandle(){return (void *)7;}
static int cardless_refreshes=0;
void memo_cardless_configure(bool ready,bool changed){if(!configuration_bootstrap)assert(memo_config_activity(&activity_gate)==MEMO_ACTIVITY_COMMIT);if(changed)assert(!unique_audio);cardless_ready_cache=ready;cardless_refreshes++;}
static memo_network_status_t status_value{};
static memo_sync_queue_t sync_queue{};
bool audio_busy();
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
void *heap_caps_malloc(size_t n,int){return malloc(n);}
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(){return (void *)3;}
int xSemaphoreTakeRecursive(SemaphoreHandle_t h,int){if(h==http_mutex){if(other_http_owner)return 0;++http_claims;}return 1;}
int xSemaphoreGiveRecursive(SemaphoreHandle_t h){if(h==http_mutex){assert(http_claims>0);--http_claims;}return 1;}
int xSemaphoreTake(SemaphoreHandle_t,int){return 1;}
int xSemaphoreGive(SemaphoreHandle_t){return 1;}
esp_err_t nvs_flash_init(){if(init_fault)return ESP_FAIL;nvs_ready=true;return ESP_OK;}
esp_err_t nvs_open(const char *ns,int mode,nvs_handle_t *h){
    ++opens;assert(!strcmp(ns,"memo_cloud1") || !strcmp(ns,"wifi"));
    assert(nvs_ready); // saved Wi-Fi must never load before NVS initialization
    *h=!strcmp(ns,"wifi")?2:1;
    if(*h==fault_namespace && fault==1 && mode==NVS_READWRITE)return ESP_FAIL;
    if(mode==NVS_READONLY&&!durable.count(*h))return ESP_ERR_NVS_NOT_FOUND;
    if(mode==NVS_READWRITE){pending[*h]=durable[*h];}
    return ESP_OK;
}
void nvs_close(nvs_handle_t){}
static size_t free_entries=500;
esp_err_t nvs_get_stats(const char *,nvs_stats_t *stats){stats->free_entries=free_entries;return ESP_OK;}
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *n){
    if(h==2)++wifi_reads;
    if(h==fault_namespace && (fault==5 || fault==7) && after_commit)return ESP_FAIL;
    auto &d=durable[h];auto found=d.find(key);if(found==d.end())return ESP_ERR_NVS_NOT_FOUND;
    if(!out){*n=found->second.size();return ESP_OK;}
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
struct esp_netif_t {};
class WifiManager {
    esp_netif_t *ap_netif_=nullptr,*sta_netif_=nullptr;
    std::mutex mutex_;bool initialized_=false;WifiManagerConfig config_;
    std::unique_ptr<WifiStation> station_;std::unique_ptr<WifiConfigurationAp> config_ap_;
public: bool Initialize(const WifiManagerConfig &);
    static WifiManager &GetInstance(){static WifiManager wifi;return wifi;}
    bool IsConnected(){return true;}
    bool IsInitialized(){return initialized_;}
    void StopStation(){assert(false); /* probe must reject suspend before Wi-Fi I/O */}
};
struct wifi_init_config_t {bool nvs_enable=true;};
#define WIFI_INIT_CONFIG_DEFAULT() wifi_init_config_t{}
#define ESP_ERR_INVALID_STATE 4
esp_netif_t *esp_netif_create_default_wifi_ap(){static esp_netif_t n;return &n;}
esp_netif_t *esp_netif_create_default_wifi_sta(){static esp_netif_t n;return &n;}
void esp_netif_destroy_default_wifi(esp_netif_t *){}
void esp_wifi_deinit(){}
int esp_netif_init(){return ESP_OK;}
int esp_event_loop_create_default(){return ESP_OK;}
int esp_wifi_init(wifi_init_config_t *cfg){assert(!cfg->nvs_enable);return ESP_OK;}

// __PRODUCTION_BODIES__


'''

RUNTIME_MAIN=r'''
#include "memo_cloud_input.h"
#include "memo_cloud.h"
static memo_cloud_patch_t credentials(){memo_cloud_patch_t p{};p.workspace="llm-fixture";p.qwen_key="synthetic-key";p.siyuan_token="synthetic-token";return p;}
int main(int argc,char **argv){
 assert(argc==2);std::string scenario=argv[1];auto begin_wifi=// __WIFI_BEGIN__
 ;auto end_wifi=// __WIFI_END__
 ;
 durable[2]["ssid"]={'o','l','d','-','w','i','f','i',0};durable[2]["password"]={'f','a','k','e',0};durable[2]["channel"]={6};
 WifiManager manager;WifiManagerConfig options{begin_wifi,end_wifi};init_fault=true;assert(!manager.Initialize(options)&&opens==0);init_fault=false;assert(manager.Initialize(options));
 assert(SsidManager::GetInstance().GetSsidList().size()==1);auto original_wifi=durable[2];status_value.provisioning=true;refresh_service_status();status_value.initialized=true;configuration_bootstrap=false;
 auto p=credentials();memo_cloud_public_t pub{};
 if(scenario=="settings"){
  assert(network_sync_configure_cloud(&p)==MEMO_CLOUD_OK);network_sync_get_cloud_config(&pub);assert(pub.configured&&!pub.enabled&&pub.qwen_generation==1&&pub.siyuan_generation==1);
  assert(network_sync_set_local_only(false)==MEMO_CLOUD_OK);network_sync_get_cloud_config(&pub);assert(pub.enabled&&pub.configured);assert(durable[2]==original_wifi);
  memo_cloud_patch_t change{};change.qwen_key="new-synthetic";assert(network_sync_configure_cloud(&change)==MEMO_CLOUD_OK);network_sync_get_cloud_config(&pub);assert(pub.qwen_generation==2&&pub.siyuan_generation==1);
  change={};change.clear_siyuan=true;assert(network_sync_configure_cloud(&change)==MEMO_CLOUD_OK);network_sync_get_cloud_config(&pub);assert(!pub.enabled&&!pub.siyuan_set&&pub.siyuan_generation==2&&pub.qwen_generation==2);
 }
 else if(scenario=="noop"){
  assert(network_sync_configure_cloud(&p)==MEMO_CLOUD_OK);auto before=durable[1];unsigned n=writes;memo_cloud_patch_t empty{};empty.workspace="";empty.qwen_key="";empty.siyuan_token="";
  for(int i=0;i<100;i++)assert(network_sync_configure_cloud(&empty)==MEMO_CLOUD_OK);assert(writes==n&&durable[1]==before);
 }
 else if(scenario=="boundaries"){
  std::string key(256,'x');p.qwen_key=key.c_str();p.siyuan_token=key.c_str();assert(network_sync_configure_cloud(&p)==MEMO_CLOUD_OK);auto before=durable[1];key+='x';assert(network_sync_configure_cloud(&p)==MEMO_CLOUD_INVALID&&durable[1]==before);
  p=credentials();p.workspace="-bad";assert(network_sync_configure_cloud(&p)==MEMO_CLOUD_INVALID);p.workspace="llm-fixture";p.qwen_key="x\nsecret";assert(network_sync_configure_cloud(&p)==MEMO_CLOUD_INVALID);
  memo_cloud_config_t value{};assert(memo_cloud_config_read(&value));value.qwen_generation=UINT32_MAX;uint8_t bytes[604];assert(memo_cloud_config_encode(&value,bytes));durable[1]["config"]={bytes,bytes+604};p=credentials();assert(network_sync_configure_cloud(&p)==MEMO_CLOUD_INVALID);
 }
 else if(scenario=="capacity"){
  free_entries=196;assert(network_sync_configure_cloud(&p)==MEMO_CLOUD_NO_SPACE&&!durable.count(1));free_entries=197;assert(network_sync_configure_cloud(&p)==MEMO_CLOUD_OK);
  p.qwen_key="changed";auto before=durable[1];free_entries=161;assert(network_sync_configure_cloud(&p)==MEMO_CLOUD_NO_SPACE&&durable[1]==before);free_entries=162;assert(network_sync_configure_cloud(&p)==MEMO_CLOUD_OK);
 }
 else if(scenario=="failures"){
  assert(network_sync_configure_cloud(&p)==MEMO_CLOUD_OK);for(int cut=1;cut<=6;cut++){fault_namespace=1;fault=cut;after_commit=false;p.qwen_key=(cut%2)?"changed-one":"changed-two";assert(network_sync_configure_cloud(&p)==MEMO_CLOUD_STORAGE);assert(durable[2]==original_wifi);fault=0;after_commit=false;memo_cloud_config_t actual{};assert(memo_cloud_config_read(&actual));assert(network_sync_configure_cloud(&p)==MEMO_CLOUD_OK);}
 }
 else if(scenario=="read_recovery"){
  assert(network_sync_configure_cloud(&p)==MEMO_CLOUD_OK&&network_sync_set_local_only(false)==MEMO_CLOUD_OK);fault_namespace=1;fault=5;after_commit=false;p.qwen_key="updated-synthetic";assert(network_sync_configure_cloud(&p)==MEMO_CLOUD_STORAGE);assert(cloud_reload_needed&&status_value.cloud_config_error&&!cardless_ready_cache);fault=0;after_commit=false;
  unsigned before=writes;network_sync_get_cloud_config(&pub);assert(pub.result==MEMO_CLOUD_OK&&pub.enabled&&pub.configured&&!cloud_reload_needed&&!status_value.cloud_config_error&&cardless_ready_cache&&writes==before);
 }
 else if(scenario=="gates"){
  for(int cut=0;cut<4;cut++){file_read_active=cut==0;active_record=cut==1?1:0;unique_audio=cut==2;other_http_owner=cut==3;int refresh=cardless_refreshes;assert(network_sync_configure_cloud(&p)==MEMO_CLOUD_BUSY&&cardless_refreshes==refresh);file_read_active=active_record=0;unique_audio=other_http_owner=false;}
  assert(network_sync_prepare_audio(0));assert(network_sync_configure_cloud(&p)==MEMO_CLOUD_BUSY);network_sync_audio_done();assert(network_sync_try_sd_probe());assert(network_sync_configure_cloud(&p)==MEMO_CLOUD_BUSY);network_sync_sd_probe_done();
 }
 else if(scenario=="wifi"){
  WifiConfigurationAp ap;for(int cut=1;cut<=7;cut++){fault_namespace=2;fault=cut;after_commit=false;assert(!ap.Save("new-fixture-"+std::to_string(cut),"synthetic-password"));fault=0;after_commit=false;assert(ap.Save("new-fixture-"+std::to_string(cut),"synthetic-password"));}
 }
 else if(scenario=="input"){
  assert(network_sync_cloud_input_begin());const char *json="{\"workspace\":\"llm-fixture\",\"qwen_key\":\"synthetic-key\",\"siyuan_token\":\"synthetic-token\"}";assert(memo_cloud_input_apply(json,strlen(json))==MEMO_CLOUD_OK);network_sync_cloud_input_end();
  memo_cloud_input_t input;memo_cloud_input_reset(&input,0);for(int i=0;i<1024;i++)assert(!memo_cloud_input_push(&input,'x',1));assert(memo_cloud_input_push(&input,'x',2)==0&&input.overflow);assert(memo_cloud_input_push(&input,'\n',3)==-2);memo_cloud_input_reset(&input,0);assert(memo_cloud_input_push(&input,'x',15000)==-1&&input.used==0);for(char c:std::string("{}\n")){int r=memo_cloud_input_push(&input,c,15001);if(c=='\n')assert(r==1);}memo_cloud_input_reset(&input,0);for(char c:input.bytes)assert(!c);
  unsigned wiped=wiped_arenas;assert(network_sync_cloud_input_begin());
  const char *partial="{\"qwen_key\":\"SYNTHETIC_PRIVATE_VALUE\",}";assert(memo_cloud_input_apply(partial,strlen(partial))==MEMO_CLOUD_INVALID);assert(wiped_arenas==wiped+1&&json_allocations.empty());
  const char *unknown="{\"SYNTHETIC_PRIVATE_FIELD\":\"SYNTHETIC_PRIVATE_VALUE\"}";assert(memo_cloud_input_apply(unknown,strlen(unknown))==MEMO_CLOUD_INVALID);assert(wiped_arenas==wiped+2&&json_allocations.empty());
  fail_secret_arena=true;assert(memo_cloud_input_apply(json,strlen(json))==MEMO_CLOUD_INVALID&&json_allocations.empty());fail_secret_arena=false;network_sync_cloud_input_end();
  for(const char *bad:{"{\"qwen_key\":\"a\",\"qwen_key\":\"b\"}","{\"server_url\":\"old\"}","{\"qwen_key\":\"a\\u0000b\"}","{\"clear_qwen\":1}"}){assert(memo_cloud_input_apply(bad,strlen(bad))==MEMO_CLOUD_INVALID);}
 }
 assert(http_claims==0&&memo_config_activity(&activity_gate)==MEMO_ACTIVITY_IDLE);puts("PASS direct settings/NVS/USB/activity/SSID scenario");
}
'''

class RuntimeTests(unittest.TestCase):
    def test_detail_selection_publishes_latest_request_atomically(self):
        body=extract((ROOT/'main/network_sync.cpp').read_text(),'extern "C" void network_sync_select_record')
        code=r'''#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
static void *status_mutex=(void *)1,*detail_task_handle=(void *)2;
static std::atomic<uint32_t> selected_cardless{0},inspect_id{0};
static uint32_t pinned,detail_revision;static int lock_depth;static std::function<void()> on_unlock;
struct Detail {uint32_t id;int state;};static Detail detail_value;
#define MEMO_CARDLESS_ID_FLAG 0x80000000u
#define MEMO_JOB_CHECKING 3
#define portMAX_DELAY 0
void memo_cardless_select(uint32_t id){pinned=id;}
void xSemaphoreTake(void *,int){assert(lock_depth==0);++lock_depth;}
void xSemaphoreGive(void *){assert(lock_depth==1);--lock_depth;if(on_unlock){auto fn=on_unlock;on_unlock={};fn();}}
void xTaskNotifyGive(void *){}
// BODY
int main(){
 on_unlock=[](){network_sync_select_record(0x8000002du);};network_sync_select_record(61);
 assert(detail_value.id==0x8000002du && inspect_id==detail_value.id && pinned==detail_value.id && selected_cardless==detail_value.id);
 on_unlock=[](){network_sync_select_record(0);};network_sync_select_record(61);
 assert(detail_value.id==0 && inspect_id==0 && pinned==0 && selected_cardless==0);
 assert(detail_revision==4 && lock_depth==0);
}
'''.replace('// BODY',body)
        with tempfile.TemporaryDirectory(prefix='detail-select-',dir=ROOT/'private/host-tests') as directory:
            file=Path(directory)/'check.cpp';file.write_text(code);binary=Path(directory)/'check'
            subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(file),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

    def test_actual_runtime_and_nvs_and_wifi_faults(self):
        (ROOT/'private/host-tests').mkdir(mode=0o700,exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='service-runtime-',dir=ROOT/'private/host-tests') as directory:
            temp=Path(directory)
            for name,text in HEADERS.items():
                file=temp/name;file.parent.mkdir(parents=True,exist_ok=True);file.write_text(text)
            source=(ROOT/'main/network_sync.cpp').read_text()
            markers=['bool audio_busy','class HttpGuard','class ConfigCommitGuard','void status_update','void set_error','void publish_cloud_status','void refresh_service_status',
                     'extern "C" void network_sync_get_status','extern "C" void network_sync_get_cloud_config',
                     'void cloud_saved','extern "C" memo_cloud_result_t network_sync_configure_cloud','extern "C" memo_cloud_result_t network_sync_set_local_only',
                     'extern "C" bool network_sync_cloud_input_begin','extern "C" memo_cloud_result_t network_sync_cloud_input_save','extern "C" void network_sync_cloud_input_end',
                     'extern "C" bool network_sync_prepare_audio','extern "C" void network_sync_audio_done',
                     'extern "C" bool network_sync_try_sd_probe','extern "C" void network_sync_sd_probe_done',
                     'extern "C" bool network_sync_power_busy','extern "C" bool network_sync_power_suspend',
                     'bool begin_sd_io','void end_sd_io']
            bodies='\n'.join(extract(source,marker)+(';' if marker.startswith('class ') else '') for marker in markers)
            ap=(ROOT/'components/esp-wifi-connect/wifi_configuration_ap.cc').read_text()
            bodies+='\n'+extract(ap,'bool WifiConfigurationAp::Save')
            manager=(ROOT/'components/esp-wifi-connect/wifi_manager.cc').read_text()
            bodies+='\n'+extract(manager,'bool WifiManager::Initialize')
            begin=extract(source,'config.config_commit_begin=');begin=begin.split('=',1)[1]
            end=extract(source,'config.config_commit_end=');end=end.split('=',1)[1]
            harness=(HARNESS+RUNTIME_MAIN).replace('// __PRODUCTION_BODIES__',bodies).replace('// __WIFI_BEGIN__',begin).replace('// __WIFI_END__',end)
            file=temp/'runtime.cpp';file.write_text(harness)
            includes=['-I',str(temp),'-I',str(ROOT/'main'),'-I',str(ROOT/'components/esp-wifi-connect/include'),'-I',str(ROOT/'.tools/esp-idf/components/json/cJSON')]
            from cloud_settings_fixture import compile_objects
            objects=compile_objects(temp,includes)
            binary=temp/'runtime'
            subprocess.run(['c++','-std=c++17','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-DMEMO_HOST_TEST',*includes,str(file),str(ROOT/'components/esp-wifi-connect/ssid_manager.cc'),*objects,'-o',str(binary)],check=True)
            results={scenario:subprocess.run([str(binary),scenario]).returncode for scenario in ('settings','noop','boundaries','capacity','failures','wifi','gates','input','read_recovery')}
            self.assertEqual(results,{name:0 for name in results})

if __name__=='__main__': unittest.main(verbosity=2)
