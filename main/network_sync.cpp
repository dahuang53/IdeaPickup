#include "network_sync.h"
#include "board.h"
#include "memo_cardless.h"
#include "network_sync_state.h"
#include "memo_network_config_nvs.h"
#include "memo_service_pairing.h"
#include "memo_service_portal.h"
extern "C" bool memo_portal_status(char *,size_t);
extern "C" bool memo_portal_apply(const char *,size_t,char *,size_t);
#include "memo_sd_sync.h"
#include "memo_http_transport.h"
#include "memo_record_ids.h"
#include <dirent.h>

#include "memo_store.h"
#include "wifi_manager.h"
#include "ssid_manager.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <inttypes.h>
#include <string>
#include <ctime>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

namespace {

constexpr const char *TAG = "MEMO_NET";
constexpr uint32_t RETRY_DELAY_MS = 15'000;
constexpr uint32_t NEW_RECORD_SETTLE_MS = 4'000;

SemaphoreHandle_t status_mutex;
SemaphoreHandle_t http_mutex;
std::atomic<bool> stream_http_active{false};
std::atomic<bool> pairing_http_active{false};
std::atomic<uint32_t> selected_cardless{0};
std::atomic<uint32_t> cancel_request{0};
class HttpGuard {
public:
    HttpGuard() { if(http_mutex)xSemaphoreTakeRecursive(http_mutex,portMAX_DELAY); }
    ~HttpGuard() { if(http_mutex)xSemaphoreGiveRecursive(http_mutex); }
};
memo_network_status_t status_value{};
memo_config_activity_gate_t activity_gate{};
// Both owners exclude background SD I/O; only AUDIO permits audio re-entry.
bool audio_busy() {
    auto owner=memo_config_activity(&activity_gate);
    return owner==MEMO_ACTIVITY_AUDIO || owner==MEMO_ACTIVITY_MEDIA;
}
std::atomic<bool> storage_available{false};
std::atomic<bool> sd_scan_requested{true};
std::atomic<bool> sd_media_work{false};
std::atomic<uint32_t> sd_directory_scans{0};
std::atomic<uint32_t> file_read_active{0};
std::atomic<uint32_t> latest_id{0};
std::atomic<int64_t> latest_change_us{0};
std::atomic<bool> power_suspended{false};
TaskHandle_t sync_task_handle, detail_task_handle;
// Lifelong stacks remain internal for NVS/TLS. Reserve them before heap users
// fragment memory needed for the temporary 8 KiB configuration HTTPD stack.
StaticTask_t sync_task_storage,detail_task_storage;
StackType_t sync_task_stack[16384/sizeof(StackType_t)];
StackType_t detail_task_stack[10240/sizeof(StackType_t)];
bool sntp_initialized;
std::atomic<uint32_t> inspect_id{0};
/* Guarded by status_mutex. Durable new-record intent or explicit user request
 * adds work; a directory maximum alone never grants automatic eligibility. */
memo_sync_queue_t sync_queue{};
std::atomic<uint32_t> active_record{0};
std::atomic<memo_job_state_t> active_state{MEMO_JOB_UNKNOWN};
memo_record_sync_t detail_value{};
uint32_t detail_revision;

class ConfigGuard {
public:
    memo_config_t *value;
    ConfigGuard():value(memo_network_config_nvs_lock()) {}
    ~ConfigGuard(){if(value)memo_network_config_nvs_unlock();}
};
class ConfigCommitGuard {
public:
    bool acquired;
    ConfigCommitGuard():acquired(memo_config_activity_acquire(&activity_gate,MEMO_ACTIVITY_COMMIT)) {
        if(acquired && (file_read_active.load() || stream_http_active.load() || active_record.load() || memo_cardless_busy())) {
            memo_config_activity_release(&activity_gate,MEMO_ACTIVITY_COMMIT);acquired=false;
        }
    }
    ~ConfigCommitGuard(){if(acquired)memo_config_activity_release(&activity_gate,MEMO_ACTIVITY_COMMIT);}
};
bool current_service(memo_config_snapshot_t &out)
{
    ConfigGuard guard;
    return guard.value && memo_config_credentials(guard.value,&out)==MEMO_CONFIG_OK;
}
void cache_path(char *path, size_t size, uint32_t id) {
    snprintf(path,size,"/sdcard/memos/sync_r1_%010" PRIu32 ".job",id);
}
void job_state(uint32_t id, memo_job_state_t state, unsigned percent=0) {
    if(active_record.load()==id)active_state.store(state);
    xSemaphoreTake(status_mutex,portMAX_DELAY);
    if(detail_value.id==id) {detail_value.state=state;detail_value.upload_percent=percent;++detail_revision;}
    xSemaphoreGive(status_mutex);

}
void status_update(const std::function<void(memo_network_status_t &)> &fn)
{
    if (!status_mutex) return;
    xSemaphoreTake(status_mutex, portMAX_DELAY);
    fn(status_value);
    xSemaphoreGive(status_mutex);
}

void set_error(const char *message)
{
    status_update([message](memo_network_status_t &s) {
        snprintf(s.error, sizeof(s.error), "%s", message ? message : "");
    });
    if (message && *message) ESP_LOGW(TAG, "%s", message);
}

esp_err_t read_http_body(esp_http_client_handle_t client, std::string &body)
{
    char block[384];
    body.clear();
    for (;;) {
        int count = esp_http_client_read(client, block, sizeof(block));
        if (count < 0) return ESP_FAIL;
        if (count == 0) return esp_http_client_is_complete_data_received(client)?ESP_OK:ESP_FAIL;
        if (body.size() + static_cast<size_t>(count) > 2048) return ESP_ERR_INVALID_SIZE;
        body.append(block, count);
    }
    return ESP_OK;
}

bool ensure_clock(const char *base)
{
    // The validated literal LAN HTTP path does not depend on internet NTP.
    if (base && !strncmp(base,"http://",7)) return true;
    constexpr time_t MIN_VALID_TIME = 1'704'067'200; // 2024-01-01 UTC
    time_t now = 0;
    time(&now);
    if (now >= MIN_VALID_TIME) return true;
    {
    // Serialize initialization only; waiting for a clock must not block saves.
    HttpGuard guard;
    if (!sntp_initialized) {
        /* Some home proxy/DNS setups return 198.18.0.0/15 fake-IP answers for
           hostnames but do not proxy UDP/123.  Literal public NTP endpoints
           keep first-time HTTPS enrollment independent of that DNS mode. */
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
            3, ESP_SNTP_SERVER_LIST("203.107.6.88", "106.55.184.199", "162.159.200.1"));
        esp_err_t error = esp_netif_sntp_init(&config);
        if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(error));
            return false;
        }
        sntp_initialized = true;
    }
    }
    esp_err_t result = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15'000));
    time(&now);
    ESP_LOGI(TAG, "CLOCK_SYNC result=%s unix=%lld", esp_err_to_name(result),
             static_cast<long long>(now));
    return now >= MIN_VALID_TIME;
}

