#pragma once
#include "memo_cloud.h"
#include "memo_http_transport.h"
#include "esp_websocket_client.h"
#include "freertos/message_buffer.h"
#include "esp_log.h"
#include <atomic>

// Construct/use/destroy on the existing network owner, never an SDK callback.
// No test endpoints, certificate overrides or credentials are stored here.
class MemoCloudEsp {
public:
    explicit MemoCloudEsp(const std::atomic<bool> &cancelled);
    ~MemoCloudEsp();
    const memo_cloud::IO &io() const {return io_;}
    MemoCloudEsp(const MemoCloudEsp &)=delete;
private:
    memo_cloud::IO io_{};
    const std::atomic<bool> &cancelled_;
    esp_http_client_handle_t http_=nullptr;
    MemoHttpTransport *transport_=nullptr;
    esp_websocket_client_handle_t ws_=nullptr;
    MemoHttpTransport *ws_guard_=nullptr;
    esp_transport_handle_t ws_transport_=nullptr;
    MessageBufferHandle_t incoming_=nullptr;
    StaticMessageBuffer_t incoming_control_{};
    uint8_t *incoming_storage_=nullptr;
    char *fragment_=nullptr;
    size_t fragment_size_=0,frame_offset_=0;
    bool fragmented_=false,leased_=false;
    uint32_t retry_after_=0;
    uint64_t http_received_=0,http_returned_=0;
    esp_log_level_t http_log_=ESP_LOG_NONE,ws_log_=ESP_LOG_NONE;
    std::atomic<bool> failed_{false};
    std::atomic<memo_cloud::Code> failure_{memo_cloud::Code::Transport};
    bool ready_clock(int64_t deadline);
    void close_http();
    void close_ws();
    static void event(void *,esp_event_base_t,int32_t,void *);
};
