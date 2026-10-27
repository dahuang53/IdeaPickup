#include "app.h"
#include "board.h"
#include "voice.h"
#include "memo_usb_mic.h"
#include "memo_store.h"
#include "memo_cardless.h"
#include "network_sync.h"
#include "memo_record_ids.h"
#include "power_policy.h"
#include "sdkconfig.h"
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "esp_lvgl_port.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "mbedtls/base64.h"

static const char *TAG = "MEOW_INBOX";
typedef struct { memo_cmd_t cmd; uint32_t arg; char source[12]; } command_t;
static QueueHandle_t queue;
static SemaphoreHandle_t mutex;
static memo_snapshot_t model;
static memo_store_t store;
static memo_item_t *record_index;
static memo_record_t *sd_index;
static size_t sd_count;
static SemaphoreHandle_t sd_probe_mutex;
static atomic_int sd_probe_ready; // 0 none, 1 mounted insertion, 2 status failure/remount
static atomic_bool sd_probe_busy;
static TaskHandle_t sd_probe_handle;
static atomic_bool sd_view_visible, sd_view_requested;
static atomic_uint sd_probe_checks, sd_probe_wakes;

/* LVGL publishes demand only; media I/O stays off the display task. */
void app_storage_view(bool visible) {
    bool changed=atomic_exchange(&sd_view_visible,visible)!=visible;
    if(changed) {
        atomic_store(&sd_view_requested,visible);
        if(sd_probe_handle)xTaskNotifyGive(sd_probe_handle);
    }
}
static bool cardless_available;
static uint32_t cardless_revision;
static size_t record_count;
static bool audio_ready, fault_space, voice_ready;
static atomic_bool microphone_open;
static atomic_bool power_listening = true;
static atomic_bool shutdown_prepared;
static atomic_bool shutdown_failed;
static atomic_bool recovery_deferred;
static atomic_int_fast64_t recovery_retry_after_us;
static FILE *playing;
static memo_audio_identity_t play_identity;
static uint32_t play_remaining, play_bytes;
static int16_t pcm[512];
static int64_t started_us;
static uint32_t recording_overflows;
static uint64_t next_checkpoint_bytes;
static int64_t notice_until_us;
static int64_t usb_suspended_since_us;