void refresh_service_status()
{
    memo_config_public_t config;network_sync_get_service_config(&config);
    status_update([&config](memo_network_status_t &s){s.configured=config.configured;});
}
bool pairing_is_current(const memo_config_snapshot_t &expected)
{
    ConfigGuard guard;memo_config_snapshot_t current{};
    return guard.value && memo_config_read_snapshot(guard.value,&current)==MEMO_CONFIG_OK &&
        current.enabled && current.phase==MEMO_SERVICE_PAIRING &&
        current.generation==expected.generation && !strcmp(current.enrollment_id,expected.enrollment_id);
}
int pairing_request(void *context,const char *base,const char *method,const char *path,const char *token,
                    const char *payload,char *response,size_t capacity)
{
    auto *expected=static_cast<memo_config_snapshot_t *>(context);
    if(!response || !capacity)return 0;
    // The already-paired SD retry caller shares this transport while holding
    // HttpGuard; only enrollment requests carry a pending-generation guard.
    if(expected && !pairing_is_current(*expected))return 0;
    response[0]=0;char url[1024];
    if(memo_config_join_url(base,path,url,sizeof(url))!=MEMO_CONFIG_OK)return 0;
    esp_http_client_config_t cfg{};cfg.url=url;cfg.crt_bundle_attach=esp_crt_bundle_attach;
    cfg.disable_auto_redirect=true;cfg.timeout_ms=15000;cfg.buffer_size=1024;
    MemoHttpTransport transport(cfg);if(!transport.ready())return 0;
    esp_http_client_handle_t client=esp_http_client_init(&cfg);if(!client)return 0;
    if(!strcmp(method,"POST"))esp_http_client_set_method(client,HTTP_METHOD_POST);
    if(token && *token){std::string auth=std::string("Bearer ")+token;esp_http_client_set_header(client,"Authorization",auth.c_str());}
    esp_http_client_set_header(client,"User-Agent","VoiceMemoDevice/0.4");
    if(payload)esp_http_client_set_header(client,"Content-Type","application/json");
    size_t length=payload?strlen(payload):0,sent=0;
    esp_err_t result=esp_http_client_open(client,(int)length);
    while(result==ESP_OK && sent<length){int n=esp_http_client_write(client,payload+sent,length-sent);if(n<=0){result=ESP_FAIL;break;}sent+=(size_t)n;}
    int code=0;std::string body;
    if(result==ESP_OK && esp_http_client_fetch_headers(client)>=0){
        code=esp_http_client_get_status_code(client);
        if(read_http_body(client,body)!=ESP_OK || body.size()>=capacity)code=0;
    }
    if(code)memcpy(response,body.c_str(),body.size()+1);
    esp_http_client_close(client);esp_http_client_cleanup(client);
    // A save can supersede this snapshot while its private HTTP handle waits.
    if(expected && !pairing_is_current(*expected)){response[0]=0;return 0;}
    return code;
}
memo_config_result_t pairing_bind(void *context,memo_config_snapshot_t *pending,const memo_server_uuid_t *instance)
{
    ConfigCommitGuard commit;if(!commit.acquired)return MEMO_CONFIG_BUSY;
    ConfigGuard guard;if(!guard.value)return MEMO_CONFIG_STORAGE_ERROR;
    memo_config_result_t result=memo_config_bind_instance(guard.value,pending->generation,instance);
    if(result==MEMO_CONFIG_OK)result=memo_config_read_snapshot(guard.value,pending);
    if(result==MEMO_CONFIG_OK && context)*static_cast<memo_config_snapshot_t *>(context)=*pending;
    return result;
}
void recover_pairing()
{
    if(power_suspended.load())return;
    auto &wifi=WifiManager::GetInstance();if(!wifi.IsConnected())return;
    // Pairing runs on this one worker with a private HTTP handle. Keep only
    // short configuration locks; snapshots/generations reject obsolete results.
    memo_config_snapshot_t pending{};
    {ConfigGuard guard;if(!guard.value)return;
        if(guard.value->needs_reload){ConfigCommitGuard commit;if(!commit.acquired)return;memo_network_config_nvs_reload();}
        if(memo_config_read_snapshot(guard.value,&pending)!=MEMO_CONFIG_OK){refresh_service_status();return;}}
    refresh_service_status();
    if(!pending.enabled || pending.phase!=MEMO_SERVICE_PAIRING)return;
    struct PairingActivity {
        PairingActivity(){pairing_http_active.store(true);}
        ~PairingActivity(){pairing_http_active.store(false);}
    } active_pairing;
    bool clock_ready=ensure_clock(pending.base_url);
    if(!pairing_is_current(pending))return;
    memo_pairing_io_t io{&pending,pairing_request,pairing_bind};memo_pairing_receipt_t receipt{};
    memo_pair_result_t result=clock_ready?memo_pairing_run(pending,&io,&receipt):MEMO_PAIR_NETWORK;
    ConfigCommitGuard commit;if(!commit.acquired)return;
    ConfigGuard guard;if(!guard.value || !pairing_is_current(pending))return;
    memo_config_snapshot_t current{};
    if(memo_config_read_snapshot(guard.value,&current)!=MEMO_CONFIG_OK)return;
    if(!clock_ready){set_error("网络时间同步失败，配对将自动重试");return;}
    if(result==MEMO_PAIR_OK || result==MEMO_PAIR_REJECTED || result==MEMO_PAIR_INSTANCE_CHANGED){
        memo_config_result_t saved=result==MEMO_PAIR_OK?memo_config_finish_pairing(guard.value,current.generation,&receipt.instance):
            memo_config_reject_pairing(guard.value,current.generation);
        if(saved!=MEMO_CONFIG_OK){set_error("服务配置保存结果未确认，稍后自动读取恢复");refresh_service_status();return;}
        refresh_service_status();set_error(result==MEMO_PAIR_OK?nullptr:result==MEMO_PAIR_INSTANCE_CHANGED?
            "服务实例已更换，请使用新的配对码":"配对码无效、过期或设备已撤销，请重新配对");
    } else if(result==MEMO_PAIR_INVALID_RESPONSE)set_error("服务接口不兼容，请检查中继版本；更换配对码无法解决");
    else if(result!=MEMO_PAIR_BUSY)set_error("配对尚未完成，将自动重试；本地录音可用");
}


