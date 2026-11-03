#include "network_sync.h"
#include "board.h"
#include "memo_cardless.h"
#include "network_sync_state.h"
#include "memo_service_portal.h"
extern "C" bool memo_portal_status(char *,size_t);
extern "C" bool memo_portal_apply(const char *,size_t,char *,size_t);
#include "memo_sd_sync.h"
#include "memo_cloud_esp.h"
#include "memo_http_transport.h"
#include "memo_record_ids.h"
#include <dirent.h>

#include "memo_store.h"
#include "wifi_manager.h"
#include "ssid_manager.h"

#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <inttypes.h>
#include <string>
#include <ctime>
#include <sys/stat.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace {

constexpr const char *TAG = "MEMO_NET";
constexpr uint32_t RETRY_DELAY_MS = 15'000;
constexpr uint32_t NEW_RECORD_SETTLE_MS = 4'000;

SemaphoreHandle_t status_mutex;
SemaphoreHandle_t http_mutex;
std::atomic<TaskHandle_t> cloud_input_owner{nullptr};
std::atomic<bool> cloud_reload_needed{false};
std::atomic<uint32_t> selected_cardless{0};
std::atomic<uint32_t> cancel_request{0};
std::atomic<bool> sd_abort{false},sd_media_abort{false};
std::atomic<bool> sd_resume_after_media{false};
memo_sd::Runtime sd_runtime;
std::atomic<int64_t> last_cloud_delivery{0};
bool sd_step_pending=false,sd_step_blocked=false;
uint32_t sd_step_delay_ms=0;
std::atomic<uint32_t> tracked_sd_id{0};
uint32_t tracked_qwen=0,tracked_siyuan=0;
bool tracked_enabled=false;
struct DirectConfig {
 memo_cloud_config_t value{};bool valid;
 DirectConfig():valid(memo_cloud_config_read(&value)){}
 ~DirectConfig(){memo_cloud_config_clear(&value);}
};
class HttpGuard {
public:
    HttpGuard() { if(http_mutex)xSemaphoreTakeRecursive(http_mutex,portMAX_DELAY); }
    ~HttpGuard() { if(http_mutex)xSemaphoreGiveRecursive(http_mutex); }
};
memo_network_status_t status_value{};
char ap_password[9]{}; // Protected by status_mutex; only used by the device screen.
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

class ConfigCommitGuard {
public:
    bool acquired;
    ConfigCommitGuard():acquired(memo_config_activity_acquire(&activity_gate,MEMO_ACTIVITY_COMMIT)) {
        if(acquired && (file_read_active.load() || active_record.load() || memo_cardless_busy())) {
            memo_config_activity_release(&activity_gate,MEMO_ACTIVITY_COMMIT);acquired=false;
        }
    }
    ~ConfigCommitGuard(){if(acquired)memo_config_activity_release(&activity_gate,MEMO_ACTIVITY_COMMIT);}
};
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

