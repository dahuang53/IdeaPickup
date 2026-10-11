#include "memo_cardless.h"
#include "memo_cardless_buffer.h"
#include "memo_cardless_state.h"
#include "memo_cardless_record.h"
#include "memo_record_ids.h"
#include "network_sync.h"
extern "C" {
#include "power_policy.h"
}

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cmath>
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_opus_enc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {
constexpr size_t AUDIO_BUFFER_BYTES = 1024 * 1024;
constexpr size_t PCM_BUFFER_BYTES = 64 * 1024;
constexpr size_t MAX_RECORDS = 1024;
constexpr size_t FRAME_SAMPLES = 320;
constexpr size_t CHUNK_FRAMES = 250;
constexpr size_t CHUNK_CAPACITY = CHUNK_FRAMES * 128;
constexpr uint32_t ENCODER_STACK_BYTES = 32768;
const char *TAG = "cardless";
const char *PARTITION = "memo_meta";
const char *NAMESPACE = "records1";

SemaphoreHandle_t meta_mutex;
SemaphoreHandle_t buffer_mutex;
SemaphoreHandle_t task_mutex;
nvs_handle_t metadata;
memo_cardless_record_t *records;
size_t record_count;
uint32_t counter;
char global_error[80];
uint32_t query_notice_id;
char query_notice[80];
std::atomic<bool> initialized{false};
std::atomic<bool> metadata_fault{false}, remote_complete{false}, transfer_blocked{false};
std::atomic<uint32_t> revision{0};
std::atomic<uint32_t> active_id{0};
std::atomic<bool> capturing{false}, finish_requested{false}, encoder_done{true};
std::atomic<bool> begin_in_progress{false};
std::atomic<bool> encode_abort{false}, stop_requested{false}, recovery_mode{false};
std::atomic<uint32_t> input_samples{0}, encoded_samples{0}, encoded_frames{0};
std::atomic<uint32_t> status_request{0}, retry_request{0};
TaskHandle_t encoder_task_handle;
TaskHandle_t network_task_handle;
StreamBufferHandle_t pcm_stream;
StaticStreamBuffer_t pcm_stream_control;
uint8_t *pcm_storage;
uint8_t *packet_storage;
uint8_t *network_chunk;
memo_cardless_buffer_t packets;
void *encoder;
char stop_reason[80];

/* Serial-only test controls. They never enter NVS or target SD IDs. Query
 * injection only permits the latest cardless record; deadlines are <=90s. */
std::atomic<uint32_t> test_http_id{0}, test_http_until_ms{0};
std::atomic<uint32_t> test_drop_put_id{0}, test_drop_finish_id{0};
std::atomic<uint32_t> test_finish_seconds{0}, test_finish_hold_id{0}, test_finish_until_ms{0};
std::atomic<uint32_t> test_begin_fault{0}, test_next_ring_frames{0}, test_ring_id{0};
std::atomic<uint32_t> test_query_id{0}, test_query_kind{0}, test_put_401_id{0};
std::atomic<uint32_t> test_boot_latest_id{0};

class Lock {
    SemaphoreHandle_t mutex;
public:
    explicit Lock(SemaphoreHandle_t m) : mutex(m) { xSemaphoreTake(mutex, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(mutex); }
};

class BeginAttempt {
    bool entered;
public:
    BeginAttempt() : entered(!begin_in_progress.exchange(true)) {}
    ~BeginAttempt() { if (entered) begin_in_progress.store(false); }
    explicit operator bool() const { return entered; }
};

uint32_t test_now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
bool test_before_deadline(uint32_t deadline)
{
    return static_cast<int32_t>(deadline - test_now_ms()) > 0;
}
void test_clear_id(std::atomic<uint32_t> &target, uint32_t id)
{
    target.compare_exchange_strong(id, 0);
}

void copy_text(char *to, size_t cap, const char *from)
{
    if (to && cap) snprintf(to, cap, "%s", from ? from : "");
}
const char *friendly_error(const char *error)
{
    if (!error || !error[0]) return "";
    struct Message { const char *code; const char *text; };
    static const Message messages[] = {
        {"metadata_write_failed", "记录状态写入失败"},
        {"metadata_partition_unavailable", "记录存储不可用"},
        {"metadata_unavailable", "记录存储不可用"},
        {"metadata_corrupt", "记录存储校验失败"},
        {"metadata_full", "记录存储已满"},
        {"out_of_memory", "录音内存不足"},
        {"opus_init_failed", "音频编码初始化失败"},
        {"opus_encode_failed", "音频编码失败，录音已中断"},
        {"network_buffer_full", "网络中断过久，录音已停止"},
        {"pcm_buffer_full", "编码未跟上，录音已停止"},
        {"network_unavailable", "无卡录音需要联网"},
        {"opus_unavailable", "服务器暂不支持无卡录音"},
        {"stream_create_rejected", "服务器拒绝接收录音"},
        {"stream_protocol_error", "上传校验失败，录音已中断"},
        {"audio_unavailable", "音频已不可用，无法重试"},
        {"audio_not_uploaded", "音频未上传，无法恢复"},
        {"audio_expired", "临时音频已清理，无法重试"},
        {"power_loss_pending", "断电中断，正在确认已传内容"},
        {"power_loss_partial", "断电中断，仅处理已传部分"},
        {"power_loss_audio_lost", "断电前未上传，音频已丢失"},
        {"empty_recording", "录音过短，没有可用音频"},
        {"recording_interrupted", "录音意外中断"},
        {"credentials_invalid", "凭据失效，配对后重试"},
        {"server_unsupported", "服务器未支持无卡，请稍后重试"},
        {"server_audio_lost", "服务器音频缺失，无法完整恢复"},
        {"partial_sent", "录音有中断，已同步有效部分"},
        {"query_failed", "状态查询失败，请稍后重试"},
        {"query_credentials_invalid", "查询凭据失效，请重新配对"},
        {"transcription_failed", "转写失败，可重试"},
        {"delivery_failed", "思源投递失败，可重试"},
        {"delivery_uncertain", "思源投递结果待确认"},
        {"transcription_rejected", "转写未通过质量检查"},
    };
    for (const auto &message : messages) if (!strcmp(error, message.code)) return message.text;
    return error;
}
size_t find_record(uint32_t id)
{
    for (size_t i = record_count; i; --i) if (records[i - 1].id == id) return i - 1;
    return SIZE_MAX;
}
void set_error_locked(const char *error)
{
    copy_text(global_error, sizeof(global_error), friendly_error(error));
    revision.fetch_add(1);
}
bool persist_locked(const memo_cardless_record_t &record)
{
    uint8_t bytes[MEMO_CARDLESS_RECORD_SIZE];
    if(!memo_cardless_record_encode(&record,bytes)){metadata_fault.store(true);set_error_locked("metadata_corrupt");return false;}
    char key[16];
    snprintf(key, sizeof(key), "r%08lx", (unsigned long)record.id);
    const int64_t started_us = esp_timer_get_time();
    esp_err_t result = nvs_set_blob(metadata, key, bytes, sizeof(bytes));
    if (result == ESP_OK) result = nvs_commit(metadata);
    if(result==ESP_OK){
        nvs_handle_t reader;result=nvs_open_from_partition(PARTITION,NAMESPACE,NVS_READONLY,&reader);
        if(result==ESP_OK){uint8_t actual[MEMO_CARDLESS_RECORD_SIZE];size_t n=sizeof(actual);result=nvs_get_blob(reader,key,actual,&n);nvs_close(reader);
            if(result==ESP_OK&&(n!=sizeof(bytes)||memcmp(actual,bytes,n)))result=ESP_FAIL;}
    }
    const int64_t elapsed_us = esp_timer_get_time() - started_us;
    if (elapsed_us > 10000) {
        ESP_LOGW(TAG, "METADATA_WRITE id=%lu state=%u elapsed_us=%lld result=%s",
                 (unsigned long)record.id, (unsigned)record.state, (long long)elapsed_us,
                 esp_err_to_name(result));
    }
    if (result != ESP_OK) {
        metadata_fault.store(true);
        set_error_locked("metadata_write_failed");
        ESP_LOGE(TAG, "Metadata write failed: %s", esp_err_to_name(result));
        return false;
    }
    return true;
}
void update_record(uint32_t id, memo_job_state_t state, const char *error,
                   bool can_retry, bool interrupted, uint64_t received = UINT64_MAX,
                   bool force_persist = false, bool authoritative_interruption = false)
{
    error = friendly_error(error);
    Lock lock(meta_mutex);
    size_t at = find_record(id);
    if (at == SIZE_MAX) return;
    auto &r = records[at];
    const bool next_interrupted = memo_cardless_interruption(r.interrupted, interrupted, authoritative_interruption);
    bool changed = r.state != state || r.can_retry != can_retry ||
                   next_interrupted != r.interrupted || strcmp(r.error, error ? error : "");
    r.state = state;
    r.can_retry = can_retry;
    r.interrupted = next_interrupted;
    copy_text(r.error, sizeof(r.error), error);
    if (received != UINT64_MAX) r.received_samples = received;
    if (id == active_id.load() && !recovery_mode.load() && state == MEMO_JOB_UPLOADING) {
        r.duration_ms = (encoder_done.load() ? encoded_samples.load() : input_samples.load()) / 16;
        /* Encoded bytes are accumulated by the encoder, independent of ACKs. */
    } else if (received != UINT64_MAX && (recovery_mode.load() || !r.duration_ms)) {
        r.duration_ms = received / 16;
        if (recovery_mode.load()) r.encoded_bytes = ((received + FRAME_SAMPLES - 1) / FRAME_SAMPLES) * 60;
    }
    if (changed || force_persist) {
        persist_locked(r);
        revision.fetch_add(1);
    }
}
void set_stop(const char *reason, bool abort_encoding)
{
    reason = friendly_error(reason);
    {
        Lock lock(meta_mutex);
        if (!stop_requested.load()) copy_text(stop_reason, sizeof(stop_reason), reason);
        set_error_locked(reason);
        size_t at = find_record(active_id.load());
        if (at != SIZE_MAX) {
            records[at].interrupted = true;
            copy_text(records[at].error, sizeof(records[at].error), reason);
        }
    }
    capturing.store(false);
    stop_requested.store(true);
    if (abort_encoding) encode_abort.store(true);
    finish_requested.store(true);
}
void pause_transfer(uint32_t id, const char *reason)
{
    set_stop(reason, false);
    transfer_blocked.store(true);
    update_record(id, MEMO_JOB_FAILED, reason, true, true, UINT64_MAX, true);
}
bool recovery_power_ready()
{
    return power_policy_recovery_ready();
}
void record_encoding_done(uint32_t id)
{
    {
        Lock lock(meta_mutex);
        size_t at = find_record(id);
        if (at != SIZE_MAX) {
            records[at].duration_ms = encoded_samples.load() / 16;
            records[at].interrupted |= stop_requested.load();
            if (stop_requested.load()) copy_text(records[at].error, sizeof(records[at].error), stop_reason);
            persist_locked(records[at]);
            revision.fetch_add(1);
        }
    }
}

void encode_task(void *)
{
    alignas(16) int16_t pcm[FRAME_SAMPLES];
    alignas(16) uint8_t output[1536];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const uint32_t id = active_id.load();
        if (!id || recovery_mode.load() || encoder_done.load() || !encoder || !pcm_stream) continue;
        uint64_t encode_total_us = 0;
        int64_t encode_peak_us = 0;
        while (id == active_id.load() && !encode_abort.load()) {
            size_t available = xStreamBufferBytesAvailable(pcm_stream);
            if (available < sizeof(pcm) && !finish_requested.load()) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            if (!available && finish_requested.load()) break;
            size_t bytes = std::min(available, sizeof(pcm));
            size_t got = xStreamBufferReceive(pcm_stream, pcm, bytes, 0);
            if (!got) continue;
            memset(reinterpret_cast<uint8_t *>(pcm) + got, 0, sizeof(pcm) - got);
            esp_audio_enc_in_frame_t in{};
            in.buffer = reinterpret_cast<uint8_t *>(pcm);
            in.len = sizeof(pcm);
            esp_audio_enc_out_frame_t out{};
            out.buffer = output;
            out.len = sizeof(output);
            int64_t started = esp_timer_get_time();
            esp_audio_err_t result = esp_opus_enc_process(encoder, &in, &out);
            int64_t elapsed = esp_timer_get_time() - started;
            if (result != ESP_AUDIO_ERR_OK || out.encoded_bytes != 60) {
                set_stop("opus_encode_failed", true);
                break;
            }
            bool retained;
            {
                Lock lock(buffer_mutex);
                retained = memo_cardless_buffer_push(&packets, output, out.encoded_bytes);
            }
            if (!retained) {
                set_stop("network_buffer_full", true);
                break;
            }
            encoded_samples.fetch_add(got / sizeof(int16_t));
            uint32_t frames = encoded_frames.fetch_add(1) + 1;
            encode_total_us += static_cast<uint64_t>(elapsed);
            encode_peak_us = std::max(encode_peak_us, elapsed);
            {
                Lock lock(meta_mutex);
                size_t at = find_record(id);
                if (at != SIZE_MAX) records[at].encoded_bytes += out.encoded_bytes;
            }
            if (frames == 1 || frames % 3000 == 0) {
                constexpr uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
                // IDF's Xtensa StackType_t is uint8_t: the high-water mark is already bytes.
                const size_t stack_min_bytes = uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t);
                ESP_LOGI(TAG, "OPUS id=%lu frames=%lu last_us=%lld avg_us=%llu peak_us=%lld "
                         "stack_min_bytes=%zu internal_free_bytes=%zu internal_largest_bytes=%zu "
                         "internal_min_bytes=%zu psram_free_bytes=%zu", (unsigned long)id,
                         (unsigned long)frames, (long long)elapsed,
                         (unsigned long long)(encode_total_us / frames), (long long)encode_peak_us,
                         stack_min_bytes, heap_caps_get_free_size(internal_caps),
                         heap_caps_get_largest_free_block(internal_caps),
                         heap_caps_get_minimum_free_size(internal_caps),
                         heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            }
            if (frames % CHUNK_FRAMES == 0 && network_task_handle) xTaskNotifyGive(network_task_handle);
        }
        if (encoder) {
            esp_opus_enc_close(encoder);
            encoder = nullptr;
        }
        record_encoding_done(id);
        /* Publish availability only after close and metadata persistence. Once
         * done is visible, the network worker may release active_id and begin()
         * may prepare another recording. Do not touch per-record state below.
         * A next begin notification arriving before the wait remains pending. */
        encoder_done.store(true);
        if (network_task_handle) xTaskNotifyGive(network_task_handle);
    }
}

struct Reply {
    bool valid = false;
    uint32_t id = 0, next_seq = 0, duration_ms = 0;
    uint64_t samples = 0;
    char status[32]{};
    char error[80]{};
    bool can_retry = false, interrupted = false;
};
Reply parse_reply(const char *body, uint32_t expected_id)
{
    Reply r;
    cJSON *json = cJSON_Parse(body);
    if (!json) return r;
    bool valid_numbers=true;
    auto number = [json,&valid_numbers](const char *key) -> double {
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(json, key);
        const double maximum=!strcmp(key,"received_samples")?9007199254740991.0:4294967295.0;
        if(!cJSON_IsNumber(p)||!std::isfinite(p->valuedouble)||p->valuedouble<0||p->valuedouble>maximum||std::floor(p->valuedouble)!=p->valuedouble){valid_numbers=false;return 0;}
        return p->valuedouble;
    };
    auto string = [json](const char *key) -> const char * {
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(json, key);
        return cJSON_IsString(p) ? p->valuestring : "";
    };
    r.id = number("recording_id");
    r.next_seq = number("next_seq");
    r.duration_ms = number("duration_ms");
    r.samples = number("received_samples");
    copy_text(r.status, sizeof(r.status), string("status"));
    copy_text(r.error, sizeof(r.error), string("error"));
    r.can_retry = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "can_retry"));
    r.interrupted = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "interrupted"));
    memo_cardless_record_t saved{};
    r.valid = valid_numbers && r.id == expected_id && r.status[0] && memo_cardless_get(expected_id,&saved) && !strcmp(string("device_id"),saved.target.device_id);
    cJSON_Delete(json);
    return r;
}
memo_job_state_t remote_state(const char *status)
{
    if (!strcmp(status, "receiving")) return MEMO_JOB_UPLOADING;
    if (!strcmp(status, "received")) return MEMO_JOB_RECEIVED;
    if (!strcmp(status, "transcribing")) return MEMO_JOB_TRANSCRIBING;
    if (!strcmp(status, "transcribed") || !strcmp(status, "sending")) return MEMO_JOB_DELIVERING;
    if (!strcmp(status, "sent")) return MEMO_JOB_SENT;
    if (!strcmp(status, "uncertain")) return MEMO_JOB_UNCERTAIN;
    if (!strcmp(status, "transcription_rejected")) return MEMO_JOB_REJECTED;
    if (!strcmp(status, "transcription_failed") || !strcmp(status, "delivery_failed") ||
        !strcmp(status, "audio_expired")) return MEMO_JOB_FAILED;
    return MEMO_JOB_UNKNOWN;
}
void selected_notice(uint32_t id, const char *message)
{
    Lock lock(meta_mutex);
    query_notice_id = id;
    copy_text(query_notice, sizeof(query_notice), friendly_error(message));
    set_error_locked(message);
}
void clear_selected_notice(uint32_t id)
{
    Lock lock(meta_mutex);
    if (query_notice_id == id) {
        query_notice_id = 0;
        query_notice[0] = 0;
        revision.fetch_add(1);
    }
}
void accept_reply(uint32_t id, const Reply &reply, bool force = false)
{
    if (!reply.valid) return;
    memo_job_state_t state = remote_state(reply.status);
    if (state == MEMO_JOB_UNKNOWN) return;
    clear_selected_notice(id);
    const char *error = reply.error;
    /* An interrupted recording remains visibly partial after successful delivery. */
    memo_cardless_record_t previous{};
    if (!error[0] && reply.interrupted && memo_cardless_get(id, &previous) && previous.interrupted)
        error = state == MEMO_JOB_SENT ? "partial_sent" : previous.error;
    if (reply.duration_ms) {
        Lock lock(meta_mutex);
        size_t at = find_record(id);
        if (at != SIZE_MAX) records[at].duration_ms = reply.duration_ms;
    }
    update_record(id, state, error, reply.can_retry, reply.interrupted, reply.samples, force,
                  state != MEMO_JOB_UPLOADING);
}
bool active_marker_matches(uint32_t expected){
 nvs_handle_t h;if(nvs_open_from_partition(PARTITION,NAMESPACE,NVS_READONLY,&h)!=ESP_OK)return false;
 uint32_t found=0;esp_err_t e=nvs_get_u32(h,"active",&found);nvs_close(h);
 return expected?e==ESP_OK&&found==expected:e==ESP_ERR_NVS_NOT_FOUND;
}
bool clear_active_marker()
{
    esp_err_t result=nvs_erase_key(metadata,"active");
    if(result==ESP_ERR_NVS_NOT_FOUND)result=ESP_OK;
    if(result==ESP_OK)result=nvs_commit(metadata);
    if(result==ESP_OK&&!active_marker_matches(0))result=ESP_FAIL;
    if(result!=ESP_OK){metadata_fault.store(true);set_error_locked("metadata_write_failed");return false;}
    return true;
}
bool mark_remote_audio(uint32_t id)
{
    Lock lock(meta_mutex);size_t at=find_record(id);
    if(at==SIZE_MAX||metadata_fault.load())return false;
    auto &r=records[at];
    if(r.may_have_remote_audio)return true;
    r.may_have_remote_audio=true;
    return persist_locked(r); // no audio PUT until this possibility is durable
}
void release_active(uint32_t id)
{
    if (active_id.load() != id || !encoder_done.load()) return;
    /* A confirmed relay result does not by itself make a new recording safe:
     * keep the slot until its state and removal of the recovery marker are durable. */
    {
        Lock lock(meta_mutex);
        size_t at = find_record(id);
        if (at == SIZE_MAX || !persist_locked(records[at])) {
            remote_complete.store(true);
            return;
        }
        if(!clear_active_marker()){
            remote_complete.store(true);
            return;
        }
    }
    capturing.store(false);
    recovery_mode.store(false);
    test_clear_id(test_http_id, id);
    test_clear_id(test_drop_put_id, id);
    test_clear_id(test_drop_finish_id, id);
    test_clear_id(test_finish_hold_id, id);
    test_clear_id(test_put_401_id, id);
    uint32_t reduced_id = id;
    if (test_ring_id.compare_exchange_strong(reduced_id, 0)) {
        Lock lock(buffer_mutex);
        memo_cardless_buffer_init(&packets, packet_storage, AUDIO_BUFFER_BYTES);
        ESP_LOGW(TAG, "INJECT kind=6 action=RESTORE_RING id=%lu capacity=%u",
                 (unsigned long)id, (unsigned)AUDIO_BUFFER_BYTES);
    }
    active_id.store(0);
    revision.fetch_add(1);
    ESP_LOGI(TAG, "STREAM_RELEASED id=%lu", (unsigned long)id);
}

