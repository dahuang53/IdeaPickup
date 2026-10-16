// Offline recognizer. Owns no codec or SD resource; bounded copies from the audio worker.
#include "voice.h"
#include "voice_gate.h"
#include "app.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "model_path.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#define FRAME 512
static const char *TAG="memo_voice";
static const esp_afe_sr_iface_t *afe;
static esp_afe_sr_data_t *afe_data;
static const esp_mn_iface_t *mn;
static model_iface_data_t *mn_data;
static srmodel_list_t *models;
static QueueHandle_t frames;
static portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
static struct {
    bool ready,window,ack_pending; voice_mode_t mode;
    uint32_t generation,drops,ack_generation; int64_t after;
} shared;
typedef struct { int64_t time; uint32_t generation; int16_t pcm[FRAME]; } frame_t;
typedef struct { voice_mode_t mode; uint32_t generation; int64_t after; } control_t;
static control_t control(void) {
    portENTER_CRITICAL(&lock);
    control_t c={shared.mode,shared.generation,shared.after};
    portEXIT_CRITICAL(&lock);return c;
}
void voice_status(bool *ready,bool *window,uint32_t *drops) {
    portENTER_CRITICAL(&lock);*ready=shared.ready;*window=shared.window;*drops=shared.drops;portEXIT_CRITICAL(&lock);
}
void voice_set_mode(voice_mode_t mode) {
    portENTER_CRITICAL(&lock);
    if(shared.mode!=mode) {
        shared.mode=mode;shared.generation++;shared.window=false;
        // Feed silence first, then discard the processing tail. Old detections
        // cannot act after playback, SD operations, or a recording transition.
        shared.after=esp_timer_get_time()+800000;
    }
    portEXIT_CRITICAL(&lock);
}
bool voice_request_valid(uint32_t generation) {
    portENTER_CRITICAL(&lock);
    bool valid=shared.ready && shared.mode!=VOICE_OFF && shared.generation==generation;
    portEXIT_CRITICAL(&lock);return valid;
}
void voice_command_ack(uint32_t generation) {
    portENTER_CRITICAL(&lock);
    shared.ack_generation=generation;
    shared.ack_pending=true;
    portEXIT_CRITICAL(&lock);
}
static bool take_command_ack(uint32_t generation) {
    portENTER_CRITICAL(&lock);
    bool matched=shared.ack_pending && shared.ack_generation==generation;
    // Old acknowledgements must not remain for a later generation.
    if(shared.ack_pending)shared.ack_pending=false;
    portEXIT_CRITICAL(&lock);
    return matched;
}
void voice_submit(const int16_t *pcm,size_t samples) {
    if(!frames || samples!=FRAME)return;
    control_t c=control();
    // Recording can continue while the Wi-Fi portal deliberately suspends
    // local recognition. Do not fill an AFE ring that has no active fetcher;
    // the recording path already owns its separate copy of these samples.
    if(c.mode==VOICE_OFF)return;
    frame_t f={.time=esp_timer_get_time(),.generation=c.generation};
    memcpy(f.pcm,pcm,sizeof(f.pcm));
    if(xQueueSend(frames,&f,0)!=pdTRUE) {
        portENTER_CRITICAL(&lock);shared.drops++;shared.generation++;shared.window=false;shared.after=esp_timer_get_time()+800000;portEXIT_CRITICAL(&lock);
        ESP_LOGW(TAG,"INPUT_DROP recognition reset; recording has priority");
    }
}
static void window_set(bool open) {
    portENTER_CRITICAL(&lock);
    shared.window=open;
    portEXIT_CRITICAL(&lock);
}
static void feed_task(void *unused) {
    (void)unused;frame_t frame;
    for(;;) {
        // Never manufacture audio on a scheduler timeout: 32 ms rounds to
        // 30 ms with this 100 Hz RTOS tick, inserting silence between real
        // microphone frames and corrupting speech/timeouts. No input means wait.
        if(xQueueReceive(frames,&frame,portMAX_DELAY)!=pdTRUE)continue;
        control_t c=control();int64_t now=esp_timer_get_time();
        // A mode change can leave a few old frames in our small input queue.
        // Discard them while recognition is off instead of filling the AFE
        // ring that detect_task intentionally is not fetching.
        if(c.mode==VOICE_OFF)continue;
        if(frame.generation!=c.generation || now<c.after || now-frame.time>200000)
            memset(frame.pcm,0,sizeof(frame.pcm));
        afe->feed(afe_data,frame.pcm);
    }
}
// Fetch-task-owned diagnostics: numeric signal evidence only, never transcripts.
static uint32_t command_frames, command_peak, command_speech_frames;
static int command_frame_bytes;
static void command_report(void) {
    esp_mn_results_t *candidate=mn->get_results(mn_data);
    int id=0;float probability=0;
    if(candidate && candidate->num>0 && candidate->num<=5) {
        id=candidate->command_id[0];probability=candidate->prob[0];
    }
    ESP_LOGI(TAG,"COMMAND_INPUT frames=%"PRIu32" bytes=%d peak=%"PRIu32" speech_frames=%"PRIu32" candidate=%d probability=%.3f",
        command_frames,command_frame_bytes,command_peak,command_speech_frames,id,(double)probability);
}
static void apply_action(voice_gate_t *gate,voice_action_t action) {
    memo_cmd_t command;
    switch(action) {
    case VG_START: command=CMD_START;break;
    case VG_STOP: command=CMD_STOP;break;
    case VG_USB_OPEN: command=CMD_USB_OPEN;break;
    case VG_USB_CLOSE: command=CMD_USB_CLOSE;break;
    case VG_USB_MUTE: command=CMD_USB_MUTE;break;
    case VG_USB_UNMUTE: command=CMD_USB_UNMUTE;break;
    case VG_WINDOW:
        command_frames=command_peak=command_speech_frames=0;
        command_frame_bytes=0;
        // Keep WakeNet active because AFE_AGC_MODE_WAKENET derives its gain
        // while that model is running. The gate ignores further wake events
        // during this explicit command window, so this improves quiet speech
        // without allowing an extra wake to stop the recording.
        mn->clean(mn_data);window_set(true);
        app_command(CMD_VOICE_WAKE,gate->generation,"voice");
        ESP_LOGI(TAG,"COMMAND_WINDOW 8000ms mode=%d",gate->mode);return;
    case VG_CANCEL:
        command_report();window_set(false);mn->clean(mn_data);
        afe->enable_wakenet(afe_data);
        app_command(CMD_VOICE_CANCEL,gate->generation,"voice");
        ESP_LOGI(TAG,"COMMAND_CANCEL mode=%d",gate->mode);return;
    case VG_TIMEOUT:
        command_report();
        window_set(false);mn->clean(mn_data);afe->enable_wakenet(afe_data);
        ESP_LOGI(TAG,"COMMAND_TIMEOUT mode=%d",gate->mode);return;
    default:return;
    }
    command_report();
    window_set(false);
    if(!app_command(command,gate->generation,"voice"))gate->pending=false;
}
static void detect_task(void *unused) {
    (void)unused;voice_gate_t gate={.generation=UINT32_MAX};
    for(;;) {
        // Playback/no-card deliberately has no microphone input. Do not poll
        // an empty AFE ring (and spam warnings) while recognition is disabled.
        if(control().mode==VOICE_OFF) {vTaskDelay(pdMS_TO_TICKS(100));continue;}
        afe_fetch_result_t *r=afe->fetch_with_delay(afe_data,pdMS_TO_TICKS(100));
        control_t c=control();int64_t now=esp_timer_get_time();
        if(c.generation!=gate.generation) {
            voice_gate_reset(&gate,c.mode,c.generation);window_set(false);
            mn->clean(mn_data);afe->enable_wakenet(afe_data);
        }
        if(take_command_ack(gate.generation))voice_gate_ack(&gate,gate.generation);
        if(c.mode==VOICE_OFF || now<c.after+400000 || gate.pending)continue;
        voice_action_t action=voice_gate_step(&gate,c.generation,now,false,0,0);
        apply_action(&gate,action);
        if(!r || r->ret_value==ESP_FAIL || action==VG_TIMEOUT)continue;
        if(r->wakeup_state==WAKENET_DETECTED && !gate.deadline) {
            ESP_LOGI(TAG,"WAKE mode=%d generation=%"PRIu32,c.mode,c.generation);
            apply_action(&gate,voice_gate_step(&gate,c.generation,now,true,0,0));
            continue;
        }
        if(gate.deadline) {
            command_frames++;command_frame_bytes=r->data_size;
            if(r->vad_state==VAD_SPEECH)command_speech_frames++;
            for(int i=0;i<r->data_size/(int)sizeof(int16_t);i++) {
                int value=r->data[i];if(value<0)value=-value;
                if((unsigned)value>command_peak)command_peak=(unsigned)value;
            }
            esp_mn_state_t result=mn->detect(mn_data,r->data);
            if(result==ESP_MN_STATE_DETECTED) {
                esp_mn_results_t *best=mn->get_results(mn_data);
                if(best && best->num>0) {
                    ESP_LOGI(TAG,"COMMAND_RESULT id=%d probability=%.3f",best->command_id[0],(double)best->prob[0]);
                    apply_action(&gate,voice_gate_step(&gate,c.generation,esp_timer_get_time(),false,
                                                      best->command_id[0],best->prob[0]));
                }
                mn->clean(mn_data);
            } else if(result==ESP_MN_STATE_TIMEOUT) {
                // Decoder silence detection can finish early. The UI/control window
                // is eight seconds of wall time, so restart only the decoder.
                mn->clean(mn_data);
                ESP_LOGI(TAG,"DECODER_RESTART keeping command window open");
            }
        }
    }
}
bool voice_init(void) {
    models=esp_srmodel_init("model");
    if(!models)return false;
    char *wake=esp_srmodel_filter(models,"wn9","nihaomiaoban");
    char *command=esp_srmodel_filter(models,"mn7","cn");
    if(!wake || !command){ESP_LOGE(TAG,"required local models missing");return false;}
    afe_config_t *config=afe_config_init("M",models,AFE_TYPE_SR,AFE_MODE_LOW_COST);
    if(!config)return false;
    config->aec_init=false;config->ns_init=false;
    config->agc_init=true;config->agc_mode=AFE_AGC_MODE_WAKENET;
    config->wakenet_model_name=wake;config->wakenet_model_name_2=NULL;
    config->memory_alloc_mode=AFE_MEMORY_ALLOC_MORE_PSRAM;
    config->afe_perferred_core=0;config->afe_perferred_priority=4;
    afe=esp_afe_handle_from_config(config);afe_data=afe->create_from_config(config);afe_config_free(config);
    if(!afe_data)return false;
    if(afe->set_wakenet_threshold(afe_data,1,VOICE_WAKE_THRESHOLD)!=1) {
        ESP_LOGE(TAG,"failed to set wake threshold");return false;
    }
    mn=esp_mn_handle_from_name(command);if(!mn)return false;
    mn_data=mn->create(command,8000);if(!mn_data)return false;
    if(afe->get_feed_chunksize(afe_data)!=FRAME || afe->get_feed_channel_num(afe_data)!=1 ||
       afe->get_fetch_chunksize(afe_data)!=mn->get_samp_chunksize(mn_data)) {
        ESP_LOGE(TAG,"unsupported model frame sizes");return false;
    }
    if(esp_mn_commands_alloc(mn,mn_data)!=ESP_OK)return false;
    size_t phrase_count=0;
    const voice_phrase_t *phrases=voice_command_phrases(&phrase_count);
    for(size_t i=0;i<phrase_count;i++)
        if(esp_mn_commands_add(phrases[i].id,phrases[i].pinyin)!=ESP_OK) {
            ESP_LOGE(TAG,"invalid MultiNet phrase id=%d",phrases[i].id);return false;
        }
    if(esp_mn_commands_update()!=NULL)return false;
    mn->set_det_threshold(mn_data,VOICE_COMMAND_THRESHOLD);
    // These are copied PCM frames, accessed only by tasks (never ISR or DMA).
    // Keep internal RAM available for the network/HTTP stacks and DMA buffers.
    frames=xQueueCreateWithCaps(8,sizeof(frame_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!frames)return false;
    TaskHandle_t feeder=NULL;
    if(xTaskCreatePinnedToCore(feed_task,"voice_feed",6144,NULL,4,&feeder,0)!=pdPASS)return false;
    if(xTaskCreatePinnedToCore(detect_task,"voice_detect",8192,NULL,4,NULL,1)!=pdPASS){vTaskDelete(feeder);return false;}
    portENTER_CRITICAL(&lock);shared.ready=true;portEXIT_CRITICAL(&lock);
    ESP_LOGI(TAG,"VOICE_READY wake=%s wake_threshold=%.3f command=%s command_threshold=%.2f phrases=%u window_ms=8000 offline=1",
             wake,(double)VOICE_WAKE_THRESHOLD,command,(double)VOICE_COMMAND_THRESHOLD,(unsigned)phrase_count);
    return true;
}