int remote_status(uint32_t record_id, std::string &body)
{
    HttpGuard guard;
    memo_config_snapshot_t service{};
    if(!current_service(service))return 0;
    char path[96],url[1024];
    snprintf(path,sizeof(path),"/v1/memos/%s/%" PRIu32,service.device_id,record_id);
    if(memo_config_join_url(service.base_url,path,url,sizeof(url))!=MEMO_CONFIG_OK)return 0;
    esp_http_client_config_t config{};
    config.url = url;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.disable_auto_redirect = true;
    config.timeout_ms = 20'000;
    config.buffer_size = 1024;
    MemoHttpTransport transport(config);if(!transport.ready())return 0;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return 0;
    std::string authorization = std::string("Bearer ") + service.token;
    esp_http_client_set_header(client, "Authorization", authorization.c_str());
    esp_http_client_set_header(client, "User-Agent", "MeowVoiceMemoDevice/0.2");
    esp_err_t result = esp_http_client_open(client, 0);
    int code = 0;
    if (result == ESP_OK) {
        if(esp_http_client_fetch_headers(client)>=0)code = esp_http_client_get_status_code(client);
        if (read_http_body(client, body) != ESP_OK) code = 0;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return code;
}

int upload_file(const memo_record_t &record, uint32_t file_size, const char *cancel_token, std::string &body)
{
    HttpGuard guard;
    memo_config_snapshot_t service{};char url[1024];
    if(!current_service(service) || memo_config_join_url(service.base_url,"/v1/memos",url,sizeof(url))!=MEMO_CONFIG_OK)return 0;
    file_read_active.fetch_add(1);
    if(audio_busy()){file_read_active.fetch_sub(1);return 0;}
    FILE *file = fopen(record.path, "rb");
    if (!file) {file_read_active.fetch_sub(1);return 0;}
    esp_http_client_config_t config{};
    config.url = url;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.disable_auto_redirect = true;
    config.timeout_ms = 20'000; // Relay keeps processing; subsequent GETs track completion.
    config.buffer_size = 1024;
    config.buffer_size_tx = 4096;
    // Upload admission returns asynchronously; processing is tracked by GET.
    // Keep TCP failure bounded even when a proxy loses the admission response.
    config.keep_alive_enable = true;
    config.keep_alive_idle = 15;
    config.keep_alive_interval = 15;
    config.keep_alive_count = 4;
    MemoHttpTransport transport(config);
    esp_http_client_handle_t client = transport.ready()?esp_http_client_init(&config):nullptr;
    if (!client) {
        fclose(file);
        file_read_active.fetch_sub(1);
        return 0;
    }
    std::string authorization = std::string("Bearer ") + service.token;
    char recording_id[16];
    snprintf(recording_id, sizeof(recording_id), "%" PRIu32, record.id);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Authorization", authorization.c_str());
    esp_http_client_set_header(client, "Content-Type", "audio/wav");
    if(cancel_token && cancel_token[0])esp_http_client_set_header(client,"X-Cancel-Token",cancel_token);
    esp_http_client_set_header(client, "X-Device-ID", service.device_id);
    esp_http_client_set_header(client, "X-Recording-ID", recording_id);
    esp_http_client_set_header(client, "User-Agent", "MeowVoiceMemoDevice/0.2");
    esp_err_t result = esp_http_client_open(client, file_size);
    char *dma_buffer=static_cast<char *>(heap_caps_malloc(4096,MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA));
    if(dma_buffer && setvbuf(file,dma_buffer,_IOFBF,4096)!=0){heap_caps_free(dma_buffer);dma_buffer=nullptr;}
    uint8_t block[4096];
    uint32_t sent = 0;
    while (result == ESP_OK && sent < file_size && !audio_busy() && cancel_request.load()!=record.id) {
        size_t want = file_size - sent > sizeof(block) ? sizeof(block) : file_size - sent;
        size_t count = fread(block, 1, want, file);
        transport.write_window();
        if (count != want || esp_http_client_write(client, reinterpret_cast<const char *>(block), count) != (int)count) {
            result = ESP_FAIL;
            break;
        }
        sent += count;
        xSemaphoreTake(status_mutex,portMAX_DELAY);
        if(detail_value.id==record.id)detail_value.upload_percent=(unsigned)((uint64_t)sent*100/file_size);
        xSemaphoreGive(status_mutex);
        taskYIELD();
    }
    fclose(file);
    heap_caps_free(dma_buffer);
    file_read_active.fetch_sub(1);
    int code = 0;
    if (result == ESP_OK && sent == file_size && !audio_busy() && cancel_request.load()!=record.id) {
        job_state(record.id,MEMO_JOB_WAITING,100);
        if(esp_http_client_fetch_headers(client)>=0)code = esp_http_client_get_status_code(client);
        if (read_http_body(client, body) != ESP_OK) code = 0;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return code;
}

bool begin_sd_io(void *) {file_read_active.fetch_add(1);if(memo_config_activity(&activity_gate)!=MEMO_ACTIVITY_IDLE){file_read_active.fetch_sub(1);return false;}return true;}
void end_sd_io(void *) {file_read_active.fetch_sub(1);}
int sd_status(void *,uint32_t id,char *out,size_t capacity){job_state(id,MEMO_JOB_CHECKING);memo_config_snapshot_t service{};if(!current_service(service)||!ensure_clock(service.base_url))return 0;std::string body;int code=remote_status(id,body);if(body.size()>=capacity)return 0;snprintf(out,capacity,"%s",body.c_str());return code;}
int sd_upload(void *,const memo_record_t *record,const char *cancel_token,char *out,size_t capacity){
 job_state(record->id,MEMO_JOB_UPLOADING);
 memo_config_snapshot_t service{};if(!current_service(service)||!ensure_clock(service.base_url))return 0;
 std::string body;int code=upload_file(*record,record->data_bytes+44,cancel_token,body);if(body.size()>=capacity)return 0;snprintf(out,capacity,"%s",body.c_str());return code;
}
int sd_retry(void *,uint32_t id,const char *cancel_token,char *out,size_t capacity){
 memo_config_snapshot_t service{};if(!current_service(service)||!ensure_clock(service.base_url))return 0;
 char path[160];snprintf(path,sizeof(path),"/v1/memos/%s/%" PRIu32 "/retry",service.device_id,id);
 char payload[80];snprintf(payload,sizeof(payload),"{\"cancel_token\":\"%s\"}",cancel_token?cancel_token:"");
 return pairing_request(nullptr,service.base_url,"POST",path,service.token,payload,out,capacity);
}
int sd_cancel(void *,uint32_t id,const char *cancel_token,char *out,size_t capacity){
 memo_network_status_t network{};network_sync_get_status(&network);
 memo_config_snapshot_t service{};
 if(!network.connected||!current_service(service)||!ensure_clock(service.base_url))return 0;
 char path[160];snprintf(path,sizeof(path),"/v1/memos/%s/%" PRIu32 "/cancel",service.device_id,id);
 char payload[80];snprintf(payload,sizeof(payload),"{\"cancel_token\":\"%s\"}",cancel_token?cancel_token:"");
 return pairing_request(nullptr,service.base_url,"POST",path,service.token,payload,out,capacity);
}
const char *sync_error(const char *s){
 if(!strcmp(s,"credentials_invalid"))return "设备凭据失效，请重新配对";
 if(!strcmp(s,"recording_changed")||!strcmp(s,"audio_identity_mismatch"))return "录音内容已变化，未复用同步结果";
 if(!strcmp(s,"cancel_pending"))return "正在取消，等待联网确认";
 if(!strcmp(s,"cancel_too_late"))return "已经开始投递，请等待处理结果";
 if(!strcmp(s,"server_task_missing"))return "服务器任务未找到，请查看后台";
 if(!strcmp(s,"different_service"))return "当前服务尚未同步此录音";
 if(!strcmp(s,"sync_metadata_write_failed")||!strcmp(s,"sync_metadata_unreadable"))return "同步状态保存失败，录音仍在本地";
 if(!strcmp(s,"recording_unreadable"))return "录音文件不可读，请检查存储卡";
 if(!strcmp(s,"audio_busy")||!strcmp(s,"no_pending_intent"))return "";
 return s[0]?"同步未完成，稍后重试":"";
}
bool sync_one(uint32_t id,bool manual=false,bool inspect=false,bool cancel=false){
 HttpGuard guard;memo_sync_target_t target{};
 if(!network_sync_current_target(&target)&&!cancel)return false;
 char wav[MEMO_PATH_MAX],cache[160];snprintf(wav,sizeof(wav),"/sdcard/memos/memo_%010" PRIu32 ".wav",id);cache_path(cache,sizeof(cache),id);
 memo_sd_sync_io_t io{nullptr,begin_sd_io,end_sd_io,sd_status,sd_upload,sd_retry,sd_cancel};memo_sd_sync_result_t result{};
 memo_sd_sync_attempt(wav,cache,id,&target,cancel?4:inspect?2:manual?1:0,&io,&result);
 memo_job_state_t presented=result.state;
 if(result.error[0]&&strcmp(result.error,"audio_busy")&&result.state==MEMO_JOB_LOCAL)presented=MEMO_JOB_FAILED;
 job_state(id,presented);
 xSemaphoreTake(status_mutex,portMAX_DELAY);
 if(detail_value.id==id){detail_value.can_retry=result.can_retry;detail_value.can_cancel=result.can_cancel;snprintf(detail_value.error,sizeof(detail_value.error),"%s",sync_error(result.error));}
 if(result.state==MEMO_JOB_SENT && id>status_value.last_uploaded_id)status_value.last_uploaded_id=id;
 xSemaphoreGive(status_mutex);
 set_error(sync_error(result.error));
 return !cancel || !strcmp(result.error,"cancel_too_late") || result.state==MEMO_JOB_CANCELLING || result.state==MEMO_JOB_CANCELLED || result.state==MEMO_JOB_SENT || result.state==MEMO_JOB_UNCERTAIN ||
        (!result.pending && (result.state==MEMO_JOB_REJECTED || result.state==MEMO_JOB_FAILED));
}
void defer_detail(uint32_t id) {
 xSemaphoreTake(status_mutex,portMAX_DELAY);
 if(detail_value.id==id){uint32_t empty=0;inspect_id.compare_exchange_strong(empty,id);}
 xSemaphoreGive(status_mutex);
}
void inspect_record(uint32_t id){
 if(id & MEMO_CARDLESS_ID_FLAG){memo_cardless_record_t local{};memo_cardless_get(id,&local);return;}
 if(!storage_available.load())return;
 uint32_t revision;
 {xSemaphoreTake(status_mutex,portMAX_DELAY);
  if(detail_value.id!=id){xSemaphoreGive(status_mutex);return;}
  revision=detail_revision;xSemaphoreGive(status_mutex);}
 if(!begin_sd_io(nullptr)){
  defer_detail(id);return;
 }
 memo_sync_target_t target{};network_sync_current_target(&target);
 char wav[MEMO_PATH_MAX],cache[160];snprintf(wav,sizeof(wav),"/sdcard/memos/memo_%010" PRIu32 ".wav",id);cache_path(cache,sizeof(cache),id);
 memo_sd_sync_io_t io{nullptr,begin_sd_io,end_sd_io,sd_status,sd_upload,sd_retry,sd_cancel};memo_sd_sync_result_t result{};
 memo_sd_sync_attempt(wav,cache,id,&target,3,&io,&result);
 end_sd_io(nullptr);
 if(!strcmp(result.error,"audio_busy")){
  defer_detail(id);return;
 }
 xSemaphoreTake(status_mutex,portMAX_DELAY);
 if(detail_value.id==id && detail_revision==revision){
  bool active=active_record.load()==id;
  detail_value.state=active?active_state.load():result.state;detail_value.can_retry=!active && result.can_retry;detail_value.can_cancel=result.can_cancel;
  if(!active)snprintf(detail_value.error,sizeof(detail_value.error),"%s",sync_error(result.error));
 }
 xSemaphoreGive(status_mutex);
}
void detail_task(void *) {
 for(;;){
  ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
  uint32_t id=inspect_id.exchange(0);
  if(id)inspect_record(id);
  if(inspect_id.load()){vTaskDelay(pdMS_TO_TICKS(250));xTaskNotifyGive(detail_task_handle);}
 }
}
bool pending_record(const memo_sync_target_t &target,bool &manual,uint32_t &next){
 static uint32_t after=0;manual=false;next=0;
 if(!begin_sd_io(nullptr))return false;
 bool healthy=board_sd_check()==ESP_OK;
 if(healthy) {
  sd_directory_scans.fetch_add(1);
  next=memo_sd_sync_pending("/sdcard/memos",&target,&after,&manual,&healthy);
 }
 end_sd_io(nullptr);return healthy;
}
/* Consume the dirty flag before I/O: an event arriving during a scan remains
 * pending. A busy or missing card never masquerades as an empty directory. */
bool discover_sd_work(const memo_sync_target_t &target) {
 if(!sd_scan_requested.exchange(false))return true;
 bool manual=false;uint32_t next=0;
 if(!pending_record(target,manual,next)) {sd_scan_requested.store(true);sd_media_work.store(true);return false;}
 xSemaphoreTake(status_mutex,portMAX_DELAY);
 if(next && !sync_queue.active_id && !sync_queue.pending_id) {
  if(manual)memo_sync_queue_offer_manual(&sync_queue,next,true);
  else memo_sync_queue_offer_auto(&sync_queue,next);
 }
 sd_media_work.store(next || sync_queue.active_id || sync_queue.pending_id);
 xSemaphoreGive(status_mutex);return true;
}
bool persist_manual(uint32_t id){
 if(!id)return true;
 if(!begin_sd_io(nullptr))return false;
 memo_sync_target_t target{};memo_record_t record{};memo_sync_entry_t e{};
 char wav[MEMO_PATH_MAX],cache[160];snprintf(wav,sizeof(wav),"/sdcard/memos/memo_%010" PRIu32 ".wav",id);cache_path(cache,sizeof(cache),id);
 bool ok=network_sync_current_target(&target)&&memo_record_ids_observe(id)&&!memo_store_validate(wav,&record);
 memo_cache_result_t r=ok?memo_sync_entry_read(cache,id,&e):MEMO_CACHE_IO_ERROR;
 if(r==MEMO_CACHE_IO_ERROR)ok=false;
 if(ok && r==MEMO_CACHE_OK && (e.state==MEMO_JOB_SENT||e.state==MEMO_JOB_UNCERTAIN))ok=false;
 if(ok){
  if(r!=MEMO_CACHE_OK||!memo_sync_target_matches(&e.target,&target,!e.bound)){
   e={};e.id=id;e.target=target;e.state=MEMO_JOB_LOCAL;
  }
  e.manual=e.automatic=true;ok=memo_sync_entry_write(cache,&e);
 }
 end_sd_io(nullptr);return ok;
}
void sync_task(void *)
{
    int64_t next_pairing_us = 0, next_scan_us = 0;
    bool previously_eligible=false;
    for (;;) {
        auto &wifi = WifiManager::GetInstance();
        wifi.ProcessConfigConnection();
        bool connected = wifi.IsConnected();
        status_update([connected](memo_network_status_t &s) { s.connected = connected; });
        int64_t now = esp_timer_get_time();
        if (now >= next_pairing_us) {
            recover_pairing();
            next_pairing_us = esp_timer_get_time() + RETRY_DELAY_MS * 1000LL;
        }
        uint32_t cancelled=cancel_request.load();
        if(cancelled && storage_available.load() && !audio_busy() && !wifi.IsConfigMode() &&
           !power_suspended.load() && !memo_cardless_busy()) {
            if(sync_one(cancelled,false,false,true)) {
                xSemaphoreTake(status_mutex,portMAX_DELAY);
                if(sync_queue.active_id==cancelled)memo_sync_queue_finish(&sync_queue,cancelled);
                if(sync_queue.pending_id==cancelled){sync_queue.pending_id=0;sync_queue.pending_manual=sync_queue.pending_persisted=false;}
                xSemaphoreGive(status_mutex);
                cancel_request.compare_exchange_strong(cancelled,0);
                sd_scan_requested.store(true);
            }
        }
        memo_cardless_network_step(); // Shared HTTP owner; independent of SD presence.
        memo_network_status_t snapshot;network_sync_get_status(&snapshot);
        bool eligible=snapshot.connected && snapshot.configured && !snapshot.provisioning && !power_suspended.load();
        if(eligible && !previously_eligible) {sd_scan_requested.store(true);next_scan_us=0;}
        previously_eligible=eligible;
        bool available=storage_available.load() && !snapshot.provisioning && !power_suspended.load() && !audio_busy() && !memo_cardless_busy();
        if(available) {
            HttpGuard service_guard;
            uint32_t persist_id=0;
            xSemaphoreTake(status_mutex,portMAX_DELAY);
            if(sync_queue.pending_manual && !sync_queue.pending_persisted)
                persist_id=sync_queue.pending_id;
            xSemaphoreGive(status_mutex);
            if(persist_id) {
                bool saved=persist_manual(persist_id);
                xSemaphoreTake(status_mutex,portMAX_DELAY);
                memo_sync_queue_manual_persisted(&sync_queue,persist_id,saved);
                xSemaphoreGive(status_mutex);
                job_state(persist_id,saved?MEMO_JOB_QUEUED:MEMO_JOB_FAILED);
            }
            if(snapshot.connected && snapshot.configured) {
                memo_sync_target_t target{};
                if(esp_timer_get_time()>=next_scan_us && network_sync_current_target(&target) &&
                   !discover_sd_work(target))next_scan_us=esp_timer_get_time()+RETRY_DELAY_MS*1000LL;
                bool settled=esp_timer_get_time()-latest_change_us.load()>=NEW_RECORD_SETTLE_MS*1000LL;
                uint32_t id;bool manual;
                xSemaphoreTake(status_mutex,portMAX_DELAY);
                memo_sync_queue_start(&sync_queue,settled);
                id=sync_queue.active_id;
                manual=sync_queue.active_manual;
                xSemaphoreGive(status_mutex);
                if(id && !audio_busy()) {
                    active_state.store(MEMO_JOB_CHECKING);
                    active_record.store(id);
                    status_update([](memo_network_status_t &s){s.syncing=true;});
                    sync_one(id,manual);
                    status_update([](memo_network_status_t &s){s.syncing=false;});
                    active_record.store(0);
                    auto result=active_state.load();
                    if(memo_sync_device_attempt_done(result)) {
                        xSemaphoreTake(status_mutex,portMAX_DELAY);
                        memo_sync_queue_finish(&sync_queue,id);
                        xSemaphoreGive(status_mutex);
                        sd_scan_requested.store(true);
                        /* Server-processing states retain durable intent. Do
                         * not spin on that same entry via self-notification. */
                        bool waiting=result==MEMO_JOB_WAITING || result==MEMO_JOB_RECEIVED ||
                            result==MEMO_JOB_TRANSCRIBING || result==MEMO_JOB_DELIVERING;
                        next_scan_us=(waiting || result==MEMO_JOB_FAILED)?esp_timer_get_time()+RETRY_DELAY_MS*1000LL:0;
                        if(manual)persist_manual(0);
                        ESP_LOGI(TAG,"SYNC_SCHEDULE_COMPLETE id=%" PRIu32 " manual=%d state=%d",
                                 id,manual,result);
                        if(sync_task_handle)xTaskNotifyGive(sync_task_handle);
                    }
                }
            }

        }
        ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(memo_cardless_busy() ? 250 : wifi.IsConfigMode() ? 1000 : RETRY_DELAY_MS));
    }
}

} // namespace