bool ensure_clock(const char *base)
{
    (void)base;
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
           keep HTTPS provider access independent of that DNS mode. */
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

uint32_t shown_qwen=0,shown_siyuan=0;
void publish_cloud_status(const memo_cloud_public_t &config,bool changed){
 status_update([&](memo_network_status_t &s){s.configured=config.result==MEMO_CLOUD_OK&&config.enabled&&config.configured;
  s.cloud_enabled=config.enabled;s.cloud_credentials=config.configured;s.cloud_config_error=config.result!=MEMO_CLOUD_OK;
  if(shown_qwen!=config.qwen_generation||shown_siyuan!=config.siyuan_generation){s.last_task_id=0;s.last_task_state=MEMO_JOB_UNKNOWN;s.last_task_error[0]=0;}});
 shown_qwen=config.qwen_generation;shown_siyuan=config.siyuan_generation;
 cloud_reload_needed=config.result!=MEMO_CLOUD_OK;
 memo_cardless_configure(config.result==MEMO_CLOUD_OK&&config.enabled&&config.configured,changed);
}
void refresh_service_status(){memo_cloud_public_t config{};memo_cloud_settings_public(&config);publish_cloud_status(config,false);}
bool begin_sd_io(void *) {file_read_active.fetch_add(1);if(memo_config_activity(&activity_gate)!=MEMO_ACTIVITY_IDLE){file_read_active.fetch_sub(1);return false;}return true;}
void end_sd_io(void *) {file_read_active.fetch_sub(1);}
const char *sync_error(memo_sd::Error error){
 using memo_sd::Error;
 switch(error){
 case Error::None:return "";
 case Error::Storage:return "状态保存失败";
 case Error::Audio:return "录音不可读";
 case Error::Legacy:return "旧服务待核对";
 case Error::Configuration:return "请确认当前账号";
 case Error::NoSpeech:return "未识别到语音";
 case Error::Credentials:return "凭据无效";
 case Error::Limited:return "服务限流";
 case Error::Response:return "服务响应无效";
 case Error::TooLarge:return "内容超出上限";
 case Error::Memory:return "设备内存不足";
 case Error::Interrupted:return "处理已中断";
 default:return "服务连接失败";
 }
}
memo_sd::Control sd_control(void *){
 if(cancel_request.load()==active_record.load()&&active_record.load())return memo_sd::Control::Cancel;
 if(sd_media_abort.load()||audio_busy()||power_suspended.load()||WifiManager::GetInstance().IsConfigMode())return memo_sd::Control::Media;
 if(sd_abort.load())return memo_sd::Control::Pause;
 return memo_sd::Control::Run;
}
bool sync_one(uint32_t id,bool cancel=false){
 HttpGuard guard;sd_step_pending=false;sd_step_blocked=false;sd_step_delay_ms=0;
 if(!begin_sd_io(nullptr)){sd_step_blocked=true;return false;}
 DirectConfig config;memo_sd::Result result;
 bool missing_cancel=false;
 if(cancel){
  memo_sd::Entry e;auto cached=memo_sd::read("/sdcard/memos",id,&e);missing_cancel=cached==MEMO_CACHE_MISSING;
  if(cached==MEMO_CACHE_OK&&e.intent&&!memo_sd::cancel("/sdcard/memos",id)){
   end_sd_io(nullptr);set_error(sync_error(memo_sd::Error::Storage));sd_step_blocked=true;return false;
  }
 }
 sd_abort.store(cancel);sd_media_abort.store(false);
 MemoCloudEsp platform(sd_abort);
 memo_sd::IO io;io.cloud=&platform.io();
 io.now_ms=[](void *){return esp_timer_get_time()/1000;};io.unix_seconds=[](void *){return int64_t(time(nullptr));};
 io.connected=[](void *){return WifiManager::GetInstance().IsConnected();};
 io.clock_ready=[](void *){return ensure_clock("https://cloud");};io.control=sd_control;
 io.progress=[](void *,memo_job_state_t state){job_state(active_record.load(),state);};
 if(missing_cancel)memo_sd::view("/sdcard/memos",id,config.value,result);
 else {
  sd_runtime.last_delivery_ms=last_cloud_delivery.load();
  memo_sd::step("/sdcard/memos",id,config.value,io,sd_runtime,result);
  last_cloud_delivery.store(std::max(last_cloud_delivery.load(),sd_runtime.last_delivery_ms));
 }
 end_sd_io(nullptr);
 sd_step_pending=result.pending;sd_step_delay_ms=result.delay_ms;
 sd_step_blocked=result.error==memo_sd::Error::Storage||result.needs_rebind||
     (result.pending&&(!config.valid||!config.value.enabled||!memo_cloud_config_ready(&config.value)||
                      !WifiManager::GetInstance().IsConnected()||sd_control(nullptr)==memo_sd::Control::Media));
 job_state(id,result.state);network_sync_task_result(id,result.state,sync_error(result.error));
 xSemaphoreTake(status_mutex,portMAX_DELAY);
 if(detail_value.id==id){
  detail_value.can_retry=result.can_retry;detail_value.can_cancel=result.can_cancel;
  detail_value.needs_rebind=result.needs_rebind;
  detail_value.service_ready=config.valid&&config.value.enabled&&memo_cloud_config_ready(&config.value);
  snprintf(detail_value.error,sizeof(detail_value.error),"%s",sync_error(result.error));
 }
 if(result.state==MEMO_JOB_SENT&&id>status_value.last_uploaded_id)status_value.last_uploaded_id=id;
 xSemaphoreGive(status_mutex);set_error(sync_error(result.error));
 return result.error!=memo_sd::Error::Storage&&(!result.pending||result.state==MEMO_JOB_CANCELLING);
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
 DirectConfig config;memo_sd::Result result;
 memo_sd::view("/sdcard/memos",id,config.value,result);
 end_sd_io(nullptr);
 xSemaphoreTake(status_mutex,portMAX_DELAY);
 if(detail_value.id==id && detail_revision==revision){
  bool active=active_record.load()==id;
  detail_value.state=active?active_state.load():result.state;detail_value.can_retry=!active && result.can_retry;detail_value.can_cancel=result.can_cancel;
  detail_value.needs_rebind=result.needs_rebind;detail_value.service_ready=config.valid&&config.value.enabled&&memo_cloud_config_ready(&config.value);
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
bool pending_record(const memo_cloud_config_t &config,bool &manual,uint32_t &next){
 static uint32_t after=0;manual=false;next=0;
 if(!begin_sd_io(nullptr))return false;
 bool healthy=board_sd_check()==ESP_OK;
 if(healthy) {
  sd_directory_scans.fetch_add(1);
  next=memo_sd::pending("/sdcard/memos",config,&after,&healthy);
 }
 end_sd_io(nullptr);return healthy;
}
/* Consume the dirty flag before I/O: an event arriving during a scan remains
 * pending. A busy or missing card never masquerades as an empty directory. */
bool discover_sd_work(const memo_cloud_config_t &config) {
 if(!sd_scan_requested.exchange(false))return true;
 bool manual=false;uint32_t next=0;
 bool healthy=pending_record(config,manual,next);
 if(!healthy){sd_scan_requested.store(true);sd_media_work.store(true);}
 xSemaphoreTake(status_mutex,portMAX_DELAY);
 if(next && !sync_queue.active_id && !sync_queue.pending_id) {
  if(manual)memo_sync_queue_offer_manual(&sync_queue,next,true);
  else memo_sync_queue_offer_auto(&sync_queue,next);
 }
 sd_media_work.store(!healthy || next || sync_queue.active_id || sync_queue.pending_id);
 xSemaphoreGive(status_mutex);return healthy;
}
bool persist_manual(uint32_t id){
 if(!id)return true;
 if(!begin_sd_io(nullptr))return false;
 DirectConfig config;
 bool ok=config.valid&&memo_record_ids_observe(id)&&memo_sd::submit("/sdcard/memos",id,config.value);
 end_sd_io(nullptr);return ok;
}
void sync_task(void *)
{
    int64_t next_scan_us=0,next_sd_us=0;
    bool previously_eligible=false;
    for(;;){
        auto &wifi=WifiManager::GetInstance();wifi.ProcessConfigConnection();
        bool connected=wifi.IsConnected();status_update([connected](memo_network_status_t &s){s.connected=connected;});
        uint32_t cancelled=cancel_request.load();
        // SD cancellation is local metadata work: it does not require Wi-Fi,
        // provider credentials or closing the configuration hotspot.
        if(cancelled&&storage_available.load()&&!audio_busy()&&!power_suspended.load()&&!memo_cardless_busy()){
            if(sync_one(cancelled,true)){
                xSemaphoreTake(status_mutex,portMAX_DELAY);
                if(sync_queue.active_id==cancelled)memo_sync_queue_finish(&sync_queue,cancelled);
                if(sync_queue.pending_id==cancelled){sync_queue.pending_id=0;sync_queue.pending_manual=sync_queue.pending_persisted=false;}
                xSemaphoreGive(status_mutex);cancel_request.compare_exchange_strong(cancelled,0);
                sd_scan_requested.store(true);next_sd_us=0;
            }
        }
        memo_cardless_network_step(); // The same owner drives bounded realtime PCM.
        memo_network_status_t snapshot;network_sync_get_status(&snapshot);
        bool eligible=snapshot.connected&&!snapshot.provisioning&&!power_suspended.load();
        if(eligible&&!previously_eligible){sd_scan_requested.store(true);next_scan_us=next_sd_us=0;}
        previously_eligible=eligible;
        bool available=storage_available.load()&&!snapshot.provisioning&&!power_suspended.load()&&!audio_busy()&&!memo_cardless_busy();
        if(available){
            uint32_t persist_id=0,active=0,pending=0;
            xSemaphoreTake(status_mutex,portMAX_DELAY);
            if(sync_queue.pending_manual&&!sync_queue.pending_persisted)persist_id=sync_queue.pending_id;
            active=sync_queue.active_id;pending=sync_queue.pending_id;xSemaphoreGive(status_mutex);
            if(persist_id){
                bool saved=persist_manual(persist_id);
                xSemaphoreTake(status_mutex,portMAX_DELAY);memo_sync_queue_manual_persisted(&sync_queue,persist_id,saved);xSemaphoreGive(status_mutex);
                if(saved)job_state(persist_id,MEMO_JOB_QUEUED);
                else {defer_detail(persist_id);if(detail_task_handle&&inspect_id.load())xTaskNotifyGive(detail_task_handle);}
            }
            // No secret/NVS read on an idle iteration with no dirty scan/work.
            if(sd_scan_requested.load()||active||pending||persist_id){
                DirectConfig config;
                if(esp_timer_get_time()>=next_scan_us&&!discover_sd_work(config.value))next_scan_us=esp_timer_get_time()+RETRY_DELAY_MS*1000LL;
                bool settled=esp_timer_get_time()-latest_change_us.load()>=NEW_RECORD_SETTLE_MS*1000LL;
                uint32_t id;
                xSemaphoreTake(status_mutex,portMAX_DELAY);memo_sync_queue_start(&sync_queue,settled);id=sync_queue.active_id;xSemaphoreGive(status_mutex);
                if(id&&!audio_busy()&&esp_timer_get_time()>=next_sd_us){
                    active_state.store(MEMO_JOB_TRANSCRIBING);active_record.store(id);
                    status_update([](memo_network_status_t &s){s.syncing=true;});sync_one(id);
                    status_update([](memo_network_status_t &s){s.syncing=false;});active_record.store(0);
                    if(!sd_step_pending||sd_step_blocked||memo_sync_device_attempt_done(active_state.load())){
                        xSemaphoreTake(status_mutex,portMAX_DELAY);memo_sync_queue_finish(&sync_queue,id);xSemaphoreGive(status_mutex);
                        sd_media_work.store(false);
                        if(!sd_step_blocked)sd_scan_requested.store(true);
                        next_sd_us=0;
                    }else{
                        next_sd_us=sd_step_delay_ms?esp_timer_get_time()+sd_step_delay_ms*1000LL:0;
                        if(!next_sd_us&&sync_task_handle)xTaskNotifyGive(sync_task_handle);
                    }
                    // Advance the streaming scan after a terminal job, without
                    // repeatedly selecting a server-processing WAITING record.
                    if(sd_scan_requested.load()&&!sd_step_blocked&&sync_task_handle)xTaskNotifyGive(sync_task_handle);
                }
            }
        }
        uint32_t wait=memo_cardless_busy()?250:wifi.IsConfigMode()?1000:RETRY_DELAY_MS;
        if(next_sd_us>esp_timer_get_time())wait=std::min<uint32_t>(wait,uint32_t((next_sd_us-esp_timer_get_time()+999)/1000));
        xSemaphoreTake(status_mutex,portMAX_DELAY);
        bool settling=sync_queue.pending_id&&!sync_queue.pending_manual;
        xSemaphoreGive(status_mutex);
        int64_t settle_left=NEW_RECORD_SETTLE_MS*1000LL-(esp_timer_get_time()-latest_change_us.load());
        if(settling&&settle_left>0)wait=std::min<uint32_t>(wait,uint32_t((settle_left+999)/1000));
        ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(wait));
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
    refresh_service_status();
    memo_cloud_public_t current{};memo_cloud_settings_public(&current);
    bool configured=current.result==MEMO_CLOUD_OK&&current.enabled&&current.configured;
    uint32_t cursor=0;
    if(current.result!=MEMO_CLOUD_OK)set_error("云配置不可读");
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
        case WifiEvent::ConfigModeEnter: {
            const auto ssid = wifi.GetApSsid();
            const auto password = wifi.GetApPassword();
            status_update([&](memo_network_status_t &s) {
                s.provisioning = true;
                s.ap_password_error = false;
                snprintf(s.ap_ssid, sizeof(s.ap_ssid), "%s", ssid.c_str());
                snprintf(ap_password, sizeof(ap_password), "%s", password.c_str());
            });
            break;
        }
        case WifiEvent::ConfigModeExit:
            status_update([](memo_network_status_t &s) { s.provisioning = false; s.ap_ssid[0] = '\0'; memset(ap_password, 0, sizeof(ap_password)); });
            if (!SsidManager::GetInstance().GetSsidList().empty()) wifi.StartStation();
            break;
        default:
            break;
        }
    });
    if (!SsidManager::GetInstance().GetSsidList().empty()) manager.StartStation();
    else manager.StartConfigAp();
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

extern "C" void network_sync_task_result(uint32_t id,memo_job_state_t state,const char *error){
 status_update([&](memo_network_status_t &s){s.last_task_id=id;s.last_task_state=state;snprintf(s.last_task_error,sizeof(s.last_task_error),"%s",error?error:"");});
}
extern "C" void network_sync_get_cloud_config(memo_cloud_public_t *out){
 if(!out)return;
 if(!cloud_reload_needed.load()){memo_cloud_settings_public(out);return;}
 if(!http_mutex||xSemaphoreTakeRecursive(http_mutex,0)!=pdTRUE){memo_cloud_settings_public(out);if(out->result==MEMO_CLOUD_OK)out->result=MEMO_CLOUD_BUSY;return;}
 {ConfigCommitGuard commit;
  memo_cloud_settings_public(out);
  if(commit.acquired&&out->result==MEMO_CLOUD_OK)publish_cloud_status(*out,false);
  else if(out->result==MEMO_CLOUD_OK)out->result=MEMO_CLOUD_BUSY;
 }
 xSemaphoreGiveRecursive(http_mutex);
}

// Recording pipelines adopt direct calls in steps 3/4, using the existing
// single network owner and HTTP gate; no additional scheduler or task.
extern "C" int64_t network_sync_delivery_last(){return last_cloud_delivery.load();}
extern "C" void network_sync_delivery_started(){last_cloud_delivery.store(esp_timer_get_time()/1000);}
extern "C" bool network_sync_cloud_acquire() {
    return http_mutex && xSemaphoreTakeRecursive(http_mutex,0)==pdTRUE;
}
extern "C" void network_sync_cloud_release() {xSemaphoreGiveRecursive(http_mutex);}
extern "C" bool network_sync_cloud_clock_ready() {return ensure_clock("https://cloud");}
namespace {
void cloud_saved(bool changed){
 memo_cloud_public_t config{};memo_cloud_settings_public(&config);publish_cloud_status(config,changed);
 if(changed){sd_scan_requested.store(true);if(sync_task_handle)xTaskNotifyGive(sync_task_handle);}
}
}
extern "C" memo_cloud_result_t network_sync_configure_cloud(const memo_cloud_patch_t *patch){
 if(!http_mutex||!status_mutex)return MEMO_CLOUD_STORAGE;
 if(xSemaphoreTakeRecursive(http_mutex,0)!=pdTRUE)return MEMO_CLOUD_BUSY;
 memo_cloud_result_t result=MEMO_CLOUD_BUSY;bool changed=false;
 {ConfigCommitGuard commit;if(commit.acquired){result=memo_cloud_settings_apply(patch,&changed);cloud_saved(changed);}}
 xSemaphoreGiveRecursive(http_mutex);return result;
}
extern "C" memo_cloud_result_t network_sync_set_local_only(bool local){
 memo_cloud_patch_t patch{};patch.set_mode=true;patch.enabled=!local;return network_sync_configure_cloud(&patch);
}
extern "C" bool network_sync_cloud_input_begin(){
 if(!http_mutex||cloud_input_owner||xSemaphoreTakeRecursive(http_mutex,0)!=pdTRUE)return false;
 memo_network_status_t s{};network_sync_get_status(&s);
 if(!s.provisioning||file_read_active.load()||active_record.load()||memo_cardless_busy()||!memo_config_activity_acquire(&activity_gate,MEMO_ACTIVITY_COMMIT)){xSemaphoreGiveRecursive(http_mutex);return false;}
 cloud_input_owner=xTaskGetCurrentTaskHandle();return true;
}
extern "C" memo_cloud_result_t network_sync_cloud_input_save(const memo_cloud_patch_t *patch){
 if(!cloud_input_owner||cloud_input_owner!=xTaskGetCurrentTaskHandle())return MEMO_CLOUD_BUSY;
 bool changed=false;auto result=memo_cloud_settings_apply(patch,&changed);cloud_saved(changed);return result;
}
extern "C" void network_sync_cloud_input_end(){
 if(!cloud_input_owner||cloud_input_owner!=xTaskGetCurrentTaskHandle())return;
 cloud_input_owner=nullptr;memo_config_activity_release(&activity_gate,MEMO_ACTIVITY_COMMIT);xSemaphoreGiveRecursive(http_mutex);
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
            out->state=record.state;out->can_retry=false;
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
    if(id & MEMO_CARDLESS_ID_FLAG)return false;
    if(!storage_available.load() || !id || id>latest_id.load() || audio_busy() || !status_mutex)return false;
    DirectConfig config;if(!config.valid||!config.value.enabled||!memo_cloud_config_ready(&config.value))return false;
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
 if(active_record.load()==id)sd_abort.store(true);
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

extern "C" bool network_sync_track_record(uint32_t id){
 if(!id){tracked_sd_id.store(0);tracked_enabled=false;return true;}
 DirectConfig config;tracked_enabled=config.valid&&config.value.enabled&&memo_cloud_config_ready(&config.value);
 tracked_qwen=config.value.qwen_generation;tracked_siyuan=config.value.siyuan_generation;
 tracked_sd_id.store(id);return config.valid;
}
extern "C" void network_sync_notify_record(uint32_t id){
 network_sync_observe_latest(id);latest_change_us.store(esp_timer_get_time());
 uint32_t expected=id;
 if(tracked_sd_id.compare_exchange_strong(expected,0)&&tracked_enabled){
  DirectConfig config;
  if(!config.valid)set_error("服务配置不可读，录音仍在本地");
  else if(config.value.enabled&&(config.value.qwen_generation!=tracked_qwen||config.value.siyuan_generation!=tracked_siyuan||
     !memo_sd::create("/sdcard/memos",id,config.value,true)))set_error("同步状态保存失败，录音仍可本地保存");
 }
 sd_scan_requested.store(true);sd_media_work.store(true);
 if(sync_task_handle)xTaskNotifyGive(sync_task_handle);
}

extern "C" bool network_sync_rotate_ap_password(void) {
    auto &manager = WifiManager::GetInstance();
    const bool rotated = manager.RotateApPassword();
    const auto password = rotated ? manager.GetApPassword() : std::string{};
    status_update([&](memo_network_status_t &s) {
        s.ap_password_error = !rotated;
        if (rotated && s.provisioning)
            snprintf(ap_password, sizeof(ap_password), "%s", password.c_str());
    });
    return rotated;
}

extern "C" void network_sync_get_ap_password(char out[9]) {
    out[0] = '\0';
    if (!status_mutex) return;
    xSemaphoreTake(status_mutex, portMAX_DELAY);
    memcpy(out, ap_password, sizeof(ap_password));
    xSemaphoreGive(status_mutex);
}

extern "C" void network_sync_toggle_provisioning(void)
{
    auto &manager = WifiManager::GetInstance();
    if (!manager.IsInitialized()) return;
    if(active_record.load()){sd_media_abort.store(true);sd_abort.store(true);}
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
    if(active_record.load()){sd_resume_after_media.store(true);sd_media_abort.store(true);sd_abort.store(true);}
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
    if(sd_resume_after_media.exchange(false)){sd_scan_requested.store(true);if(sync_task_handle)xTaskNotifyGive(sync_task_handle);}
    return false;
}

extern "C" void network_sync_audio_done(void)
{
    memo_config_activity_release(&activity_gate,MEMO_ACTIVITY_AUDIO);
    if(sd_resume_after_media.exchange(false))sd_scan_requested.store(true);
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

extern "C" uint32_t network_sync_worker_stack_free(void) {
    return sync_task_handle ? uxTaskGetStackHighWaterMark(sync_task_handle)*sizeof(StackType_t) : 0;
}

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
           memo_cardless_busy();
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
    if(active_record.load()){sd_media_abort.store(true);sd_abort.store(true);}
    manager.SetPowerSaveLevel(saving ? WifiPowerSaveLevel::LOW_POWER :
                                      WifiPowerSaveLevel::PERFORMANCE);
}