static void memory_checkpoint(const char *stage) {
    const uint32_t caps=MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT;
    ESP_LOGI(TAG,"MEMORY stage=%s internal_free=%zu internal_largest=%zu internal_min=%zu psram_free=%zu",
        stage,heap_caps_get_free_size(caps),heap_caps_get_largest_free_block(caps),
        heap_caps_get_minimum_free_size(caps),heap_caps_get_free_size(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
}

#define RECORD_CHECKPOINT_BYTES (60ULL * MEMO_SAMPLE_RATE * 2ULL)

static void network_boot(void *arg) {
    uint32_t latest=(uint32_t)(uintptr_t)arg;
    /* Let app_main return first so its internal stack is released before the
       Wi-Fi driver creates DMA-only SoftAP beacon buffers. */
    vTaskDelay(pdMS_TO_TICKS(100));
    if(network_sync_init(latest)!=ESP_OK)
        ESP_LOGE(TAG,"NETWORK_UNAVAILABLE offline recording remains available");
    else memo_cardless_start_pending();
    memory_checkpoint("network_init_done");
    vTaskDelete(NULL);
}

static unsigned meter_level(unsigned peak) {
    if(!peak)return 0;
    // A linear full-scale meter makes ordinary speech look nearly silent.
    // Map -42 dBFS..-6 dBFS to the visible range, which follows perceived
    // loudness while leaving headroom for a close or loud speaker.
    float dbfs=20.0f*log10f((float)peak/32768.0f);
    int level=(int)((dbfs+42.0f)*100.0f/36.0f);
    return level<0?0U:level>100?100U:(unsigned)level;
}

void app_snapshot(memo_snapshot_t *out) {
    xSemaphoreTake(mutex, portMAX_DELAY); *out=model; xSemaphoreGive(mutex);
    voice_status(&out->voice_ready,&out->voice_window,&out->voice_drops);
    out->cardless_ready=out->cardless && memo_cardless_ready();
    memo_usb_status_t usb;memo_usb_get_status(&usb);
    out->usb_available=usb.available;out->usb_connected=usb.connected;
    out->usb_suspended=usb.suspended;out->usb_streaming=usb.streaming;
    out->usb_local_muted=usb.local_muted;
    out->usb_muted=usb.local_muted || usb.host_muted;
}
bool app_command(memo_cmd_t cmd, uint32_t arg, const char *source) {
    command_t c={.cmd=cmd,.arg=arg}; snprintf(c.source,sizeof(c.source),"%s",source);
    bool ok=queue && xQueueSend(queue,&c,0)==pdTRUE;
    if(ok && (!source || (strcmp(source,"idle_timer") &&
                          strcmp(source,"low_battery") &&
                          strcmp(source,"power_recovery"))))
        power_policy_note_activity(source);
    if (!ok) ESP_LOGW(TAG,"command queue full source=%s cmd=%d",source,cmd);
    return ok;
}
static void state(memo_state_t s, const char *error) {
    xSemaphoreTake(mutex,portMAX_DELAY);
    model.state=s; model.level=0; model.revision++;
    snprintf(model.error,sizeof(model.error),"%s",error?error:"");
    xSemaphoreGive(mutex);
    ESP_LOGI(TAG,"STATE %d %s",s,error?error:"");
}
static void set_recovering(bool recovering) {
    xSemaphoreTake(mutex,portMAX_DELAY);
    model.recovering=recovering;model.revision++;
    xSemaphoreGive(mutex);
}
static void notice(const char *text) {
    xSemaphoreTake(mutex,portMAX_DELAY);
    snprintf(model.notice,sizeof(model.notice),"%s",text?text:"");
    notice_until_us=text && text[0]?esp_timer_get_time()+5000000:0;
    model.revision++;
    xSemaphoreGive(mutex);
}
static void failure(const char *reason) {
    memo_usb_close();
    ESP_LOGE(TAG,"FAILED %s errno=%d (%s)",reason,errno,strerror(errno));
    state(MEMO_ERROR,reason);
}
static int show_page(size_t page) {
    int64_t started=esp_timer_get_time();
    size_t pages=record_count/MEMO_PAGE_SIZE+(record_count%MEMO_PAGE_SIZE!=0);
    if((pages && page>=pages)||(!pages&&page)) {errno=EINVAL;return -1;}
    size_t offset=page*MEMO_PAGE_SIZE;
    size_t count=record_count>offset?record_count-offset:0;
    if(count>MEMO_PAGE_SIZE)count=MEMO_PAGE_SIZE;
    xSemaphoreTake(mutex,portMAX_DELAY);
    model.page=page; model.page_count=pages;
    model.count=record_count; model.item_count=count; model.revision++;
    for (size_t i=0;i<count;i++) {
        model.items[i]=record_index[offset+i];
    }
    xSemaphoreGive(mutex);
    ESP_LOGI(TAG,"LIST_PAGE page=%zu count=%zu total=%zu cache_ms=%"PRId64,
        page,count,record_count,(esp_timer_get_time()-started)/1000);
    return 0;
}
static int item_compare(const void *a,const void *b) {
    uint32_t x=MEMO_CARDLESS_DISPLAY_ID(((const memo_item_t *)a)->id);
    uint32_t y=MEMO_CARDLESS_DISPLAY_ID(((const memo_item_t *)b)->id);
    return x<y?1:x>y?-1:0;
}
static uint32_t latest_visible_id(void) {
    return record_count?MEMO_CARDLESS_DISPLAY_ID(record_index[0].id):0;
}
static int merge_index(size_t page) {
    uint32_t observed_revision=memo_cardless_revision();
    size_t cardless_count=memo_cardless_count();
    size_t total=sd_count+cardless_count;
    memo_item_t *updated=total?calloc(total,sizeof(*updated)):NULL;
    if(total && !updated)return -1;
    size_t n=0;
    for(size_t i=0;i<sd_count;i++)updated[n++]=(memo_item_t){
        .id=sd_index[i].id,.duration_ms=sd_index[i].duration_ms,
        .file_bytes=sd_index[i].data_bytes+44,.cardless=false};
    for(size_t i=0;i<cardless_count;i++) {
        memo_cardless_record_t record;
        if(!memo_cardless_list(i,&record,1))break;
        updated[n++]=(memo_item_t){.id=record.id,.duration_ms=record.duration_ms,
            .file_bytes=record.encoded_bytes,.cardless=true};
    }
    if(n)qsort(updated,n,sizeof(*updated),item_compare);
    free(record_index);record_index=updated;record_count=n;
    cardless_revision=observed_revision;
    if(page*MEMO_PAGE_SIZE>=n)page=0;
    return show_page(page);
}
static int rebuild_index(void) {
    int64_t started=esp_timer_get_time();
    memo_record_t *updated=NULL;size_t count=0;
    if(memo_store_index(&store,&updated,&count))return -1;
    free(sd_index);sd_index=updated;sd_count=count;
    ESP_LOGI(TAG,"LIST_INDEX count=%zu scan_ms=%"PRId64,
        count,(esp_timer_get_time()-started)/1000);
    return merge_index(0);
}
static int add_to_index(const memo_record_t *record) {
    if(sd_count==SIZE_MAX/sizeof(*sd_index)) {errno=EOVERFLOW;return -1;}
    memo_record_t *updated=realloc(sd_index,(sd_count+1)*sizeof(*updated));
    if(!updated)return -1;
    sd_index=updated;
    memmove(sd_index+1,sd_index,sd_count*sizeof(*sd_index));
    sd_index[0]=*record;sd_count++;
    if(cardless_available)memo_cardless_observe_id(record->id);
    return merge_index(0);
}
static bool sd_probe_allowed(const memo_snapshot_t *s) {
    memo_power_status_t power;power_policy_get_status(&power);
    memo_network_status_t network;network_sync_get_status(&network);
    return !network.provisioning && !network.syncing && atomic_load(&power_listening) && (!power.initialized || power.external_power || power.stage==MEMO_POWER_ACTIVE) &&
        !memo_cardless_busy() &&
        s->state!=MEMO_RECORDING && s->state!=MEMO_PLAYING && s->state!=MEMO_PAUSED &&
        s->state!=MEMO_SAVING && s->state!=MEMO_USB_MIC;
}
/* Zero means indefinite notification wait, including pure cat idle. */
static uint32_t sd_probe_interval(const memo_snapshot_t *s) {
    bool view=atomic_load(&sd_view_visible);
    bool work=network_sync_sd_work_pending();
    if((!view && !work) || !sd_probe_allowed(s))return 0;
    if(!s->cardless && !s->sd_ready && !atomic_load(&sd_view_requested))return 0;
    return view?5000U:15000U;
}
static bool storage_prepare(bool remount) {
    network_sync_set_storage_available(false);
    xSemaphoreTake(mutex,portMAX_DELAY);
    model.sd_ready=false;model.cardless=false;model.count=0;model.page=0;model.page_count=0;
    model.item_count=0;model.revision++;
    xSemaphoreGive(mutex);
    free(record_index);record_index=NULL;record_count=0;
    free(sd_index);sd_index=NULL;sd_count=0;
    xSemaphoreTake(sd_probe_mutex,portMAX_DELAY);
    esp_err_t err=remount?board_sd_unmount():ESP_OK;
    if(err==ESP_OK)err=board_sd_mount();
    bool absent=board_sd_absent();
    xSemaphoreGive(sd_probe_mutex);
    if(err!=ESP_OK) {
        ESP_LOGE(TAG,"STORAGE_PREPARE stage=mount result=%s",esp_err_to_name(err));
        if(absent && cardless_available) {
            xSemaphoreTake(mutex,portMAX_DELAY);model.cardless=true;model.revision++;xSemaphoreGive(mutex);
            if(merge_index(0)) {failure("记录列表读取失败");return false;}
            state(MEMO_IDLE,NULL);
            ESP_LOGI(TAG,"STORAGE_MODE cardless no_card_response");
            return true;
        }
        merge_index(0); // Previously received cardless history stays inspectable.
        state(MEMO_ERROR,absent?"无卡记录存储未就绪":"存储卡挂载失败");return false;
    }
    if(memo_store_init(&store,"/sdcard/memos")) {
        failure("录音目录创建失败");return false;
    }
    if(rebuild_index()) {
        failure("录音目录读取失败");return false;
    }
    // FatFS's first free-space query may scan the entire FAT. Do this before
    // offering record, so the first touch cannot stall for several seconds.
    uint64_t total=0,free_bytes=0;
    int64_t capacity_started=esp_timer_get_time();
    err=esp_vfs_fat_info("/sdcard",&total,&free_bytes);
    if(err!=ESP_OK) {
        ESP_LOGE(TAG,"STORAGE_PREPARE stage=capacity result=%s",esp_err_to_name(err));
        state(MEMO_ERROR,"存储卡容量读取失败");return false;
    }
    ESP_LOGI(TAG,"STORAGE_CAPACITY total=%"PRIu64" free=%"PRIu64" check_ms=%"PRId64,
        total,free_bytes,(esp_timer_get_time()-capacity_started)/1000);
    xSemaphoreTake(mutex,portMAX_DELAY);model.sd_ready=true;model.revision++;xSemaphoreGive(mutex);
    if(cardless_available)memo_cardless_observe_id(latest_visible_id());
    network_sync_observe_latest(latest_visible_id());
    network_sync_set_storage_available(true);
    ESP_LOGI(TAG,"STORAGE_READY count=%zu",model.count);
    return true;
}
/* AUDIO is already claimed. Recheck even when the cached mode is cardless.
 * A responsive bad card stays an error; only confirmed no-response can fall
 * back before capture starts. Never call while a recording is in progress. */
static esp_err_t microphone_stop(void);
static bool storage_check_for_operation(void) {
    if(memo_cardless_busy())return false;
    memo_snapshot_t s;app_snapshot(&s);
    int change=atomic_exchange(&sd_probe_ready,0);
    bool prepared;
    if(!s.sd_ready && !s.cardless)prepared=storage_prepare(true);
    else {
        xSemaphoreTake(sd_probe_mutex,portMAX_DELAY);
        esp_err_t result=s.sd_ready?board_sd_check():board_sd_mount();
        bool absent=board_sd_absent();
        xSemaphoreGive(sd_probe_mutex);
        atomic_fetch_add(&sd_probe_checks,1);
        if(s.sd_ready && result==ESP_OK && change!=2)return true;
        if(s.cardless && result!=ESP_OK && absent && !change)return true;
        voice_set_mode(VOICE_OFF);microphone_stop();
        prepared=storage_prepare(s.sd_ready || change==2);
    }
    if(!prepared)return false;
    app_snapshot(&s);
    if(s.sd_ready) {
        uint32_t pending=0;int present=memo_store_pending_id(&store,&pending);
        if(present) {
            /* Do not begin over a recovered card's unfinished recording.
             * The existing worker resumes recovery after AUDIO is released. */
            if(present>0)atomic_store(&recovery_deferred,true);
            state(MEMO_ERROR,"录音恢复暂时繁忙");return false;
        }
    }
    return true;
}
static bool have_space(void) {
    uint64_t total=0,free_bytes=0;
    esp_err_t err=esp_vfs_fat_info("/sdcard",&total,&free_bytes);
    if (err!=ESP_OK) { ESP_LOGE(TAG,"capacity check %s",esp_err_to_name(err)); errno=EIO; return false; }
    if(free_bytes<128*1024) {errno=ENOSPC;return false;}
    return true; // reserve room for header and metadata; never format
}
static esp_err_t microphone_start(void) {
    if(atomic_load(&microphone_open))return ESP_OK;
    esp_err_t err=board_record_open();if(err==ESP_OK)atomic_store(&microphone_open,true);
    return err;
}
static esp_err_t microphone_stop(void) {
    if(!atomic_load(&microphone_open))return ESP_OK;
    esp_err_t err=board_record_close();
    atomic_store(&microphone_open,false);
    return err;
}
static bool listening_allowed(void) {
    memo_network_status_t network;network_sync_get_status(&network);
    memo_usb_status_t usb;memo_usb_get_status(&usb);
    // An unconfigured unit starts a provisioning AP. A computer connection
    // must still permit the wholly offline USB microphone command.
    return atomic_load(&power_listening) && voice_ready && audio_ready &&
           (!network.provisioning || usb.connected);
}
static void update_voice_mode(void) {
    memo_snapshot_t s;app_snapshot(&s);
    voice_mode_t mode=VOICE_OFF;
    // USB microphone selection must work even without a card or Wi-Fi.
    if(listening_allowed()) {
        if(s.state==MEMO_RECORDING)mode=VOICE_RECORDING;
        else if(s.state==MEMO_USB_MIC)mode=VOICE_USB_MIC;
        else if(s.state==MEMO_IDLE || s.state==MEMO_SAVED || s.state==MEMO_ERROR)mode=VOICE_IDLE;
    }
    voice_set_mode(mode);
}
static bool should_listen(const memo_snapshot_t *s) {
    return listening_allowed() &&
        (s->state==MEMO_IDLE || s->state==MEMO_SAVED || s->state==MEMO_ERROR ||
         s->state==MEMO_RECORDING || s->state==MEMO_USB_MIC);
}
static void usb_close_mode(const char *message) {
    memo_usb_close();
    xSemaphoreTake(mutex,portMAX_DELAY);
    model.active_id=0;model.elapsed_ms=0;
    xSemaphoreGive(mutex);
    state(MEMO_IDLE,NULL);
    notice(message);
}
static void usb_no_host_prompt(void) {
#if CONFIG_MEMO_VOCAT16
    extern const uint8_t prompt_start[] asm("_binary_usb_no_host_pcm_start");
    extern const uint8_t prompt_end[] asm("_binary_usb_no_host_pcm_end");
    // The same worker owns input and output; never race a second I2S reader.
    // Gate USB before the speaker, so feedback cannot leak to the computer.
    memo_usb_close();
    if(!audio_ready || !network_sync_prepare_audio(1500))return;
    voice_set_mode(VOICE_OFF);
    if(microphone_stop()==ESP_OK && board_play_open()==ESP_OK) {
        for(const uint8_t *p=prompt_start;p<prompt_end;) {
            size_t bytes=(size_t)(prompt_end-p);
            if(bytes>sizeof(pcm))bytes=sizeof(pcm);
            memcpy(pcm,p,bytes);
            if(board_play_write(pcm,bytes/2)!=ESP_OK)break;
            p+=bytes;
        }
        board_play_close();
    }
    network_sync_audio_done();
#endif
}
static void usb_open_mode(void) {
    memo_network_status_t network;network_sync_get_status(&network);
    memo_usb_status_t usb;memo_usb_get_status(&usb);
    if(!usb.available) {notice("麦克风功能未就绪喵");return;}
    if(!usb.connected || usb.suspended) {notice("还没连上电脑喵");usb_no_host_prompt();return;}
    if(network.provisioning)network_sync_toggle_provisioning();
    if(!audio_ready || microphone_start()!=ESP_OK || board_record_resync()!=ESP_OK) {
        notice("麦克风暂不可用喵");return;
    }
    if(!memo_usb_open()) {notice("电脑连接断开了喵");return;}
    xSemaphoreTake(mutex,portMAX_DELAY);
    model.active_id=0;model.elapsed_ms=0;
    xSemaphoreGive(mutex);
    notice(NULL);state(MEMO_USB_MIC,NULL);
    ESP_LOGI(TAG,"USB_MIC_OPEN no_file no_upload");
}
static void record_stop(bool valid, bool emergency) {
    uint32_t lost=board_record_overflows()-recording_overflows;
    if(valid && lost) {valid=false;errno=EOVERFLOW;ESP_LOGE(TAG,"RECORD_OVERRUN at stop count=%"PRIu32,lost);}
    const int cause=errno;
    memo_snapshot_t current;app_snapshot(&current);
    if(current.cardless) {
        voice_set_mode(VOICE_OFF);
        if(emergency || !valid || !voice_ready)microphone_stop();
        char reason[80]={0};
        if(!valid) {
            memo_cardless_error(reason,sizeof(reason));
            if(!reason[0])snprintf(reason,sizeof(reason),"%s",lost?"音频采集中断":"录音输入中断");
        } else if(emergency)snprintf(reason,sizeof(reason),"低电量停止录音");
        memo_cardless_finish(!valid || emergency,reason);
        if(!emergency)network_sync_audio_done();
        merge_index(0);
        state(valid?MEMO_SAVED:MEMO_ERROR,reason);
        ESP_LOGI(TAG,"CARDLESS_STOP id=%"PRIu32" interrupted=%d",current.active_id,!valid || emergency);
        return;
    }
    voice_set_mode(VOICE_OFF);
    esp_err_t audio_err=(emergency || !valid || !voice_ready)?microphone_stop():ESP_OK;
    if (!valid || audio_err!=ESP_OK) {
        memo_store_abort(&store);
        if(!emergency)network_sync_audio_done();
        errno=valid?EIO:cause; failure("录音失败，未保存"); return;
    }
    state(MEMO_SAVING,NULL);
    memo_record_t record;
    if (memo_store_finish(&store,&record)) {network_sync_audio_done();failure("保存失败，请检查卡"); return;}
    ESP_LOGI(TAG,"SAVED id=%"PRIu32" path=%s bytes=%"PRIu32" duration_ms=%"PRIu32" wall_ms=%"PRId64,
        record.id,record.path,record.data_bytes,record.duration_ms,(esp_timer_get_time()-started_us)/1000);
    ESP_LOGI(TAG,"RECORD_INPUT_OVERRUNS %"PRIu32,lost);
    network_sync_notify_record(record.id);
    if(!emergency)network_sync_audio_done();
    if (add_to_index(&record)) {failure("记录已写入，列表错误");return;}
    state(MEMO_SAVED,NULL);
}
static void playback_stop(bool valid, bool release_network) {
    const int cause=errno;
    bool active=playing!=NULL;
    int closed=active?fclose(playing):0; playing=NULL;
    esp_err_t err=active?board_play_close():ESP_OK;
    if(release_network && active)network_sync_audio_done();
    if (!valid || closed || err!=ESP_OK) {errno=valid?EIO:cause;failure("播放失败，请检查卡");return;}
    ESP_LOGI(TAG,"PLAY_END bytes=%"PRIu32,play_bytes);
    play_remaining=0;play_bytes=0;
    xSemaphoreTake(mutex,portMAX_DELAY);model.elapsed_ms=0;model.active_id=0;xSemaphoreGive(mutex);
    state(MEMO_IDLE,NULL);
}
static void playback_pause(void) {
    if(!playing) {errno=EIO;failure("播放暂停失败");return;}
    if(!play_remaining) {playback_stop(true,true);return;}
    esp_err_t err=board_play_close(); // Drain the queued 128 ms before saving the byte offset.
    int closed=fclose(playing);playing=NULL;
    network_sync_audio_done();
    if(err!=ESP_OK || closed) {errno=EIO;failure("播放暂停失败");return;}
    state(MEMO_PAUSED,NULL);
    memo_snapshot_t snapshot;app_snapshot(&snapshot);
    ESP_LOGI(TAG,"PLAY_PAUSE id=%"PRIu32" bytes=%"PRIu32" ms=%"PRIu32,
             snapshot.active_id,play_bytes,play_bytes/32);
}
static void playback_resume(void) {
    memo_snapshot_t s;app_snapshot(&s);
    if(!s.sd_ready || !audio_ready || !s.active_id) {errno=EIO;failure("无法继续播放");return;}
    if(!network_sync_prepare_audio(1500)) {
        xSemaphoreTake(mutex,portMAX_DELAY);
        snprintf(model.error,sizeof(model.error),"同步正忙，请稍后续播喵");model.revision++;
        xSemaphoreGive(mutex);
        ESP_LOGW(TAG,"PLAY_RESUME_BUSY id=%"PRIu32,s.active_id);
        return;
    }
    char path[MEMO_PATH_MAX];memo_record_t record;memo_audio_identity_t current_identity;
    if(board_sd_check()!=ESP_OK || memo_store_path(&store,s.active_id,path,sizeof(path)) ||
       memo_store_validate(path,&record) ||
       record.data_bytes!=play_bytes+play_remaining ||
       !network_sync_hash_local_audio(path,&current_identity) ||
       !memo_sync_audio_matches(&play_identity,&current_identity) ||
       play_bytes>(uint32_t)(LONG_MAX-44L) ||
       (playing=fopen(path,"rb"))==NULL ||
       fseek(playing,44L+(long)play_bytes,SEEK_SET)!=0) {
        if(playing)fclose(playing);
        playing=NULL;network_sync_audio_done();errno=EIO;failure("无法继续播放");return;
    }
    if(microphone_stop()!=ESP_OK || board_play_open()!=ESP_OK) {
        fclose(playing);playing=NULL;network_sync_audio_done();errno=EIO;
        failure("扬声器打开失败");return;
    }
    state(MEMO_PLAYING,NULL);
    ESP_LOGI(TAG,"PLAY_RESUME id=%"PRIu32" bytes=%"PRIu32" remaining=%"PRIu32,
             s.active_id,play_bytes,play_remaining);
}
static void start_record(void) {
    memo_snapshot_t s;app_snapshot(&s);
    if (!audio_ready) {failure("音频初始化失败");return;}
    if (atomic_load(&recovery_deferred)) {failure("电量不足，恢复暂缓");return;}
    if (memo_cardless_busy()) {failure("无卡录音需联网且上传空闲");return;}
    if (!network_sync_prepare_audio(1500)) {failure("网络同步正忙，请重试");return;}
    if(!storage_check_for_operation()) {network_sync_audio_done();return;}
    app_snapshot(&s);
    if(s.cardless && !s.cardless_ready) {network_sync_audio_done();failure("无卡录音需联网且上传空闲");return;}
    if(s.cardless) {
        if(microphone_start()!=ESP_OK || board_record_resync()!=ESP_OK) {
            microphone_stop();network_sync_audio_done();failure("麦克风打开失败");return;
        }
        memo_network_status_t network;network_sync_get_status(&network);
        uint32_t floor=latest_visible_id();
        if(network.last_uploaded_id>floor)floor=network.last_uploaded_id;
        uint32_t id=0;
        if(memo_cardless_begin(floor,&id)!=ESP_OK) {
            char error[80];memo_cardless_error(error,sizeof(error));
            network_sync_audio_done();failure(error[0]?error:"无卡录音暂不可用");return;
        }
        if(board_record_resync()!=ESP_OK) {
            memo_cardless_finish(true,"麦克风同步失败");
            microphone_stop();network_sync_audio_done();failure("麦克风同步失败");return;
        }
        started_us=esp_timer_get_time();recording_overflows=board_record_overflows();
        xSemaphoreTake(mutex,portMAX_DELAY);model.elapsed_ms=0;model.active_id=id;xSemaphoreGive(mutex);
        state(MEMO_RECORDING,NULL);
        ESP_LOGI(TAG,"CARDLESS_BEGIN id=%"PRIu32" format=16000/opus/mono",id);
        return;
    }
    bool injected=fault_space;fault_space=false;
    if (injected || !have_space()) {
        network_sync_audio_done();
        failure(injected?"测试：空间不足，未保存":"空间不足或存储卡错误");return;
    }
    uint32_t floor=0,reserved=0;
    if(memo_store_max_id(&store,&floor)) {network_sync_audio_done();failure("录音目录读取失败");return;}
    if(latest_visible_id()>floor)floor=latest_visible_id();
    bool allocated=memo_record_ids_reserve(floor,&reserved);
    int opened=allocated?memo_store_begin_reserved(&store,reserved):memo_store_begin_after(&store,floor);
    if(opened) {network_sync_audio_done();failure("无法创建录音文件");return;}
    if(allocated)network_sync_track_record(store.id);
    else ESP_LOGW(TAG,"LOCAL_ID_FALLBACK id=%"PRIu32" automatic_sync=0",store.id);
    if (microphone_start()!=ESP_OK) {memo_store_abort(&store);network_sync_audio_done();failure("麦克风打开失败");return;}
    if (board_record_resync()!=ESP_OK) {
        memo_store_abort(&store);microphone_stop();network_sync_audio_done();failure("麦克风同步失败");return;
    }
    started_us=esp_timer_get_time();recording_overflows=board_record_overflows();
    next_checkpoint_bytes=RECORD_CHECKPOINT_BYTES;
    xSemaphoreTake(mutex,portMAX_DELAY);model.elapsed_ms=0;model.active_id=store.id;xSemaphoreGive(mutex);
    state(MEMO_RECORDING,NULL);
    ESP_LOGI(TAG,"RECORD_BEGIN id=%"PRIu32" format=16000/16/mono",store.id);
}
static void start_play(uint32_t id) {
    memo_snapshot_t s;app_snapshot(&s);
    if(id & MEMO_CARDLESS_ID_FLAG) {failure("无卡记录不提供回放");return;}
    if (!audio_ready) {failure("存储卡或音频未就绪");return;}
    if (!network_sync_prepare_audio(1500)) {failure("网络同步正忙，请重试");return;}
    if(!storage_check_for_operation()) {network_sync_audio_done();return;}
    app_snapshot(&s);
    if(!s.sd_ready) {network_sync_audio_done();failure("存储卡或音频未就绪");return;}
    char path[MEMO_PATH_MAX]; memo_record_t r;
    if (memo_store_path(&store,id,path,sizeof(path)) || memo_store_validate(path,&r) ||
        !network_sync_hash_local_audio(path,&play_identity)) {network_sync_audio_done();failure("录音文件无效");return;}
    playing=fopen(path,"rb");
    if (!playing || fseek(playing,44,SEEK_SET)) {if(playing)fclose(playing);playing=NULL;network_sync_audio_done();failure("无法读取录音");return;}
    voice_set_mode(VOICE_OFF);
    if (microphone_stop()!=ESP_OK || board_play_open()!=ESP_OK) {fclose(playing);playing=NULL;network_sync_audio_done();failure("扬声器打开失败");return;}
    play_remaining=r.data_bytes;play_bytes=0;
    xSemaphoreTake(mutex,portMAX_DELAY);model.active_id=id;model.elapsed_ms=0;xSemaphoreGive(mutex);
    state(MEMO_PLAYING,NULL);ESP_LOGI(TAG,"PLAY_BEGIN id=%"PRIu32,id);
}
static void dump_record(uint32_t id) {
    if (!network_sync_prepare_audio(1500)) {printf("DUMP_ERROR %"PRIu32"\n",id);return;}
    char path[MEMO_PATH_MAX];memo_record_t r;
    if (memo_store_path(&store,id,path,sizeof(path)) || memo_store_validate(path,&r)) {network_sync_audio_done();printf("DUMP_ERROR %"PRIu32"\n",id);return;}
    FILE *f=fopen(path,"rb");if(!f){network_sync_audio_done();printf("DUMP_ERROR %"PRIu32"\n",id);return;}
    uint8_t block[384];unsigned char encoded[513];size_t n,out,offset=0;
    memo_usb_set_bulk_output(true);
    clearerr(stdout);
    bool output_ok=printf("DUMP_BEGIN %"PRIu32" %"PRIu32"\n",id,r.data_bytes+44)>=0;
    while(output_ok && (n=fread(block,1,sizeof(block),f))>0) {
        if(mbedtls_base64_encode(encoded,sizeof(encoded),&out,block,n)) break;
        output_ok=printf("DATA %zu %.*s\n",offset,(int)out,encoded)>=0;
        if(!output_ok)break;
        offset+=n;
        if ((offset / sizeof(block)) % 32 == 0) vTaskDelay(1);
    }
    bool good=output_ok && !ferror(stdout) && !ferror(f) && offset==r.data_bytes+44;
    if(fclose(f))good=false;
    network_sync_audio_done();
    printf("DUMP_%s %"PRIu32" %zu\n",good?"END":"ERROR",id,offset);fflush(stdout);
    memo_usb_set_bulk_output(false);
}
static void inspect_partial(uint32_t id) {
    if(!network_sync_prepare_audio(1500)) {printf("PARTIAL_ERROR %"PRIu32" busy\n",id);fflush(stdout);return;}
    memo_record_t record;
    if(memo_store_inspect_partial(&store,id,&record))
        printf("PARTIAL_ERROR %"PRIu32" errno=%d\n",id,errno);
    else
        printf("PARTIAL id=%"PRIu32" bytes=%"PRIu32" duration_ms=%"PRIu32"\n",
               record.id,record.data_bytes,record.duration_ms);
    network_sync_audio_done();
    fflush(stdout);
}
static void recover_partial(uint32_t id) {
    if(!network_sync_prepare_audio(1500)) {printf("RECOVER_ERROR %"PRIu32" busy\n",id);return;}
    memo_record_t record;
    if(memo_store_recover_partial(&store,id,&record)) {
        int error=errno;network_sync_audio_done();
        printf("RECOVER_ERROR %"PRIu32" errno=%d\n",id,error);fflush(stdout);return;
    }
    ESP_LOGI(TAG,"RECOVERED id=%"PRIu32" path=%s bytes=%"PRIu32" duration_ms=%"PRIu32,
             record.id,record.path,record.data_bytes,record.duration_ms);
    network_sync_notify_record(record.id);network_sync_audio_done();
    if(add_to_index(&record)) {failure("录音已恢复，列表错误");return;}
    state(MEMO_SAVED,NULL);
}
static void discard_partial(uint32_t id) {
    if(!network_sync_prepare_audio(1500)) {
        printf("DISCARD_ERROR %"PRIu32" busy\n",id);fflush(stdout);return;
    }
    int result=memo_store_discard_partial(&store,id);
    int error=errno;
    network_sync_audio_done();
    if(result)printf("DISCARD_ERROR %"PRIu32" errno=%d\n",id,error);
    else printf("DISCARDED %"PRIu32"\n",id);
    fflush(stdout);
}
static void recover_pending(void) {
    atomic_store(&recovery_deferred,false);
    if(!network_sync_prepare_audio(5000)) {
        atomic_store(&recovery_retry_after_us,esp_timer_get_time()+5000000);
        atomic_store(&recovery_deferred,true);
        set_recovering(false);state(MEMO_ERROR,"录音恢复暂时繁忙");return;
    }
    set_recovering(true);
    state(MEMO_SAVING,NULL);
    memo_record_t record={0};uint32_t pending=0;
    int result=memo_store_recover_pending(&store,&record,&pending);
    int error=errno;
    if(result<0) {
        network_sync_audio_done();set_recovering(false);
        ESP_LOGE(TAG,"AUTO_RECOVERY_FAILED id=%"PRIu32" errno=%d",pending,error);
        state(MEMO_ERROR,"录音恢复失败");return;
    }
    if(result==1) {
        ESP_LOGI(TAG,"AUTO_RECOVERED id=%"PRIu32" bytes=%"PRIu32" duration_ms=%"PRIu32,
                 record.id,record.data_bytes,record.duration_ms);
        network_sync_notify_record(record.id);
        network_sync_audio_done();
        if(add_to_index(&record)) {
            set_recovering(false);failure("录音已恢复，列表错误");return;
        }
        set_recovering(false);state(MEMO_SAVED,NULL);return;
    }
    network_sync_audio_done();set_recovering(false);state(MEMO_IDLE,NULL);
    ESP_LOGI(TAG,"AUTO_RECOVERY_NONE pending=%"PRIu32,pending);
}
static bool sd_recovery_ready(void) {
    memo_snapshot_t s;app_snapshot(&s);
    if(!s.sd_ready)return false;
    if(!network_sync_prepare_audio(1500)) {
        atomic_store(&recovery_deferred,true);return false;
    }
    uint32_t pending=0;
    int present=memo_store_pending_id(&store,&pending);
    network_sync_audio_done();
    if(present!=1) {
        if(present<0)ESP_LOGE(TAG,"AUTO_RECOVERY_MARKER_INVALID errno=%d",errno);
        return false;
    }
    bool external=board_external_power_present();
    uint16_t raw=0;board_battery_protection_raw(&raw);
    if(power_policy_recovery_ready()) {
        set_recovering(true);state(MEMO_SAVING,NULL);
        ESP_LOGI(TAG,"AUTO_RECOVERY_READY id=%"PRIu32" external=%d raw=%u",pending,external,raw);
        return true;
    }
    atomic_store(&recovery_deferred,true);
    ESP_LOGW(TAG,"AUTO_RECOVERY_DEFERRED id=%"PRIu32" raw=%u external=0",pending,raw);
    return false;
}
static void prepare_power_shutdown(void) {
    const int64_t close_deadline=esp_timer_get_time()+6500000;
    atomic_store(&shutdown_prepared,false);
    atomic_store(&shutdown_failed,false);
    atomic_store(&power_listening,false);
    memo_usb_close();
    if(!network_sync_prepare_audio(2000)) {
        atomic_store(&shutdown_failed,true);
        ESP_LOGE(TAG,"LOW_BATTERY_CLOSE failed: network SD reader busy");
        return;
    }
    while(atomic_load(&sd_probe_busy) && esp_timer_get_time()<close_deadline)
        vTaskDelay(pdMS_TO_TICKS(10));
    if(atomic_load(&sd_probe_busy)) {
        atomic_store(&shutdown_failed,true);return;
    }
    memo_snapshot_t s;app_snapshot(&s);
    if(s.state==MEMO_RECORDING)record_stop(true,true);
    else if(s.state==MEMO_PLAYING || s.state==MEMO_PAUSED)playback_stop(true,false);
    else if(microphone_stop()!=ESP_OK)
        ESP_LOGE(TAG,"LOW_BATTERY_CLOSE microphone close failed");
    while(memo_cardless_busy() && esp_timer_get_time()<close_deadline)
        vTaskDelay(pdMS_TO_TICKS(20));
    if(memo_cardless_busy())ESP_LOGW(TAG,"CARDLESS_LOW_BATTERY deadline; recovery limited to server ACK prefix");
    atomic_store(&shutdown_prepared,true);
    ESP_LOGI(TAG,"LOW_BATTERY_CLOSE ready state=%d",s.state);
}
static void handle_command(command_t *c) {
    memo_snapshot_t s;app_snapshot(&s);
    ESP_LOGI(TAG,"COMMAND source=%s cmd=%d arg=%"PRIu32,c->source,c->cmd,c->arg);
    if(c->cmd==CMD_POWER_SHUTDOWN) {prepare_power_shutdown();return;}
    if(c->cmd==CMD_RECOVER_PENDING) {
        if(s.state==MEMO_USB_MIC || s.state==MEMO_RECORDING || s.state==MEMO_PLAYING || s.state==MEMO_PAUSED) {
            atomic_store(&recovery_deferred,true);return;
        }
        if(!s.sd_ready || !power_policy_recovery_ready()) {
            atomic_store(&recovery_deferred,true);set_recovering(false);
            if(s.state==MEMO_SAVING)state(MEMO_IDLE,NULL);
            return;
        }
        recover_pending();return;
    }
    if(!strcmp(c->source,"voice") && (!voice_request_valid(c->arg) ||
       (c->cmd==CMD_STOP && s.state!=MEMO_RECORDING) ||
       (c->cmd==CMD_START && s.state!=MEMO_IDLE && s.state!=MEMO_SAVED &&
        s.state!=MEMO_ERROR))) {
        ESP_LOGW(TAG,"REJECT stale voice request");return;
    }
    if (!memo_can_command(s.state,c->cmd)) {
        ESP_LOGW(TAG,"REJECT state=%d cmd=%d",s.state,c->cmd);
        if(c->cmd==CMD_SYNC_MODE){printf("SYNC_MODE result=%d\n",(int)MEMO_CONFIG_BUSY);fflush(stdout);}
        return;
    }
    if(c->cmd>=CMD_NEXT && c->cmd<=CMD_REBOOT)voice_set_mode(VOICE_OFF);
    switch(c->cmd) {
        case CMD_VOICE_WAKE:
            xSemaphoreTake(mutex,portMAX_DELAY);model.voice_wake_count++;model.revision++;xSemaphoreGive(mutex);
            notice(NULL);break;
        case CMD_VOICE_CANCEL:notice("好喵，取消了");break;
        case CMD_USB_OPEN:usb_open_mode();break;
        case CMD_USB_CLOSE:usb_close_mode(NULL);break;
        case CMD_USB_MUTE:memo_usb_set_muted(true);notice(NULL);break;
        case CMD_USB_UNMUTE:memo_usb_set_muted(false);notice(NULL);break;
        case CMD_START:notice(NULL);start_record();break;
        case CMD_STOP:if(s.state==MEMO_RECORDING)record_stop(true,false);else playback_stop(true,true);break;
        case CMD_PLAY:start_play(c->arg);break;
        case CMD_PAUSE:playback_pause();break;
        case CMD_RESUME:playback_resume();break;
        case CMD_NEXT:if(s.page+1<s.page_count && show_page(s.page+1))failure("列表读取失败");break;
        case CMD_PREV:if(s.page && show_page(s.page-1))failure("列表读取失败");break;
        case CMD_REFRESH:
            if(!memo_cardless_busy() && network_sync_prepare_audio(3000)) {
                voice_set_mode(VOICE_OFF);microphone_stop();
                if(storage_prepare(true))state(audio_ready?MEMO_IDLE:MEMO_ERROR,audio_ready?NULL:"音频初始化失败");
                network_sync_audio_done();
                if(sd_recovery_ready())recover_pending();
            }
            break;
        case CMD_FAULT_WRITE:
            if(memo_store_inject_write_failure(&store,c->arg))failure("测试注入失败");
            else ESP_LOGW(TAG,"INJECT next recording write failure after %"PRIu32" bytes",c->arg);
            break;
        case CMD_FAULT_SPACE:fault_space=true;ESP_LOGW(TAG,"INJECT next recording low-space branch");break;
        case CMD_DUMP:dump_record(c->arg);break;
        case CMD_PARTIAL:inspect_partial(c->arg);break;
        case CMD_RECOVER:recover_partial(c->arg);break;
        case CMD_DISCARD:discard_partial(c->arg);break;
        case CMD_REBOOT:ESP_LOGI(TAG,"REBOOT idle, all files closed");fflush(stdout);vTaskDelay(pdMS_TO_TICKS(100));esp_restart();break;
        case CMD_BOOTLOADER: {
            memo_network_status_t network;network_sync_get_status(&network);
            if(network.syncing || memo_cardless_busy() || atomic_load(&sd_probe_busy) ||
               s.recovering || atomic_load(&recovery_deferred) ||
               !network_sync_prepare_audio(1500)) {
                printf("BOOTLOADER_ERROR busy\n");fflush(stdout);break;
            }
            voice_set_mode(VOICE_OFF);memo_usb_close();
            if(microphone_stop()!=ESP_OK) {
                network_sync_audio_done();printf("BOOTLOADER_ERROR audio\n");fflush(stdout);break;
            }
            printf("BOOTLOADER_ENTER explicit_idle_maintenance\n");fflush(stdout);
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_err_t err=memo_usb_enter_bootloader();
            network_sync_audio_done();
            printf("BOOTLOADER_ERROR %s\n",esp_err_to_name(err));fflush(stdout);
            break;
        }
        case CMD_IDLE:state(MEMO_IDLE,NULL);break;
        case CMD_PROVISION:network_sync_toggle_provisioning();break;
        case CMD_SYNC_MODE: {
            // NVS save/readback runs on the existing 10 KiB worker, never on
            // the 4 KiB console parser. The original commit/busy guards apply.
            int result=c->arg<=1 && !s.recovering ?
                (int)network_sync_set_local_only(c->arg!=0):(int)MEMO_CONFIG_BUSY;
            printf("SYNC_MODE result=%d\nSYNC_MODE_STACK free=%u\n",result,
                   (unsigned)(uxTaskGetStackHighWaterMark(NULL)*sizeof(StackType_t)));
            fflush(stdout);break;
        }
        case CMD_RECOVER_PENDING:
        case CMD_POWER_SHUTDOWN:break;
    }
}
static void handle(command_t *c) {
    handle_command(c);
    if(!strcmp(c->source,"voice") && c->cmd!=CMD_VOICE_WAKE && c->cmd!=CMD_VOICE_CANCEL)
        voice_command_ack(c->arg);
}
static bool apply_sd_probe(void) {
    memo_snapshot_t s;app_snapshot(&s);
    if(!atomic_load(&sd_probe_ready) || !sd_probe_allowed(&s) || !network_sync_prepare_audio(3000))return false;
    int change=atomic_exchange(&sd_probe_ready,0);
    voice_set_mode(VOICE_OFF);microphone_stop();
    if(storage_prepare(change==2))state(audio_ready?MEMO_IDLE:MEMO_ERROR,audio_ready?NULL:"音频初始化失败");
    network_sync_audio_done();
    if(sd_recovery_ready())recover_pending();
    return true;
}
static void worker(void *arg) {
    (void)arg;uint32_t capacity_ticks=0;
    int64_t metadata_refresh_us=0;
    int64_t recovery_retry_us=0;
    int64_t capture_previous_us=0;
    uint32_t previous_probe_interval=UINT32_MAX;
    for (;;) {
        const int64_t loop_started_us=esp_timer_get_time();
        if(notice_until_us && loop_started_us>=notice_until_us)notice(NULL);
        memo_usb_status_t usb;memo_usb_get_status(&usb);
        memo_snapshot_t current;app_snapshot(&current);
        if(current.state==MEMO_USB_MIC && usb.suspended) {
            if(!usb_suspended_since_us)usb_suspended_since_us=loop_started_us;
        } else usb_suspended_since_us=0;
        if(current.state==MEMO_USB_MIC && (!usb.enabled ||
           (usb_suspended_since_us && loop_started_us-usb_suspended_since_us>=5000000))) {
            usb_close_mode("电脑连接暂停，麦克风已关喵");
            usb_suspended_since_us=0;
        }
        update_voice_mode();
        command_t c;memo_snapshot_t s;app_snapshot(&s);
        bool listening=should_listen(&s);
        TickType_t wait=(listening||s.state==MEMO_RECORDING||s.state==MEMO_PLAYING||s.state==MEMO_USB_MIC)?0:pdMS_TO_TICKS(100);
        if(xQueueReceive(queue,&c,wait)==pdTRUE)handle(&c);
        update_voice_mode();app_snapshot(&s);
        /* Existing audio loop observes only flags. No extra timer wakes the
         * device for media when all demand is gone. */
        uint32_t probe_interval=sd_probe_interval(&s);
        if(probe_interval!=previous_probe_interval) {
            previous_probe_interval=probe_interval;
            if(sd_probe_handle)xTaskNotifyGive(sd_probe_handle);
        }
        // A recovery deferred by an active USB/record/play task must resume
        // after that task ends, without requiring another charger edge.
        if(atomic_load(&recovery_deferred) && s.sd_ready &&
           (s.state==MEMO_IDLE || s.state==MEMO_SAVED || s.state==MEMO_ERROR) &&
           loop_started_us-recovery_retry_us>=1000000) {
            recovery_retry_us=loop_started_us;
            app_power_recover_deferred();
        }
        if(s.state!=MEMO_RECORDING && s.state!=MEMO_PLAYING && s.state!=MEMO_PAUSED && s.state!=MEMO_SAVING && s.state!=MEMO_USB_MIC) {
            if(apply_sd_probe())app_snapshot(&s);
            if(memo_cardless_revision()!=cardless_revision &&
               esp_timer_get_time()-metadata_refresh_us>1000000) {
                merge_index(s.page);metadata_refresh_us=esp_timer_get_time();
            }
        }
        listening=should_listen(&s);
        if(s.state==MEMO_RECORDING || s.state==MEMO_USB_MIC || listening) {
            const bool monitor_rx=(s.cardless && s.state==MEMO_RECORDING) || s.state==MEMO_USB_MIC;
            board_rx_diagnostics_t rx_before={0};
            if(monitor_rx)board_record_rx_diagnostics(&rx_before);
            const int64_t before_read_us=esp_timer_get_time();
            esp_err_t read_err=microphone_start();
            if(read_err==ESP_OK)read_err=board_record_read(pcm,512);
            const int64_t after_read_us=esp_timer_get_time();
            if(monitor_rx && capture_previous_us && after_read_us-capture_previous_us>64000) {
                board_rx_diagnostics_t rx_after;
                board_record_rx_diagnostics(&rx_after);
                const uint32_t diag_now_us=(uint32_t)esp_timer_get_time();
                ESP_LOGW(TAG,"CAPTURE_STALL gap_us=%"PRId64" scheduling_us=%"PRId64
                         " control_us=%"PRId64" read_us=%"PRId64" overflows=%"PRIu32
                         " rx_eofs=%"PRIu32" rx_overflows=%"PRIu32
                         " last_eof_age_us=%"PRIu32" last_overflow_age_us=%"PRIu32
                         " rx_irq_core=%"PRId32" mode=%d",
                         after_read_us-capture_previous_us,loop_started_us-capture_previous_us,
                         before_read_us-loop_started_us,after_read_us-before_read_us,
                         rx_after.overflow_count-recording_overflows,
                         rx_after.eof_count-rx_before.eof_count,
                         rx_after.overflow_count-rx_before.overflow_count,
                         rx_after.eof_count?diag_now_us-rx_after.last_eof_us:UINT32_MAX,
                         rx_after.overflow_count?diag_now_us-rx_after.last_overflow_us:UINT32_MAX,
                         rx_after.irq_core,s.state);
            }
            capture_previous_us=after_read_us;
            if(read_err!=ESP_OK) {
                ESP_LOGE(TAG,"I2S_READ %s",esp_err_to_name(read_err));errno=EIO;
                if(s.state==MEMO_RECORDING)record_stop(false,false);
                else {microphone_stop();failure("麦克风读取失败");}
                continue;
            }
            if(s.state==MEMO_RECORDING) {
                if(board_record_overflows()!=recording_overflows) {
                    ESP_LOGE(TAG,"RECORD_OVERRUN lost input; refusing incomplete save");errno=EOVERFLOW;record_stop(false,false);continue;
                }
                if(s.cardless) {
                    char reason[80];
                    if(memo_cardless_should_stop(reason,sizeof(reason)) || !memo_cardless_append(pcm,512)) {
                        record_stop(false,false);continue;
                    }
                } else if(memo_store_append(&store,pcm,512)) {record_stop(false,false);continue;}
                if(!s.cardless && store.data_bytes>=next_checkpoint_bytes) {
                    int64_t checkpoint_started=esp_timer_get_time();
                    if(memo_store_checkpoint(&store)) {record_stop(false,false);continue;}
                    ESP_LOGI(TAG,"RECORD_CHECKPOINT id=%"PRIu32" bytes=%"PRIu32
                                 " sync_ms=%"PRId64,
                             store.id,store.data_bytes,
                             (esp_timer_get_time()-checkpoint_started)/1000);
                    next_checkpoint_bytes+=RECORD_CHECKPOINT_BYTES;
                }
                unsigned peak=0;for(unsigned i=0;i<512;i++){int a=pcm[i];if(a<0)a=-a;if((unsigned)a>peak)peak=(unsigned)a;}
                xSemaphoreTake(mutex,portMAX_DELAY);model.elapsed_ms=s.cardless?memo_cardless_elapsed_ms():store.data_bytes/32;model.level=meter_level(peak);xSemaphoreGive(mutex);
                if(!s.cardless && ++capacity_ticks%32==0 && !have_space()) {memo_store_abort(&store);microphone_stop();network_sync_audio_done();errno=ENOSPC;failure("空间不足，未保存");continue;}
            }
            if(s.state==MEMO_USB_MIC) {
                memo_usb_submit(pcm,512);
                unsigned peak=0;for(unsigned i=0;i<512;i++){int a=pcm[i];if(a<0)a=-a;if((unsigned)a>peak)peak=(unsigned)a;}
                xSemaphoreTake(mutex,portMAX_DELAY);model.level=meter_level(peak);xSemaphoreGive(mutex);
            }
            if(voice_ready)voice_submit(pcm,512);
        } else if(s.state==MEMO_PLAYING) {
            size_t bytes=play_remaining>sizeof(pcm)?sizeof(pcm):play_remaining;
            if(!bytes) {playback_stop(true,true);continue;}
            if(fread(pcm,1,bytes,playing)!=bytes || board_play_write(pcm,bytes/2)!=ESP_OK) {playback_stop(false,true);continue;}
            play_remaining-=bytes;play_bytes+=bytes;
            xSemaphoreTake(mutex,portMAX_DELAY);model.elapsed_ms=play_bytes/32;xSemaphoreGive(mutex);
        } else if(atomic_load(&microphone_open))microphone_stop();
    }
}

bool app_power_is_busy(void)
{
    memo_snapshot_t snapshot;
    app_snapshot(&snapshot);
    return snapshot.state == MEMO_RECORDING || snapshot.state == MEMO_SAVING ||
           snapshot.state == MEMO_PLAYING || snapshot.state == MEMO_USB_MIC ||
           snapshot.voice_window; // Background media probes are not user activity.
}

bool app_power_set_listening(bool enabled, uint32_t timeout_ms)
{
    if (!enabled && app_power_is_busy()) return false;
    atomic_store(&power_listening, enabled);
    // Publish sleep intent before checking the probe, whose entry rechecks it.
    // Keep the SD controller awake until an in-flight query has released it.
    if(!enabled && atomic_load(&sd_probe_busy)) {
        atomic_store(&power_listening,true);return false;
    }
    if (enabled || !atomic_load(&microphone_open)) return true;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000LL;
    while (atomic_load(&microphone_open) && esp_timer_get_time() < deadline)
        vTaskDelay(pdMS_TO_TICKS(10));
    return !atomic_load(&microphone_open);
}

bool app_power_prepare_shutdown(uint32_t timeout_ms)
{
    if(atomic_load(&shutdown_prepared))return true;
    atomic_store(&shutdown_failed,false);
    if(!app_command(CMD_POWER_SHUTDOWN,0,"low_battery"))return false;
    int64_t deadline=esp_timer_get_time()+(int64_t)timeout_ms*1000LL;
    while(!atomic_load(&shutdown_prepared) && !atomic_load(&shutdown_failed) &&
          esp_timer_get_time()<deadline)
        vTaskDelay(pdMS_TO_TICKS(10));
    return atomic_load(&shutdown_prepared);
}

void app_power_cancel_shutdown(void)
{
    bool prepared=atomic_exchange(&shutdown_prepared,false);
    bool failed=atomic_exchange(&shutdown_failed,false);
    if(prepared || failed) {
        network_sync_audio_done();
        atomic_store(&power_listening,true);
        ESP_LOGI(TAG,"LOW_BATTERY_CLOSE cancelled external power restored");
    }
}

void app_power_recover_deferred(void)
{
    memo_snapshot_t s;app_snapshot(&s);
    if(app_power_is_busy() || s.state==MEMO_PAUSED || !s.sd_ready ||
       !atomic_load(&power_listening) ||
       esp_timer_get_time()<atomic_load(&recovery_retry_after_us) ||
       !power_policy_recovery_ready() ||
       !atomic_exchange(&recovery_deferred,false))return;
    // The power task only queues the request. The audio worker must recheck
    // its current mode before changing state: a USB-open/record command may
    // have won the race since the snapshot above.
    if(!app_command(CMD_RECOVER_PENDING,0,"power_recovery")) {
        atomic_store(&recovery_deferred,true);
    }
}

static void print_status(void) {
    const esp_app_desc_t *desc=esp_app_get_description();
    printf("IDENTITY project=%s version=%s idf=%s board=%s offline_core=1 cloud_sync=1\n",desc->project_name,desc->version,desc->idf_ver,board_name());
    memo_snapshot_t s;app_snapshot(&s);
    printf("VOICE ready=%d window=%d drops=%"PRIu32"\n",s.voice_ready,s.voice_window,s.voice_drops);
    printf("USB_MIC available=%d connected=%d suspended=%d requested=%d active=%d muted=%d local_muted=%d\n",
        s.usb_available,s.usb_connected,s.usb_suspended,s.usb_streaming,
        s.state==MEMO_USB_MIC,s.usb_muted,s.usb_local_muted);
    memo_usb_status_t usb;memo_usb_get_status(&usb);
    printf("USB_AUDIO dropped_samples=%"PRIu32" underrun_samples=%"PRIu32" cdc_dropped_bytes=%"PRIu32"\n",
        usb.dropped_samples,usb.underrun_samples,usb.cdc_tx_dropped_bytes);
    printf("USB_BUS frame=%"PRIu32" age_ms=%"PRIu32" watchdog_losses=%"PRIu32"\n",
        usb.bus_frame,usb.bus_age_ms,usb.bus_watchdog_losses);
    if(s.notice[0])printf("NOTICE %s\n",s.notice);
    printf("SD_PROBE checks=%u wakes=%u view=%d work=%d scans=%"PRIu32"\n",
        atomic_load(&sd_probe_checks),atomic_load(&sd_probe_wakes),atomic_load(&sd_view_visible),
        network_sync_sd_work_pending(),network_sync_sd_scan_count());
    printf("CARDLESS mode=%d ready=%d transport_busy=%d records=%zu\n",s.cardless,s.cardless_ready,memo_cardless_busy(),memo_cardless_count());
    printf("STATUS state=%d sd=%d count=%zu page=%zu id=%"PRIu32" ms=%"PRIu32" level=%u error=%s\n",
        s.state,s.sd_ready,s.count,s.page,s.active_id,s.elapsed_ms,s.level,s.error);
    memo_network_status_t network;network_sync_get_status(&network);
    printf("NETWORK initialized=%d configured=%d connected=%d provisioning=%d syncing=%d device=%s cursor=%"PRIu32" last=%"PRIu32" active=%"PRIu32" queued=%"PRIu32" manual=%d error=%s\n",
        network.initialized,network.configured,network.connected,network.provisioning,network.syncing,
        network.device_id,network.cursor_id,network.last_uploaded_id,network.active_sync_id,
        network.queued_sync_id,network.queued_sync_manual,network.error);
    memo_power_status_t power;power_policy_get_status(&power);
    printf("POWER supported=%d initialized=%d external=%d busy=%d stage=%d idle_s=%"PRIu32
           " battery_valid=%d battery_raw=%u battery_percent=%u low=%d critical_pending=%d guard_raw=%u\n",
        power.supported,power.initialized,power.external_power,power.busy,power.stage,power.idle_seconds,
        power.battery_valid,power.battery_raw,power.battery_percent,power.battery_low,
        power.critical_pending,power.battery_guard_raw);
    for(size_t i=0;i<s.item_count;i++)printf("ITEM id=%"PRIu32" ms=%"PRIu32"\n",s.items[i].id,s.items[i].duration_ms);
    memo_record_sync_t detail;network_sync_record_status(&detail);
    if(detail.id)printf("RECORD_SYNC id=%"PRIu32" state=%d percent=%u pending=%d cardless=%d retry=%d interrupted=%d error=%s\n",
        detail.id,detail.state,detail.upload_percent,detail.request_pending,
        detail.cardless,detail.can_retry,detail.interrupted,detail.error);
    fflush(stdout);
}
static void console(void *arg) {
    (void)arg; char line[96];size_t len=0;
    for(;;) {
        int ch=getchar();if(ch==EOF){clearerr(stdin);vTaskDelay(pdMS_TO_TICKS(20));continue;}
        if(ch=='\r')continue;
        if(ch!='\n'){if(len<sizeof(line)-1)line[len++]=(char)ch;else len=0;continue;}
        line[len]=0;len=0;char verb[32];uint32_t val=0;
        if(sscanf(line,"%31s %"SCNu32,verb,&val)<1)continue;
        if(!strcmp(verb,"cardless_test")) {
            unsigned kind=0;uint32_t value=0;char extra;
            memo_snapshot_t snapshot;app_snapshot(&snapshot);
            bool valid=sscanf(line,"%*s %u %"SCNu32" %c",&kind,&value,&extra)==2;
            printf("CARDLESS_TEST accepted=%d kind=%u value=%"PRIu32"\n",
                   valid && snapshot.cardless && memo_cardless_test_fault(kind,value),kind,value);
            continue;
        }
        if(!strcmp(verb,"gauge_diag")) {
            board_battery_diagnostics_t gauge={0};
            esp_err_t err=board_battery_diagnostics(&gauge);
            if(err!=ESP_OK)printf("GAUGE_DIAG error=%s\n",esp_err_to_name(err));
            else printf("GAUGE_DIAG soc=%u rm_mah=%u fcc_mah=%u design_mah=%u "
                        "tte_min=%u cycles=%u mv=%u current_ma=%d average_ma=%d flags=0x%04x "
                        "op=0x%04x profile=%d changed=%d old_fcc=%u old_design=%u rollback=%d/%d reinit=%d ram_backup=%u/%u\n",
                        (unsigned)gauge.soc_percent,(unsigned)gauge.remaining_mah,
                        (unsigned)gauge.full_mah,(unsigned)gauge.design_mah,
                        (unsigned)gauge.time_to_empty_min,(unsigned)gauge.cycle_count,
                        (unsigned)gauge.voltage_mv,(int)gauge.current_ma,
                        (int)gauge.average_ma,(unsigned)gauge.flags,(unsigned)gauge.operation_status,
                        gauge.profile_ready,gauge.profile_changed,(unsigned)gauge.previous_full_mah,
                        (unsigned)gauge.previous_design_mah,gauge.rollback_attempted,gauge.rollback_verified,
                        gauge.profile_reinitialized,(unsigned)gauge.previous_ram_full_mah,
                        (unsigned)gauge.previous_ram_design_mah);
            fflush(stdout);continue;
        }
        if(!strcmp(verb,"status")||!strcmp(verb,"list")||!strcmp(verb,"net_status")){print_status();continue;}
        if(!strcmp(verb,"memory")){
            memory_checkpoint("serial");
            printf("MEMORY_TASK detail_stack_free=%"PRIu32" http_stop_stack_free=%"PRIu32" reset_reason=%d\n",
                   network_sync_detail_stack_free(),network_sync_http_stack_free(),(int)esp_reset_reason());
            fflush(stdout);continue;
        }
        if(!strcmp(verb,"sync_mode")) {
            char mode[12],extra;
            bool valid=sscanf(line,"%*s %11s %c",mode,&extra)==1 &&
                       (!strcmp(mode,"local") || !strcmp(mode,"service"));
            if(!valid || !app_command(CMD_SYNC_MODE,!strcmp(mode,"local"),"serial")) {
                printf("SYNC_MODE result=%d\n",valid?(int)MEMO_CONFIG_BUSY:-1);fflush(stdout);
            }
            continue;
        }
        if(!strcmp(verb,"detail")){network_sync_select_record(val);print_status();continue;}
        if(!strcmp(verb,"retry_sync")){printf("RETRY_ACCEPTED %d\n",network_sync_retry_record(val));continue;}
        if(!strcmp(verb,"provision")){app_command(CMD_PROVISION,0,"serial");continue;}
        if(!strcmp(verb,"usb_open")){app_command(CMD_USB_OPEN,0,"serial");continue;}
        if(!strcmp(verb,"usb_close")){app_command(CMD_USB_CLOSE,0,"serial");continue;}
        if(!strcmp(verb,"usb_mute")){app_command(CMD_USB_MUTE,0,"serial");continue;}
        if(!strcmp(verb,"usb_unmute")){app_command(CMD_USB_UNMUTE,0,"serial");continue;}
        if(!strcmp(verb,"bootloader")){app_command(CMD_BOOTLOADER,0,"serial");continue;}
        if(!strcmp(verb,"pause")){app_command(CMD_PAUSE,0,"serial");continue;}
        if(!strcmp(verb,"resume")){app_command(CMD_RESUME,0,"serial");continue;}
        const char *names[]={"start","stop","play","next","prev","refresh","fault_write","fault_space","dump","partial","recover","discard","reboot"};
        bool found=false;for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++)if(!strcmp(verb,names[i])){app_command((memo_cmd_t)i,val,"serial");found=true;break;}
        if(!found)printf("HELP status list net_status gauge_diag memory sync_mode local|service provision start stop play ID pause resume usb_open usb_close usb_mute usb_unmute next prev refresh fault_write BYTES fault_space dump ID partial ID recover ID discard ID reboot bootloader cardless_test KIND VALUE\n");
    }
}
/* Healthy mounted cards need only CMD13; no FAT scan or write. The task
 * is notification-driven and has a timer only while real demand exists. */
static void sd_probe_once(void) {
    memo_snapshot_t s;app_snapshot(&s);
    if(atomic_load(&sd_probe_ready) || !sd_probe_allowed(&s) ||
       (!atomic_load(&sd_view_visible) && !network_sync_sd_work_pending()))return;
    if(xSemaphoreTake(sd_probe_mutex,0)!=pdTRUE)return;
    atomic_store(&sd_probe_busy,true);
    app_snapshot(&s);
    if(sd_probe_allowed(&s) && network_sync_try_sd_probe()) {
        // Audio/config may have won before the exclusive claim. Check again.
        app_snapshot(&s);
        if(sd_probe_allowed(&s)) {
            atomic_fetch_add(&sd_probe_checks,1);
            esp_err_t err=s.sd_ready?board_sd_check():board_sd_mount();
            int change=s.sd_ready?(err!=ESP_OK?2:0):(err==ESP_OK || !board_sd_absent()?1:0);
            if(change) {
                atomic_store(&sd_probe_ready,change);
                ESP_LOGI(TAG,"SD_MEDIA_CHANGE kind=%d result=%s",change,esp_err_to_name(err));
            }
        }
        network_sync_sd_probe_done();
    }
    atomic_store(&sd_probe_busy,false);
    xSemaphoreGive(sd_probe_mutex);
}
static void sd_probe_task(void *arg) {
    (void)arg;
    int64_t next_us=0;
    for(;;) {
        memo_snapshot_t s;app_snapshot(&s);
        uint32_t interval=sd_probe_interval(&s);
        TickType_t wait=portMAX_DELAY;
        if(interval) {
            int64_t now=esp_timer_get_time();
            if(atomic_exchange(&sd_view_requested,false) || now>=next_us) {
                sd_probe_once();
                next_us=esp_timer_get_time()+(int64_t)interval*1000;
            }
            int64_t remaining=next_us-esp_timer_get_time();
            wait=pdMS_TO_TICKS((uint32_t)(remaining>0?(remaining+999)/1000:1));
            if(!wait)wait=1;
        } else next_us=0;
        ulTaskNotifyTake(pdTRUE,wait);
        atomic_fetch_add(&sd_probe_wakes,1);
    }
}
void app_main(void) {
    mutex=xSemaphoreCreateMutex();queue=xQueueCreate(8,sizeof(command_t));
    sd_probe_mutex=xSemaphoreCreateMutex();
    if(!mutex||!queue||!sd_probe_mutex)abort();
#if CONFIG_MEMO_VOCAT16
    esp_err_t usb_result=memo_usb_init();
    if(usb_result!=ESP_OK)ESP_LOGE(TAG,"USB microphone unavailable: %s",esp_err_to_name(usb_result));
#else
    usb_serial_jtag_driver_config_t usb={.rx_buffer_size=256,.tx_buffer_size=8192};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));usb_serial_jtag_vfs_use_driver();