extern "C" esp_err_t network_sync_init(uint32_t existing_latest_id)
{
    status_mutex = xSemaphoreCreateMutex();
    http_mutex = xSemaphoreCreateRecursiveMutex();
    if (!status_mutex || !http_mutex) return ESP_ERR_NO_MEM;
    uint32_t observed_latest = latest_id.load();
    if (observed_latest > existing_latest_id) existing_latest_id = observed_latest;
    latest_id.store(existing_latest_id);
    latest_change_us.store(esp_timer_get_time());
    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_STA), TAG, "read Wi-Fi MAC");
    status_update([&mac](memo_network_status_t &s) {
        snprintf(s.device_id, sizeof(s.device_id), "%s-%02x%02x%02x%02x%02x%02x",
                 board_device_id_prefix(), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    });
    WifiManagerConfig config;
    config.ssid_prefix = "MeowInbox";
    config.language = "zh-CN";
    config.station_hostname = "meow-inbox";
    memo_service_portal_register(memo_portal_status,memo_portal_apply);
    config.config_commit_begin=[](){
        if(!http_mutex || xSemaphoreTakeRecursive(http_mutex,0)!=pdTRUE)return false;
        if(!memo_config_activity_acquire(&activity_gate,MEMO_ACTIVITY_COMMIT)){xSemaphoreGiveRecursive(http_mutex);return false;}
        if(file_read_active.load()){
            memo_config_activity_release(&activity_gate,MEMO_ACTIVITY_COMMIT);xSemaphoreGiveRecursive(http_mutex);return false;
        }
        return true;
    };
    config.config_commit_end=[](){memo_config_activity_release(&activity_gate,MEMO_ACTIVITY_COMMIT);xSemaphoreGiveRecursive(http_mutex);};
    auto &manager = WifiManager::GetInstance();
    if (!manager.Initialize(config)) {
        set_error("网络配置存储初始化失败");
        return ESP_FAIL;
    }
    esp_err_t service_init=memo_network_config_nvs_init();
    refresh_service_status();
    uint32_t cursor=0,last_ok=0;
    memo_config_public_t current;network_sync_get_service_config(&current);bool configured=current.configured;
    if(service_init!=ESP_OK)set_error("服务配置不可读；本地录音仍可用");
    status_update([configured, cursor, last_ok](memo_network_status_t &s) {
        s.configured = configured; s.cursor_id = cursor; s.last_uploaded_id = last_ok;
    });
    manager.SetEventCallback([](WifiEvent event, const std::string &data) {
        auto &wifi = WifiManager::GetInstance();
        switch (event) {
        case WifiEvent::Connected:
            status_update([](memo_network_status_t &s) {
                s.connected = true;
                // A Wi-Fi association must not hide a failed sync-task init.
                if(s.initialized)s.error[0] = '\0';
            });
            ESP_LOGI(TAG, "WIFI_CONNECTED ssid=%s", data.c_str());
            break;
        case WifiEvent::Disconnected:
            status_update([](memo_network_status_t &s) { s.connected = false; });
            break;
        case WifiEvent::ConfigModeEnter:
            status_update([&wifi](memo_network_status_t &s) {
                s.provisioning = true;
                snprintf(s.ap_ssid, sizeof(s.ap_ssid), "%s", wifi.GetApSsid().c_str());
            });
            break;
        case WifiEvent::ConfigModeExit:
            status_update([](memo_network_status_t &s) { s.provisioning = false; s.ap_ssid[0] = '\0'; });
            if (!SsidManager::GetInstance().GetSsidList().empty()) wifi.StartStation();
            break;
        default:
            break;
        }
    });
    if (!SsidManager::GetInstance().GetSsidList().empty()) manager.StartStation();
    else manager.StartConfigAp();
    if (manager.IsConfigMode()) {
        status_update([&manager](memo_network_status_t &s) {
            s.provisioning = true;
            snprintf(s.ap_ssid, sizeof(s.ap_ssid), "%s", manager.GetApSsid().c_str());
        });
    }
    /* TLS, NVS and the HTTP client may run while flash cache is disabled, so
       keep this stack in internal RAM.  The compact LVGL DMA canvas leaves
       enough internal memory for Wi-Fi and TLS. */
    // Keep HTTPS/NVS work on the same core as the cardless transfer worker,
    // separate from core-1 microphone capture. Other board schedules stay unchanged.
#if CONFIG_MEMO_VOCAT16
    const BaseType_t sync_core = 0;
#else
    const BaseType_t sync_core = tskNO_AFFINITY;
#endif
    detail_task_handle=xTaskCreateStaticPinnedToCore(detail_task,"memo_detail",sizeof(detail_task_stack),nullptr,2,detail_task_stack,&detail_task_storage,0);
    if(!detail_task_handle)return ESP_ERR_NO_MEM;
    // Consume selections made before the task handle became visible.
    xTaskNotifyGive(detail_task_handle);
    sync_task_handle=xTaskCreateStaticPinnedToCore(sync_task,"memo_sync",sizeof(sync_task_stack),nullptr,3,sync_task_stack,&sync_task_storage,sync_core);
    if (!sync_task_handle) {
        ESP_LOGE(TAG,"SYNC_TASK_ALLOC_FAILED internal_free=%zu internal_largest=%zu stack=16384",
                 heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
                 heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
        set_error("网络初始化失败");
        return ESP_ERR_NO_MEM;
    }
    status_update([](memo_network_status_t &s) { s.initialized = true; });
    memo_network_status_t ready_snapshot;
    network_sync_get_status(&ready_snapshot);
    ESP_LOGI(TAG, "NETWORK_READY configured=%d device=%s baseline=%" PRIu32,
             configured, ready_snapshot.device_id, cursor);
    return ESP_OK;
}

extern "C" void network_sync_get_status(memo_network_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!status_mutex) return;
    xSemaphoreTake(status_mutex, portMAX_DELAY);
    *out = status_value;
    out->active_sync_id=sync_queue.active_id;
    out->active_sync_state=active_state.load();
    out->queued_sync_id=sync_queue.pending_id;
    out->queued_sync_manual=sync_queue.pending_manual;
    xSemaphoreGive(status_mutex);
}