esp_err_t restore_active_record()
{
    uint32_t id=0;esp_err_t result=nvs_get_u32(metadata,"active",&id);
    if(result==ESP_ERR_NVS_NOT_FOUND)return ESP_OK;
    if(result!=ESP_OK||find_record(id)==SIZE_MAX){copy_text(global_error,sizeof(global_error),friendly_error("metadata_corrupt"));return ESP_ERR_INVALID_STATE;}
    // A failed begin (or power loss before the first PUT) has no remote audio.
    // PSRAM is empty at boot; release this marker without needing its old target.
    if(!records[find_record(id)].may_have_remote_audio){
        update_record(id,MEMO_JOB_FAILED,"audio_not_uploaded",false,true,0,true);
        if(metadata_fault.load())return ESP_FAIL;
        Lock lock(meta_mutex);return clear_active_marker()?ESP_OK:ESP_FAIL;
    }
    active_id.store(id);recovery_mode.store(true);
    update_record(id,MEMO_JOB_CHECKING,"power_loss_pending",false,false,UINT64_MAX,true);
    return metadata_fault.load()?ESP_FAIL:ESP_OK;
}

void make_path(char *path, size_t cap, uint32_t id, const char *suffix = "")
{
    memo_network_status_t network{};
    network_sync_get_status(&network);
    snprintf(path, cap, "/v1/streams/%s/%lu%s", network.device_id, (unsigned long)id, suffix);
}
int request(const char *method, const char *path, uint32_t id, const void *body, size_t size,
            const char *type, const char *hash, Reply *reply)
{
    if (reply) *reply = Reply{};
    memo_cardless_record_t saved{};memo_sync_target_t current{};
    if(!memo_cardless_get(id,&saved)||!network_sync_current_target(&current))return 0;
    if(!memo_sync_target_matches(&saved.target,&current,false)){selected_notice(id,"当前服务未接收此无卡录音");return 412;}
    const bool finish = !strcmp(method, "POST") && strlen(path) >= 7 &&
                        !strcmp(path + strlen(path) - 7, "/finish");
    if (id == active_id.load()) {
        const bool blocked = test_http_id.load() == id && test_before_deadline(test_http_until_ms.load());
        const bool finish_held = test_finish_hold_id.load() == id &&
                                 (!strcmp(method, "GET") || finish) &&
                                 test_before_deadline(test_finish_until_ms.load());
        if (blocked || finish_held) {
            ESP_LOGW(TAG, "INJECT kind=%u action=HTTP_BLOCK id=%lu method=%s path=%s",
                     finish_held ? 3U : 1U, (unsigned long)id, method, path);
            return 0;
        }
    }
    if (!strcmp(method, "GET") && test_query_id.load() == id) {
        Lock lock(meta_mutex);
        uint32_t target = id;
        // Recheck at consumption: a newly added record makes this target old.
        if (!active_id.load() && !begin_in_progress.load() && record_count &&
            test_boot_latest_id.load() == id && records[record_count - 1].id == id &&
            test_query_id.compare_exchange_strong(target, 0)) {
            const unsigned kind = test_query_kind.load();
            const int injected_status = kind == 7 ? 401 : kind == 8 ? 409 : 410;
            ESP_LOGW(TAG, "INJECT kind=%u action=HTTP_RESPONSE_ONLY id=%lu method=GET status=%d actual_network_request=0",
                     kind, (unsigned long)id, injected_status);
            return injected_status;
        }
    }
    uint32_t blocked_put_id = id;
    if (id == active_id.load() && !recovery_mode.load() && !strcmp(method, "PUT") &&
        test_put_401_id.compare_exchange_strong(blocked_put_id, 0)) {
        ESP_LOGW(TAG, "INJECT kind=10 action=HTTP_RESPONSE_ONLY id=%lu method=PUT status=401 actual_network_request=0",
                 (unsigned long)id);
        return 401;
    }
    char response[1536]{};
    const int64_t started_us = esp_timer_get_time();
    int status = network_sync_stream_request(method, path, id, body, size, type, hash,
                                            response, sizeof(response));
    bool drop_response = false;
    uint32_t target = id;
    if (id == active_id.load() && !strcmp(method, "PUT") && (status == 200 || status == 201) &&
        test_drop_put_id.compare_exchange_strong(target, 0)) {
        drop_response = true;
        ESP_LOGW(TAG, "INJECT kind=2 action=DROP_PUT_ACK id=%lu actual_http=%d",
                 (unsigned long)id, status);
    }
    target = id;
    if (id == active_id.load() && finish && (status == 200 || status == 202) &&
        test_drop_finish_id.compare_exchange_strong(target, 0)) {
        const uint32_t seconds = test_finish_seconds.load();
        test_finish_until_ms.store(test_now_ms() + seconds * 1000);
        test_finish_hold_id.store(id);
        drop_response = true;
        ESP_LOGW(TAG, "INJECT kind=3 action=DROP_FINISH_ACK id=%lu actual_http=%d hold_seconds=%lu",
                 (unsigned long)id, status, (unsigned long)seconds);
    }
    if (reply && !drop_response) {
        *reply = parse_reply(response, id);
        if (reply->valid && remote_state(reply->status) == MEMO_JOB_UNKNOWN) reply->valid = false;
    }
    ESP_LOGI(TAG, "STREAM_HTTP method=%s id=%lu path=%s status=%d elapsed_ms=%lld",
             method, (unsigned long)id, path, status,
             (long long)((esp_timer_get_time() - started_us) / 1000));
    return drop_response ? 0 : status;
}
bool online()
{
    memo_network_status_t network{};
    network_sync_get_status(&network);
    return network.initialized && network.configured && network.connected && !network.provisioning;
}
void sha_hex(const void *data, size_t size, char result[65])
{
    uint8_t digest[32];
    mbedtls_sha256(static_cast<const unsigned char *>(data), size, digest, 0);
    for (size_t i = 0; i < sizeof(digest); ++i) snprintf(result + i * 2, 3, "%02x", digest[i]);
}
int finish_remote(uint32_t id, uint32_t chunks, uint64_t samples, bool interrupted,
                  const char *reason, Reply *reply)
{
    char path[160];
    make_path(path, sizeof(path), id, "/finish");
    cJSON *json = cJSON_CreateObject();
    if (!json) return 0;
    cJSON_AddNumberToObject(json, "chunks", chunks);
    cJSON_AddNumberToObject(json, "samples", samples);
    cJSON_AddBoolToObject(json, "interrupted", interrupted);
    cJSON_AddStringToObject(json, "reason", reason ? reason : "");
    char *body = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!body) return 0;
    int status = request("POST", path, id, body, strlen(body), "application/json", nullptr, reply);
    cJSON_free(body);
    return status;
}
void query_selected(uint32_t id, bool retry)
{
    if (!id || (id & MEMO_CARDLESS_ID_FLAG) == 0) return;
    memo_cardless_record_t previous{};
    if (!memo_cardless_get(id, &previous)) return;
    if(!previous.may_have_remote_audio){
        update_record(id,MEMO_JOB_FAILED,"audio_not_uploaded",false,true,0,true);
        return;
    }
    if (!online()) {
        selected_notice(id, "network_unavailable");
        return;
    }
    char path[160];
    make_path(path, sizeof(path), id, retry ? "/retry" : "");
    Reply reply;
    int status = request(retry ? "POST" : "GET", path, id, retry ? "{}" : nullptr,
                         retry ? 2 : 0, "application/json", nullptr, &reply);
    memo_cardless_query_action_t action = memo_cardless_query_action(status, reply.valid, previous.state);
    if (action == MEMO_CARDLESS_QUERY_REFRESH) {
        /* A rejected retry may mean audio expired, delivery is uncertain, or
         * another request already succeeded. Reconcile once with authority. */
        make_path(path, sizeof(path), id);
        status = request("GET", path, id, nullptr, 0, nullptr, nullptr, &reply);
        action = memo_cardless_query_action(status, reply.valid, previous.state);
    }
    switch (action) {
    case MEMO_CARDLESS_QUERY_APPLY:
        accept_reply(id, reply);
        break;
    case MEMO_CARDLESS_QUERY_EXPIRED:
        clear_selected_notice(id);
        update_record(id, MEMO_JOB_FAILED, "audio_expired", false, false);
        break;
    case MEMO_CARDLESS_QUERY_MISSING:
        clear_selected_notice(id);
        update_record(id, MEMO_JOB_FAILED, "audio_unavailable", false, false);
        break;
    case MEMO_CARDLESS_QUERY_AUTH_NOTICE:
        /* A temporary presentation-only notice must not turn a known SENT
         * record into FAILED or replace its persistent processing result. */
        selected_notice(id, "query_credentials_invalid");
        break;
    case MEMO_CARDLESS_QUERY_REFRESH: // A second 409 is not recursively retried.
    case MEMO_CARDLESS_QUERY_WAIT:
        selected_notice(id, "query_failed");
        break;
    }
}