#endif
    setvbuf(stdin,NULL,_IONBF,0);
    const esp_app_desc_t *desc=esp_app_get_description();
    ESP_LOGI(TAG,"PROJECT=%s VERSION=%s IDF=%s built=%s %s OFFLINE_CORE CLOUD_SYNC",desc->project_name,desc->version,desc->idf_ver,desc->date,desc->time);
#if CONFIG_MEMO_BOARD_UNCONFIRMED
    ESP_LOGE(TAG,"BOARD UNCONFIRMED: no peripheral init. Do not flash this build for acceptance.");
    return;
#endif
    ESP_LOGI(TAG,"BOARD=%s",board_name());
    bool power_monitor_ok=board_power_monitor_init()==ESP_OK;
#if CONFIG_MEMO_VOCAT16
    if(esp_reset_reason()==ESP_RST_DEEPSLEEP && power_monitor_ok) {
        uint16_t charge=0;
        bool valid=board_battery_protection_raw(&charge);
        if(valid && !board_external_power_present() && charge<15U) {
            ESP_LOGW(TAG,"LOW_BATTERY_RECOVERY_DEFERRED soc=%u; retry after charger/15%%",charge);
            board_power_off();
        }
    }
#endif
    bool display_ok=board_init_display()==ESP_OK;
    memory_checkpoint("before_metadata");
    if(!memo_record_ids_init())ESP_LOGW(TAG,"ID_STORAGE_UNAVAILABLE exclusive SD recording remains local");
    cardless_available=memo_cardless_init()==ESP_OK;
    memory_checkpoint("after_metadata");
    if(!cardless_available)ESP_LOGE(TAG,"CARDLESS_METADATA_UNAVAILABLE SD core remains available");
    bool storage_ok=storage_prepare(false);
    audio_ready=board_audio_init()==ESP_OK;
    if(storage_ok) {
        if(!audio_ready)state(MEMO_ERROR,"音频初始化失败");
        else state(MEMO_IDLE,NULL);
    }
    voice_ready=audio_ready && voice_init();
    memory_checkpoint("after_voice");
    if(!voice_ready)ESP_LOGE(TAG,"VOICE_UNAVAILABLE touch recording remains available");

    bool start_recovery=sd_recovery_ready();
    uint32_t latest=latest_visible_id(); // Capture before the audio worker owns/replaces the index.
    if(display_ok && lvgl_port_lock(1000)){ui_init();lvgl_port_unlock();}
    else ESP_LOGE(TAG,"UI initialization failed");
    /* Reserve the two offline-core task stacks before Wi-Fi allocates its
       driver buffers.  A networking failure must never prevent recording. */
    /* The level meter uses float: IDF otherwise pins this task implicitly to
     * whichever core first executes it. Keep time-critical RX off Wi-Fi's
     * core 0 now that cardless capture and TLS uploads run concurrently. */
    if(xTaskCreatePinnedToCore(worker,"memo_audio",10240,NULL,5,NULL,1)!=pdPASS)abort();
    if(xTaskCreate(console,"memo_console",4096,NULL,2,NULL)!=pdPASS)abort();
    if(start_recovery && !app_command(CMD_RECOVER_PENDING,0,"power_recovery")) {
        set_recovering(false);state(MEMO_ERROR,"录音恢复暂时繁忙");
    }
    if(xTaskCreate(network_boot,"memo_net_boot",5120,(void *)(uintptr_t)latest,1,NULL)!=pdPASS)
        ESP_LOGE(TAG,"NETWORK_UNAVAILABLE offline recording remains available");
    if(xTaskCreate(sd_probe_task,"memo_sd_probe",4096,NULL,1,&sd_probe_handle)!=pdPASS)
        ESP_LOGW(TAG,"SD auto-detect unavailable; use refresh while idle");
    power_policy_init();
    ESP_LOGI(TAG,"READY display=%d sd=%d audio=%d",display_ok,model.sd_ready,audio_ready);
}