extern "C" void network_sync_get_service_config(memo_config_public_t *out)
{
    if(!out)return;
    ConfigGuard guard;memo_config_public_status(guard.value,out);
}
extern "C" memo_config_result_t network_sync_configure_service(const char *url,const char *code,bool local_only)
{
    // A request is accepted only after the durable snapshot is read back. No
    // Wi-Fi credentials, SD files or old-service permissions are involved.
    if(!http_mutex || !status_mutex)return MEMO_CONFIG_STORAGE_ERROR;
    if(xSemaphoreTakeRecursive(http_mutex,0)!=pdTRUE)return MEMO_CONFIG_BUSY;
    memo_config_result_t result=MEMO_CONFIG_BUSY;
    {
        ConfigCommitGuard commit;
        if(commit.acquired){
            ConfigGuard guard;
            if(!guard.value)result=MEMO_CONFIG_STORAGE_ERROR;
            else {
                if(guard.value->needs_reload)memo_network_config_nvs_reload();
                uint8_t random[48];char identifier[37],token[65];esp_fill_random(random,sizeof(random));
                random[6]=(random[6]&15U)|64U;random[8]=(random[8]&63U)|128U;
                snprintf(identifier,sizeof(identifier),"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                    random[0],random[1],random[2],random[3],random[4],random[5],random[6],random[7],random[8],random[9],random[10],random[11],random[12],random[13],random[14],random[15]);
                for(unsigned i=0;i<32;++i)snprintf(token+i*2,3,"%02x",random[16+i]);
                memo_network_status_t current;network_sync_get_status(&current);
                const uint64_t before=guard.value->value.generation;
                result=memo_config_set_service(guard.value,current.device_id,url,code,identifier,token,local_only);
                memset(random,0,sizeof(random));memset(token,0,sizeof(token));
                if(result==MEMO_CONFIG_OK && before!=guard.value->value.generation)set_error(nullptr);
                if((result==MEMO_CONFIG_OK && before!=guard.value->value.generation) || result==MEMO_CONFIG_STORAGE_ERROR){
                    // Queued SD attempts have not sent any bytes; cancel their
                    // old target intent even when the new durable outcome is
                    // uncertain, retain the files. Active/unique audio
                    // was refused by the admission guard above.
                    xSemaphoreTake(status_mutex,portMAX_DELAY);sync_queue={};
                    status_value.cursor_id=0;status_value.last_uploaded_id=0;
                    xSemaphoreGive(status_mutex);
                }
            }
        }
    }
    refresh_service_status();xSemaphoreGiveRecursive(http_mutex);
    if(result==MEMO_CONFIG_OK) {
        sd_scan_requested.store(true);
        if(sync_task_handle)xTaskNotifyGive(sync_task_handle);
    }
    return result;
}

extern "C" memo_config_result_t network_sync_set_local_only(bool local_only) {
    if(!http_mutex || !status_mutex)return MEMO_CONFIG_STORAGE_ERROR;
    // Serialize the read and save with portal writes so maintenance cannot
    // accidentally restore an address changed between those two operations.
    if(xSemaphoreTakeRecursive(http_mutex,0)!=pdTRUE)return MEMO_CONFIG_BUSY;
    memo_config_public_t service{};network_sync_get_service_config(&service);
    memo_config_result_t result=service.result;
    if(result==MEMO_CONFIG_OK)
        result=network_sync_configure_service(service.base_url,"",local_only);
    xSemaphoreGiveRecursive(http_mutex);
    return result;
}

extern "C" void network_sync_select_record(uint32_t id) {
    if(!status_mutex)return;
    xSemaphoreTake(status_mutex,portMAX_DELAY);
    selected_cardless.store((id & MEMO_CARDLESS_ID_FLAG)?id:0);
    memo_cardless_select((id & MEMO_CARDLESS_ID_FLAG)?id:0);
    detail_value={};detail_value.id=id;detail_value.state=MEMO_JOB_CHECKING;++detail_revision;
    inspect_id.store(id);
    xSemaphoreGive(status_mutex);
    if(detail_task_handle)xTaskNotifyGive(detail_task_handle);
}
extern "C" void network_sync_record_status(memo_record_sync_t *out) {
    memset(out,0,sizeof(*out));
    uint32_t cardless_id=selected_cardless.load();
    if(cardless_id) {
        memo_cardless_record_t record{};
        out->id=cardless_id;out->cardless=true;out->state=MEMO_JOB_CHECKING;
        if(memo_cardless_peek(cardless_id,&record)) {
            out->state=record.state;out->can_retry=memo_cardless_can_resume(cardless_id);
            out->interrupted=record.interrupted;out->can_cancel=memo_cardless_can_cancel(cardless_id);
            snprintf(out->error,sizeof(out->error),"%s",record.error);

        }
        return;
    }
    if(!status_mutex)return;
    xSemaphoreTake(status_mutex,portMAX_DELAY);
    *out=detail_value;
    out->request_pending=memo_sync_queue_contains(&sync_queue,out->id);
    if(out->id && cancel_request.load()==out->id){out->state=MEMO_JOB_CANCELLING;out->can_cancel=out->can_retry=false;}
    else if(out->request_pending && out->state!=MEMO_JOB_CANCELLING)out->can_cancel=true;
    xSemaphoreGive(status_mutex);
}
extern "C" bool network_sync_retry_record(uint32_t id) {
    if(id & MEMO_CARDLESS_ID_FLAG)return memo_cardless_retry(id);
    memo_network_status_t snapshot;network_sync_get_status(&snapshot);
    if(!storage_available.load() || !id || id>latest_id.load() || !snapshot.configured || audio_busy() || !status_mutex)return false;
    xSemaphoreTake(status_mutex,portMAX_DELAY);
    bool accepted=memo_sync_queue_offer_manual(&sync_queue,id,false);
    xSemaphoreGive(status_mutex);
    if(!accepted)return false;
    sd_media_work.store(true);
    if(sync_task_handle)xTaskNotifyGive(sync_task_handle);
    return true;
}

extern "C" bool network_sync_cancel_record(uint32_t id) {
 if(id & MEMO_CARDLESS_ID_FLAG)return memo_cardless_cancel(id);
 if(!id||!status_mutex||!storage_available.load())return false;
 uint32_t empty=0;if(!cancel_request.compare_exchange_strong(empty,id))return empty==id;
 if(sync_task_handle)xTaskNotifyGive(sync_task_handle);
 return true;
}

extern "C" void network_sync_set_storage_available(bool available) {
    storage_available.store(available);
    if(available)sd_scan_requested.store(true);
    if(sync_task_handle)xTaskNotifyGive(sync_task_handle);
}

extern "C" void network_sync_observe_latest(uint32_t record_id) {
    uint32_t previous=latest_id.load();
    while(record_id>previous && !latest_id.compare_exchange_weak(previous,record_id)) {}
}

extern "C" bool network_sync_current_target(memo_sync_target_t *out){
 if(!out)return false;
 *out={};memo_config_snapshot_t service{};{ConfigGuard guard;if(!guard.value||memo_config_read_snapshot(guard.value,&service)!=MEMO_CONFIG_OK||service.phase!=MEMO_SERVICE_PAIRED)return false;}
 out->instance=service.instance;snprintf(out->device_id,sizeof(out->device_id),"%s",service.device_id);
 return mbedtls_sha256((const unsigned char *)service.base_url,strlen(service.base_url),out->url_hash,0)==0;
}
extern "C" bool network_sync_track_record(uint32_t id){
 memo_sync_entry_t e{};e.id=id;e.state=MEMO_JOB_QUEUED;e.automatic=true;
 {ConfigGuard guard;memo_config_snapshot_t service{};
  if(guard.value&&memo_config_read_snapshot(guard.value,&service)==MEMO_CONFIG_OK&&service.base_url[0]){
   e.target.instance=service.instance;snprintf(e.target.device_id,sizeof(e.target.device_id),"%s",service.device_id);
   if(mbedtls_sha256((const unsigned char *)service.base_url,strlen(service.base_url),e.target.url_hash,0))return false;
  }
 }
 char path[160];cache_path(path,sizeof(path),id);
 if(!memo_sync_entry_write(path,&e)){set_error("同步状态保存失败，录音仍可本地保存");return false;}
 return true;
}
extern "C" void network_sync_notify_record(uint32_t id){
 network_sync_observe_latest(id);latest_change_us.store(esp_timer_get_time());
 sd_scan_requested.store(true);sd_media_work.store(true);
 // The durable per-file intents, rather than a latest-only RAM queue, survive reboot.
 if(sync_task_handle)xTaskNotifyGive(sync_task_handle);
}

extern "C" void network_sync_toggle_provisioning(void)
{
    auto &manager = WifiManager::GetInstance();
    if (!manager.IsInitialized()) return;
    if (manager.IsConfigMode()) {
        manager.StopConfigAp();
    } else {
        manager.StopStation();
        manager.StartConfigAp();
    }
}

extern "C" bool network_sync_prepare_audio(uint32_t timeout_ms)
{
    // The application has one audio owner; emergency close may re-enter its
    // existing AUDIO claim. Never re-enter a COMMIT or MEDIA owner.
    int64_t deadline = esp_timer_get_time() + timeout_ms * 1000LL;
    while(memo_config_activity(&activity_gate)!=MEMO_ACTIVITY_AUDIO && !memo_config_activity_acquire(&activity_gate,MEMO_ACTIVITY_AUDIO)){
        if(esp_timer_get_time()>=deadline)return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    while (file_read_active.load() && esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!file_read_active.load()) return true;
    memo_config_activity_release(&activity_gate,MEMO_ACTIVITY_AUDIO);
    return false;
}

extern "C" void network_sync_audio_done(void)
{
    memo_config_activity_release(&activity_gate,MEMO_ACTIVITY_AUDIO);
    if (sync_task_handle) xTaskNotifyGive(sync_task_handle);
}

extern "C" bool network_sync_try_sd_probe(void)
{
    if(!memo_config_activity_acquire(&activity_gate,MEMO_ACTIVITY_MEDIA))return false;
    // Match begin_sd_io's publish-then-check barrier. Never race an SD reader.
    if(file_read_active.load() || power_suspended.load() || memo_cardless_busy()) {
        memo_config_activity_release(&activity_gate,MEMO_ACTIVITY_MEDIA);return false;
    }
    return true;
}
extern "C" void network_sync_sd_probe_done(void)
{
    memo_config_activity_release(&activity_gate,MEMO_ACTIVITY_MEDIA);
}

/* Reuse the existing streaming hash under the caller's offline AUDIO claim.
 * No network connection, configuration or remote operation is involved. */


extern "C" bool network_sync_sd_work_pending(void) {
    memo_network_status_t s;network_sync_get_status(&s);
    return !power_suspended.load() && !s.provisioning &&
        ((s.configured && s.connected && sd_media_work.load()) || inspect_id.load());
}
extern "C" uint32_t network_sync_sd_scan_count(void) {return sd_directory_scans.load();}

extern "C" void network_sync_wake(void) {
    if(sync_task_handle)xTaskNotifyGive(sync_task_handle);
}

extern "C" uint32_t network_sync_http_stack_free(void) {return MemoHttpTransport::stack_free();}

extern "C" uint32_t network_sync_detail_stack_free(void) {
    return detail_task_handle ? uxTaskGetStackHighWaterMark(detail_task_handle)*sizeof(StackType_t) : 0;
}

extern "C" bool network_sync_power_busy(void)
{
    memo_network_status_t snapshot;
    network_sync_get_status(&snapshot);
    auto owner=memo_config_activity(&activity_gate);
    // A periodic media query must not restart the user inactivity timer.
    return snapshot.provisioning || snapshot.syncing || owner==MEMO_ACTIVITY_AUDIO || owner==MEMO_ACTIVITY_COMMIT || file_read_active.load() ||
           stream_http_active.load() || pairing_http_active.load() || memo_cardless_busy();
}

extern "C" bool network_sync_power_suspend(void)
{
    if (network_sync_power_busy() || memo_config_activity(&activity_gate)==MEMO_ACTIVITY_MEDIA) return false;
    power_suspended.store(true);
    /* Close the check/start race with sync_task: a transfer that crossed its
     * predicate before the flag was set will publish syncing here. */
    if (network_sync_power_busy() || memo_config_activity(&activity_gate)==MEMO_ACTIVITY_MEDIA) {
        power_suspended.store(false);
        return false;
    }
    memo_network_status_t snapshot;
    network_sync_get_status(&snapshot);
    auto &manager = WifiManager::GetInstance();
    if (manager.IsInitialized()) manager.StopStation();
    ESP_LOGI(TAG, "POWER_SUSPEND station=off cursor=%" PRIu32, snapshot.cursor_id);
    return true;
}

extern "C" void network_sync_power_resume(void)
{
    if (!power_suspended.exchange(false)) return;
    auto &manager = WifiManager::GetInstance();
    memo_network_status_t snapshot;
    network_sync_get_status(&snapshot);
    if (manager.IsInitialized() &&
        !SsidManager::GetInstance().GetSsidList().empty()) manager.StartStation();
    if (sync_task_handle) xTaskNotifyGive(sync_task_handle);
    ESP_LOGI(TAG, "POWER_RESUME station=%s", snapshot.configured ? "starting" : "unconfigured");
}

extern "C" void network_sync_power_set_saving(bool saving)
{
    auto &manager = WifiManager::GetInstance();
    if (!manager.IsInitialized()) return;
    manager.SetPowerSaveLevel(saving ? WifiPowerSaveLevel::LOW_POWER :
                                      WifiPowerSaveLevel::PERFORMANCE);
}

extern "C" int network_sync_stream_request(const char *method,const char *path,
    uint32_t record_id,const void *payload,size_t length,const char *content_type,
    const char *chunk_sha,char *response,size_t capacity)
{
    if(response && capacity)response[0]='\0';
    if(!http_mutex || !method || !path || strncmp(path,"/v1/streams",11) ||
       length>65536 || !response || !capacity)return 0;
    memo_network_status_t snapshot;network_sync_get_status(&snapshot);
    if(!snapshot.initialized || !snapshot.configured || !snapshot.connected || snapshot.provisioning || power_suspended.load())return 0;
    stream_http_active.store(true);
    HttpGuard guard;
    memo_config_snapshot_t service{};char url[1024];
    if(!current_service(service) || memo_config_join_url(service.base_url,path,url,sizeof(url))!=MEMO_CONFIG_OK){stream_http_active.store(false);return 0;}
    if(!ensure_clock(service.base_url)) {stream_http_active.store(false);return 0;}
    // Recheck inside HTTP exclusion: a selected closed task cannot race a
    // service edit between its early task check and transport construction.
    memo_cardless_record_t task{};memo_sync_target_t current{};
    current.instance=service.instance;snprintf(current.device_id,sizeof(current.device_id),"%s",service.device_id);
    if(mbedtls_sha256((const unsigned char *)service.base_url,strlen(service.base_url),current.url_hash,0)||!memo_cardless_get(record_id,&task)||!memo_sync_target_matches(&task.target,&current,false)){stream_http_active.store(false);return 412;}

    esp_http_client_config_t config{};
    config.url=url;config.crt_bundle_attach=esp_crt_bundle_attach;config.disable_auto_redirect=true;
    config.timeout_ms=12000;config.buffer_size=1024;config.buffer_size_tx=2048;
    MemoHttpTransport transport(config);
    esp_http_client_handle_t client=transport.ready()?esp_http_client_init(&config):nullptr;
    if(!client) {stream_http_active.store(false);return 0;}
    esp_http_client_set_method(client,!strcmp(method,"PUT")?HTTP_METHOD_PUT:
                              !strcmp(method,"POST")?HTTP_METHOD_POST:HTTP_METHOD_GET);
    std::string authorization=std::string("Bearer ")+service.token;
    char id[16];snprintf(id,sizeof(id),"%" PRIu32,record_id);
    esp_http_client_set_header(client,"Authorization",authorization.c_str());
    esp_http_client_set_header(client,"X-Device-ID",service.device_id);
    esp_http_client_set_header(client,"X-Recording-ID",id);
    esp_http_client_set_header(client,"User-Agent","MeowVoiceMemoDevice/0.3");
    if(content_type)esp_http_client_set_header(client,"Content-Type",content_type);
    if(chunk_sha)esp_http_client_set_header(client,"X-Chunk-SHA256",chunk_sha);
    esp_err_t result=esp_http_client_open(client,(int)length);
    size_t sent=0;
    while(result==ESP_OK && sent<length) {
        size_t want=length-sent; if(want>4096)want=4096;
        transport.write_window();
        int n=esp_http_client_write(client,static_cast<const char *>(payload)+sent,want);
        if(n<=0) {result=ESP_FAIL;break;}
        sent+=(size_t)n;
    }
    int code=0;std::string body;
    if(result==ESP_OK && esp_http_client_fetch_headers(client)>=0) {
        code=esp_http_client_get_status_code(client);
        if(read_http_body(client,body)!=ESP_OK || body.size()>=capacity)code=0;
        else memcpy(response,body.c_str(),body.size()+1);
    }
    esp_http_client_close(client);esp_http_client_cleanup(client);
    stream_http_active.store(false);
    return code;
}