void transfer_task(void *)
{
    uint8_t *chunk = network_chunk;
    uint32_t working_id = 0, seq = 0, ack_frames = 0;
    bool created = false, finishing_uncertain = false;
    uint32_t backoff_ms = 1000;
    int64_t retry_at = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
        uint32_t id = active_id.load();
        if (!id) {
            working_id = 0;
            uint32_t requested = retry_request.exchange(0);
            if (requested) query_selected(requested, true);
            requested = status_request.exchange(0);
            if (requested) query_selected(requested, false);
            continue;
        }
        if (working_id != id) {
            working_id = id;
            seq = ack_frames = 0;
            created = finishing_uncertain = false;
            backoff_ms = 1000;
            retry_at = 0;
        }
        if (metadata_fault.load() && capturing.load()) set_stop("metadata_write_failed", false);
        if (remote_complete.load() || transfer_blocked.load() || esp_timer_get_time() < retry_at || !online()) continue;
        auto defer = [&]() {
            retry_at = esp_timer_get_time() + (int64_t)backoff_ms * 1000;
            backoff_ms = std::min(backoff_ms * 2, UINT32_C(30000));
        };
        auto reset_backoff = [&]() { backoff_ms = 1000; retry_at = 0; };
        char path[160];
        make_path(path, sizeof(path), id);
        Reply reply;
        if (recovery_mode.load()) {
            if (!recovery_power_ready()) continue;
            const bool power_loss = !stop_requested.load();
            int status = request("GET", path, id, nullptr, 0, nullptr, nullptr, &reply);
            if (status == 404 || status == 410) {
                update_record(id, MEMO_JOB_FAILED, status == 404 ? (power_loss ? "power_loss_audio_lost" : "server_audio_lost") : "audio_expired",
                              false, true, 0, true);
                release_active(id);
            } else if (status == 200 && reply.valid) {
                if (remote_state(reply.status) != MEMO_JOB_UNKNOWN && strcmp(reply.status, "receiving")) {
                    accept_reply(id, reply, true);
                    release_active(id);
                } else if (reply.samples && reply.next_seq) {
                    int finished = finish_remote(id, reply.next_seq, reply.samples, true, power_loss ? "power_loss" : stop_reason, &reply);
                    if ((finished == 200 || finished == 202) && reply.valid && strcmp(reply.status, "receiving")) {
                        update_record(id, remote_state(reply.status), power_loss ? "power_loss_partial" : stop_reason, reply.can_retry,
                                      true, reply.samples, true);
                        release_active(id);
                    } else defer();
                } else {
                    update_record(id, MEMO_JOB_FAILED, power_loss ? "power_loss_audio_lost" : "server_audio_lost", false, true, 0, true);
                    release_active(id);
                }
            } else if (status == 401 || status == 403) pause_transfer(id, "credentials_invalid");
            else defer();
            continue;
        }
        if (!created) {
            const char *config = "{\"format\":\"opus\",\"sample_rate\":16000,\"channels\":1,\"frame_ms\":20,\"bitrate\":24000}";
            int status = request("POST", "/v1/streams", id, config, strlen(config), "application/json", nullptr, &reply);
            if ((status == 200 || status == 201) && reply.valid && !strcmp(reply.status, "receiving") && reply.next_seq == 0) {
                created = true;
                update_record(id, MEMO_JOB_UPLOADING, stop_requested.load() ? stop_reason : "", false,
                              stop_requested.load());
                reset_backoff();
            } else if (status == 401 || status == 403) pause_transfer(id, "credentials_invalid");
            else if (status == 503 || status == 404) pause_transfer(id, status == 503 ? "opus_unavailable" : "server_unsupported");
            else if (status == 400 || status == 409 || status == 413 || status == 415) pause_transfer(id, "stream_create_rejected");
            else defer();
            continue;
        }
        if (finishing_uncertain) {
            int status = request("GET", path, id, nullptr, 0, nullptr, nullptr, &reply);
            if (status == 200 && reply.valid) {
                if (remote_state(reply.status) != MEMO_JOB_UNKNOWN && strcmp(reply.status, "receiving")) {
                    accept_reply(id, reply, true);
                    release_active(id);
                    continue;
                }
                if (reply.next_seq == seq) finishing_uncertain = false;
                else {
                    set_stop("stream_protocol_error", true);
                    recovery_mode.store(true);
                    finishing_uncertain = false;
                    continue;
                }
            } else if (status == 410 || status == 404) {
                update_record(id, MEMO_JOB_FAILED, status == 410 ? "audio_expired" : "server_audio_lost", false, true, UINT64_MAX, true);
                release_active(id);
                continue;
            } else if (status == 401 || status == 403) {
                pause_transfer(id, "credentials_invalid");
                continue;
            }
            if (finishing_uncertain) { defer(); continue; }
        }
        size_t count = 0, length = 0, pending = 0;
        {
            Lock lock(buffer_mutex);
            pending = packets.packets;
            if (pending >= CHUNK_FRAMES || encoder_done.load()) {
                length = memo_cardless_buffer_peek(&packets, chunk, CHUNK_CAPACITY, CHUNK_FRAMES, &count);
            }
        }
        if (length) {
            if(!mark_remote_audio(id)){pause_transfer(id,"metadata_write_failed");continue;}
            char hash[65];
            sha_hex(chunk, length, hash);
            char suffix[48];
            snprintf(suffix, sizeof(suffix), "/chunks/%lu", (unsigned long)seq);
            make_path(path, sizeof(path), id, suffix);
            int status = request("PUT", path, id, chunk, length, "application/octet-stream", hash, &reply);
            if ((status == 200 || status == 201) && reply.valid && !strcmp(reply.status, "receiving") &&
                reply.next_seq == seq + 1 && reply.samples == ((uint64_t)ack_frames + count) * FRAME_SAMPLES) {
                {
                    Lock lock(buffer_mutex);
                    memo_cardless_buffer_ack(&packets, length, count);
                }
                ++seq;
                ack_frames += count;
                update_record(id, MEMO_JOB_UPLOADING, stop_requested.load() ? stop_reason : "", false,
                              stop_requested.load(), reply.samples);
                reset_backoff();
                ESP_LOGI(TAG, "STREAM_ACK id=%lu seq=%lu samples=%llu", (unsigned long)id,
                         (unsigned long)seq, (unsigned long long)reply.samples);
            } else if (status == 401 || status == 403) {
                pause_transfer(id, "credentials_invalid");
            } else if (status == 404 && !ack_frames) {
                created = false; // No released audio: same ID and retained first chunk can be recreated.
                defer();
            } else if (status == 404) {
                set_stop("server_audio_lost", true);
                if (encoder_done.load()) {
                    update_record(id, MEMO_JOB_FAILED, "server_audio_lost", false, true, UINT64_MAX, true);
                    release_active(id);
                } else defer();
            } else if (status == 410) {
                set_stop("audio_expired", true);
                if (encoder_done.load()) {
                    update_record(id, MEMO_JOB_FAILED, "audio_expired", false, true, reply.samples, true);
                    release_active(id);
                } else defer();
            } else if (status == 409 || status == 400 || status == 413 || status == 415) {
                /* A permanent protocol rejection cannot silently drop a chunk.
                 * Stop capture and query/finalize only the confirmed prefix. */
                set_stop("stream_protocol_error", true);
                if (encoder_done.load()) recovery_mode.store(true);
                defer();
            } else defer();
            continue;
        }
        if (!encoder_done.load() || pending) continue;
        uint64_t samples = std::min((uint64_t)encoded_samples.load(), (uint64_t)ack_frames * FRAME_SAMPLES);
        if (!samples) {
            update_record(id, MEMO_JOB_FAILED, "empty_recording", false, stop_requested.load(), 0, true);
            release_active(id);
            continue;
        }
        int status = finish_remote(id, seq, samples, stop_requested.load(), stop_requested.load() ? stop_reason : "", &reply);
        if ((status == 200 || status == 202) && reply.valid && strcmp(reply.status, "receiving")) {
            accept_reply(id, reply, true);
            release_active(id);
        } else if (status == 401 || status == 403) {
            pause_transfer(id, "credentials_invalid");
        } else if (status == 410 || status == 404) {
            update_record(id, MEMO_JOB_FAILED, status == 410 ? "audio_expired" : "server_audio_lost", false, true, UINT64_MAX, true);
            release_active(id);
        } else if (status == 400) {
            set_stop("stream_protocol_error", true);
            recovery_mode.store(true);
            defer();
        } else {
            finishing_uncertain = true;
            update_record(id, MEMO_JOB_WAITING, stop_requested.load() ? stop_reason : "", false,
                          stop_requested.load(), samples);
            defer();
        }
    }
}
esp_err_t ensure_network_task()
{
    Lock task_lock(task_mutex);
    if (network_task_handle) return ESP_OK;
    if (!network_chunk) network_chunk = static_cast<uint8_t *>(heap_caps_malloc(CHUNK_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!network_chunk) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(transfer_task, "memo_stream", 10240, nullptr, 2, &network_task_handle, 0) != pdPASS) {
        network_task_handle = nullptr;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
} // namespace

extern "C" esp_err_t memo_cardless_init(void)
{
    if (initialized.load()) return ESP_OK;
    if (meta_mutex) return ESP_ERR_INVALID_STATE;
    meta_mutex = xSemaphoreCreateMutex();
    buffer_mutex = xSemaphoreCreateMutex();
    task_mutex = xSemaphoreCreateMutex();
    if (!meta_mutex || !buffer_mutex || !task_mutex) return ESP_ERR_NO_MEM;
    esp_err_t result = nvs_flash_init_partition(PARTITION);
    if (result != ESP_OK) {
        copy_text(global_error, sizeof(global_error), friendly_error("metadata_partition_unavailable"));
        return result; // Explicitly no nvs_flash_erase_partition fallback.
    }
    result = nvs_open_from_partition(PARTITION, NAMESPACE, NVS_READWRITE, &metadata);
    if (result != ESP_OK) return result;
    records = static_cast<memo_cardless_record_t *>(heap_caps_calloc(MAX_RECORDS, sizeof(*records), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!records) return ESP_ERR_NO_MEM;
    counter=0;
    nvs_iterator_t it = nullptr;
    result = nvs_entry_find(PARTITION, NAMESPACE, NVS_TYPE_BLOB, &it);
    while (result == ESP_OK) {
        nvs_entry_info_t info{};
        nvs_entry_info(it, &info);
        if (info.key[0] == 'r') {
            uint8_t bytes[MEMO_CARDLESS_RECORD_SIZE];size_t size=sizeof(bytes);memo_cardless_record_t r{};
            esp_err_t read=nvs_get_blob(metadata,info.key,bytes,&size);
            char key[16];
            if(read!=ESP_OK||!memo_cardless_record_decode(bytes,size,&r)||record_count==MAX_RECORDS){
                nvs_release_iterator(it);copy_text(global_error,sizeof(global_error),friendly_error("metadata_corrupt"));return ESP_ERR_INVALID_STATE;
            }
            snprintf(key,sizeof(key),"r%08lx",(unsigned long)r.id);
            if(strcmp(key,info.key)){nvs_release_iterator(it);return ESP_ERR_INVALID_STATE;}
            records[record_count++]=r;counter=std::max(counter,MEMO_CARDLESS_DISPLAY_ID(r.id));
        }
        result = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    if (result != ESP_ERR_NVS_NOT_FOUND) return result;
    std::sort(records, records + record_count, [](const memo_cardless_record_t &a, const memo_cardless_record_t &b) { return a.id < b.id; });
    result=restore_active_record();
    if(result!=ESP_OK)return result;
    if (metadata_fault.load()) return ESP_FAIL;
    /* Reserve the large contiguous internal stack before audio and Wi-Fi can
     * fragment the heap. This task also commits NVS, so PSRAM is not suitable
     * for its stack. It blocks on notifications while no recording is active. */
    if (xTaskCreatePinnedToCore(encode_task, "memo_opus", ENCODER_STACK_BYTES, nullptr, 3,
                              &encoder_task_handle, 1) != pdPASS) {
        encoder_task_handle = nullptr;
        Lock lock(meta_mutex);
        set_error_locked("out_of_memory");
        return ESP_ERR_NO_MEM;
    }
    initialized.store(true);
    revision.fetch_add(1);
    ESP_LOGI(TAG, "Metadata ready: records=%u recovery=%lu encoder_stack_bytes=%lu",
             (unsigned)record_count, (unsigned long)active_id.load(), (unsigned long)ENCODER_STACK_BYTES);
    return ESP_OK;
}
extern "C" bool memo_cardless_ready(void)
{
    return initialized.load() && !metadata_fault.load() && encoder_task_handle &&
           !begin_in_progress.load() && encoder_done.load() && !active_id.load() && online();
}
extern "C" esp_err_t memo_cardless_observe_id(uint32_t floor)
{
    if (floor & MEMO_CARDLESS_ID_FLAG) return ESP_ERR_INVALID_ARG;
    if (!initialized.load()) return ESP_ERR_INVALID_STATE;
    Lock lock(meta_mutex);
    if (metadata_fault.load()) return ESP_ERR_INVALID_STATE;
    if (floor <= counter) return ESP_OK;

    counter = floor;
    return ESP_OK;
}
extern "C" esp_err_t memo_cardless_start_pending(void)
{
    if (!initialized.load()) return ESP_ERR_INVALID_STATE;
    esp_err_t result = active_id.load() ? ensure_network_task() : ESP_OK;
    if (result != ESP_OK) { Lock lock(meta_mutex); set_error_locked("out_of_memory"); }
    return result;
}
extern "C" esp_err_t memo_cardless_begin(uint32_t floor, uint32_t *out_id)
{
    if (!initialized.load() || metadata_fault.load() || !out_id || !encoder_task_handle)
        return ESP_ERR_INVALID_STATE;
    BeginAttempt attempt;
    if (!attempt) return ESP_ERR_INVALID_STATE;
    if (active_id.load() || !encoder_done.load()) return ESP_ERR_INVALID_STATE;
    if (!online()) {
        Lock lock(meta_mutex);
        set_error_locked("network_unavailable");
        return ESP_ERR_INVALID_STATE;
    }
    if (ensure_network_task() != ESP_OK) {
        Lock lock(meta_mutex); set_error_locked("out_of_memory"); return ESP_ERR_NO_MEM;
    }
    if (!packet_storage) packet_storage = static_cast<uint8_t *>(heap_caps_malloc(AUDIO_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!pcm_storage) pcm_storage = static_cast<uint8_t *>(heap_caps_malloc(PCM_BUFFER_BYTES + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!packet_storage || !pcm_storage) {
        Lock lock(meta_mutex);
        set_error_locked("out_of_memory");
        return ESP_ERR_NO_MEM;
    }
    if (!pcm_stream) pcm_stream = xStreamBufferCreateStatic(PCM_BUFFER_BYTES + 1, 1, pcm_storage, &pcm_stream_control);
    if (!pcm_stream) return ESP_ERR_NO_MEM;
    xStreamBufferReset(pcm_stream);
    {
        Lock lock(buffer_mutex);
        memo_cardless_buffer_init(&packets, packet_storage, AUDIO_BUFFER_BYTES);
    }
    esp_opus_enc_config_t config = ESP_OPUS_ENC_CONFIG_DEFAULT();
    config.sample_rate = 16000;
    config.channel = 1;
    config.bits_per_sample = 16;
    config.bitrate = 24000;
    config.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_20_MS;
    config.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    config.complexity = 0;
    config.enable_fec = false;
    config.enable_dtx = false;
    config.enable_vbr = false;
    if (esp_opus_enc_open(&config, sizeof(config), &encoder) != ESP_AUDIO_ERR_OK) {
        Lock lock(meta_mutex);
        set_error_locked("opus_init_failed");
        return ESP_FAIL;
    }
    int frame_input = 0, frame_output = 0;
    esp_audio_enc_info_t codec_info{};
    if (esp_opus_enc_get_frame_size(encoder, &frame_input, &frame_output) != ESP_AUDIO_ERR_OK ||
        frame_input != static_cast<int>(FRAME_SAMPLES * sizeof(int16_t)) || frame_output <= 0 || frame_output > 1536 ||
        esp_opus_enc_get_info(encoder, &codec_info) != ESP_AUDIO_ERR_OK || codec_info.bitrate != 24000 ||
        codec_info.sample_rate != 16000 || codec_info.channel != 1) {
        esp_opus_enc_close(encoder); encoder = nullptr;
        Lock lock(meta_mutex); set_error_locked("opus_init_failed"); return ESP_FAIL;
    }
    uint32_t id;
    {
        Lock lock(meta_mutex);
        const uint32_t fault = test_begin_fault.exchange(0);
        if (fault == 5) {
            set_error_locked("metadata_full");
            esp_opus_enc_close(encoder);
            encoder = nullptr;
            ESP_LOGW(TAG, "INJECT kind=5 action=REJECT_CAPACITY no_nvs_write=1 no_record_added=1");
            return ESP_ERR_NO_MEM;
        }
        nvs_stats_t stats{};
        esp_err_t result = nvs_get_stats(PARTITION, &stats);
        if (record_count >= MAX_RECORDS || result != ESP_OK || stats.free_entries < 64) {
            set_error_locked("metadata_full");
            esp_opus_enc_close(encoder);
            encoder = nullptr;
            return ESP_ERR_NO_MEM;
        }
        if (fault == 4) {
            metadata_fault.store(true);
            set_error_locked("metadata_write_failed");
            esp_opus_enc_close(encoder);
            encoder = nullptr;
            ESP_LOGW(TAG, "INJECT kind=4 action=REJECT_METADATA_WRITE no_nvs_write=1 no_record_added=1");
            return ESP_FAIL;
        }
        if(!memo_record_ids_reserve(std::max(counter,floor),&id)){
            set_error_locked("metadata_write_failed");esp_opus_enc_close(encoder);encoder=nullptr;return ESP_FAIL;
        }
        id|=MEMO_CARDLESS_ID_FLAG;
        counter = MEMO_CARDLESS_DISPLAY_ID(id);
        memo_cardless_record_t record{};
        record.id = id;
        record.state = MEMO_JOB_QUEUED;
        if(!network_sync_current_target(&record.target)){esp_opus_enc_close(encoder);encoder=nullptr;set_error_locked("network_unavailable");return ESP_FAIL;}
        if (!persist_locked(record) || nvs_set_u32(metadata, "active", id) != ESP_OK || nvs_commit(metadata) != ESP_OK) {
            metadata_fault.store(true);
            set_error_locked("metadata_write_failed");
            esp_opus_enc_close(encoder);
            encoder = nullptr;
            return ESP_FAIL;
        }
        if(!active_marker_matches(id)){metadata_fault.store(true);esp_opus_enc_close(encoder);encoder=nullptr;set_error_locked("metadata_write_failed");return ESP_FAIL;}
        counter = MEMO_CARDLESS_DISPLAY_ID(id);
        records[record_count++] = record;
        global_error[0] = stop_reason[0] = 0;
    }
    const uint32_t test_frames = test_next_ring_frames.exchange(0);
    if (test_frames) {
        Lock lock(buffer_mutex);
        // Fixed CBR packets are 60 bytes plus the two-byte packet length.
        memo_cardless_buffer_init(&packets, packet_storage, static_cast<size_t>(test_frames) * 62);
        test_ring_id.store(id);
        ESP_LOGW(TAG, "INJECT kind=6 action=REDUCE_RING id=%lu frames=%lu capacity=%u",
                 (unsigned long)id, (unsigned long)test_frames, (unsigned)packets.capacity);
    }
    input_samples.store(0);
    encoded_samples.store(0);
    encoded_frames.store(0);
    stop_requested.store(false);
    remote_complete.store(false);
    transfer_blocked.store(false);
    encode_abort.store(false);
    finish_requested.store(false);
    encoder_done.store(false);
    recovery_mode.store(false);
    capturing.store(true);
    active_id.store(id);
    test_boot_latest_id.store(id);
    *out_id = id;
    revision.fetch_add(1);
    xTaskNotifyGive(encoder_task_handle);
    xTaskNotifyGive(network_task_handle);
    ESP_LOGI(TAG, "STREAM_BEGIN id=%lu format=opus rate=16000 bitrate=24000", (unsigned long)id);
    return ESP_OK;
}
extern "C" bool memo_cardless_append(const int16_t *pcm, size_t samples)
{
    if (!capturing.load() || !active_id.load() || !pcm || !samples) return false;
    if (samples > PCM_BUFFER_BYTES / sizeof(int16_t) ||
        xStreamBufferSpacesAvailable(pcm_stream) < samples * sizeof(int16_t)) {
        set_stop("pcm_buffer_full", false);
        return false;
    }
    size_t bytes = samples * sizeof(int16_t);
    if (xStreamBufferSend(pcm_stream, pcm, bytes, 0) != bytes) {
        set_stop("pcm_buffer_full", false);
        return false;
    }
    input_samples.fetch_add(samples);
    return true;
}
extern "C" void memo_cardless_finish(bool interrupted, const char *reason)
{
    if (!active_id.load() || recovery_mode.load()) return;
    if (interrupted) set_stop(reason && reason[0] ? reason : "recording_interrupted", false);
    capturing.store(false);
    finish_requested.store(true);
    if (network_task_handle) xTaskNotifyGive(network_task_handle);
}
extern "C" bool memo_cardless_recording(void) { return capturing.load(); }
extern "C" bool memo_cardless_busy(void)
{
    // A failed optional-module init must not prevent the existing SD workflow.
    return initialized.load() && active_id.load() != 0;
}
extern "C" bool memo_cardless_should_stop(char *error, size_t capacity)
{
    bool stop = stop_requested.load();
    if (stop && error && capacity) {
        Lock lock(meta_mutex);
        copy_text(error, capacity, stop_reason);
    }
    return stop;
}
extern "C" uint32_t memo_cardless_elapsed_ms(void) { return input_samples.load() / 16; }
extern "C" void memo_cardless_error(char *error, size_t capacity)
{
    if (!meta_mutex) { copy_text(error, capacity, friendly_error("metadata_unavailable")); return; }
    Lock lock(meta_mutex);
    copy_text(error, capacity, global_error);
}
extern "C" size_t memo_cardless_count(void)
{
    if (!meta_mutex || !records) return 0;
    Lock lock(meta_mutex);
    return record_count;
}
extern "C" size_t memo_cardless_list(size_t offset, memo_cardless_record_t *out, size_t capacity)
{
    if (!meta_mutex || !records || !out) return 0;
    Lock lock(meta_mutex);
    size_t count = 0;
    while (offset + count < record_count && count < capacity) {
        out[count] = records[record_count - 1 - offset - count];
        ++count;
    }
    return count;
}
extern "C" bool memo_cardless_get(uint32_t id, memo_cardless_record_t *out)
{
    if (!meta_mutex || !records || !out) return false;
    Lock lock(meta_mutex);
    size_t at = find_record(id);
    if (at == SIZE_MAX) return false;
    *out = records[at];
    if (id == query_notice_id) copy_text(out->error, sizeof(out->error), query_notice);
    if (id == active_id.load() && capturing.load()) out->duration_ms = input_samples.load() / 16;
    return true;
}
extern "C" uint32_t memo_cardless_revision(void) { return revision.load(); }
extern "C" bool memo_cardless_request_status(uint32_t id)
{
    memo_cardless_record_t record{};
    if (!initialized.load() || !memo_cardless_get(id, &record)) return false;
    if (ensure_network_task() != ESP_OK) return false;
    status_request.store(id);
    xTaskNotifyGive(network_task_handle);
    return true;
}
extern "C" bool memo_cardless_retry(uint32_t id)
{
    memo_cardless_record_t record{};
    if (!initialized.load() || !memo_cardless_get(id, &record) || !record.can_retry) return false;
    if (id == active_id.load()) {
        if (!transfer_blocked.load() || metadata_fault.load() || !online()) return false;
        transfer_blocked.store(false);
        update_record(id, MEMO_JOB_UPLOADING, record.error, false, true, UINT64_MAX, true);
        xTaskNotifyGive(network_task_handle);
        return true;
    }
    if (ensure_network_task() != ESP_OK) return false;
    uint32_t expected = 0;
    if (!retry_request.compare_exchange_strong(expected, id)) return false;
    xTaskNotifyGive(network_task_handle);
    return true;
}
extern "C" bool memo_cardless_test_fault(unsigned kind, uint32_t value)
{
    if (!initialized.load() || !meta_mutex) return false;
    Lock lock(meta_mutex);
    if (metadata_fault.load()) return false;
    const uint32_t id = active_id.load();
    if ((kind >= 1 && kind <= 3) || kind == 10) {
        if (!(id & MEMO_CARDLESS_ID_FLAG) || recovery_mode.load()) return false;
        if (((kind == 2 || kind == 10) && value != 0) ||
            ((kind == 1 || kind == 3) && (value < 1 || value > 90))) return false;
        switch (kind) {
        case 1:
            test_http_until_ms.store(test_now_ms() + value * 1000);
            test_http_id.store(id);
            break;
        case 2:
            test_drop_put_id.store(id);
            break;
        case 3:
            test_finish_seconds.store(value);
            test_drop_finish_id.store(id);
            break;
        case 10:
            test_put_401_id.store(id);
            break;
        }
        ESP_LOGW(TAG, "INJECT kind=%u action=ARM_ACTIVE id=%lu value=%lu ram_only=1",
                 kind, (unsigned long)id, (unsigned long)value);
        return true;
    }
    if (kind >= 7 && kind <= 9) {
        if (id || begin_in_progress.load() || !encoder_done.load() ||
            !(value & MEMO_CARDLESS_ID_FLAG) || test_boot_latest_id.load() != value ||
            !record_count || records[record_count - 1].id != value)
            return false;
        test_query_kind.store(kind);
        test_query_id.store(value);
        ESP_LOGW(TAG, "INJECT kind=%u action=ARM_LATEST_QUERY id=%lu ram_only=1",
                 kind, (unsigned long)value);
        return true;
    }
    if (kind < 4 || kind > 6 || id || begin_in_progress.load() || !encoder_done.load()) return false;
    if ((kind == 6 && (value < 250 || value > 1500)) || (kind != 6 && value != 0)) return false;
    if (kind == 6) test_next_ring_frames.store(value);
    else test_begin_fault.store(kind);
    ESP_LOGW(TAG, "INJECT kind=%u action=ARM_NEXT_BEGIN value=%lu ram_only=1", kind, (unsigned long)value);
    return true;
}
