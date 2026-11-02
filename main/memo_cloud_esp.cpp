#include "memo_cloud_esp.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <algorithm>
#include <cstring>
#include <new>
#include <climits>
#include <ctime>
#include <strings.h>
#include "esp_transport_ws.h"

#if defined(ESP_PLATFORM) && !CONFIG_LOG_DYNAMIC_LEVEL_CONTROL
#error "Cloud credentials require SDK tag-level log suppression"
#endif
#if defined(ESP_PLATFORM) && !CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK
#error "Cloud WS sends and SDK control frames require the official TX lock"
#endif
#ifdef ESP_PLATFORM
static_assert(CONFIG_ESP_WS_CLIENT_TX_LOCK_TIMEOUT_MS==pdMS_TO_TICKS(2000),
              "WS 1.5.0 uses ticks for its nominal-ms TX lock timeout");
#endif

extern "C" bool network_sync_cloud_acquire();
extern "C" void network_sync_cloud_release();
extern "C" bool network_sync_cloud_clock_ready();
namespace {
std::atomic<bool> cloud_owner{false};
constexpr size_t EVENT_MAX=65536;
MemoCloudEsp &self(void *p){return *static_cast<MemoCloudEsp *>(p);}
}
bool MemoCloudEsp::ready_clock(int64_t deadline){
    if(!network_sync_cloud_clock_ready() || esp_timer_get_time()/1000>=deadline){failure_=memo_cloud::Code::Timeout;return false;}
    return true;
}
MemoCloudEsp::MemoCloudEsp(const std::atomic<bool> &cancelled):cancelled_(cancelled){
    using memo_cloud::Code;
    io_.context=this;
    io_.now=[](void *){return esp_timer_get_time()/1000;};
    io_.cancelled=[](void *p){return self(p).cancelled_.load();};
    io_.acquire=[](void *p){
        auto &s=self(p);bool expected=false;
        if(!cloud_owner.compare_exchange_strong(expected,true))return false;
        if(!network_sync_cloud_acquire()){cloud_owner=false;return false;}
        s.http_log_=esp_log_level_get("HTTP_CLIENT");s.ws_log_=esp_log_level_get("transport_ws");
        // Locked SDK logs raw authorization at DEBUG, and WS upgrade write
        // failure even at ERROR. Suppress only these tags for this lease.
        esp_log_level_set("HTTP_CLIENT",ESP_LOG_NONE);esp_log_level_set("transport_ws",ESP_LOG_NONE);
        s.leased_=true;s.failure_=Code::Transport;s.failed_=false;return true;
    };
    io_.release=[](void *p){auto &s=self(p);if(s.leased_){esp_log_level_set("HTTP_CLIENT",s.http_log_);esp_log_level_set("transport_ws",s.ws_log_);network_sync_cloud_release();s.leased_=false;cloud_owner=false;}};
    io_.failure=[](void *p){return self(p).failure_.load();};
    io_.http_open=[](void *p,const char *url,const char *auth,size_t length,int64_t deadline){
        auto &s=self(p);
        if(!s.ready_clock(deadline)||length>INT_MAX)return -1;
        esp_http_client_config_t cfg{};cfg.url=url;cfg.crt_bundle_attach=esp_crt_bundle_attach;
        cfg.disable_auto_redirect=true;cfg.timeout_ms=10000;cfg.buffer_size=4096;cfg.buffer_size_tx=4096;
        s.retry_after_=0;s.http_received_=s.http_returned_=0;cfg.user_data=&s;
        cfg.event_handler=[](esp_http_client_event_t *e)->esp_err_t {
            if(e->event_id==HTTP_EVENT_ON_HEADER && e->header_key && e->header_value && !strcasecmp(e->header_key,"Retry-After"))
                self(e->user_data).retry_after_=memo_cloud::retry_after(e->header_value,time(nullptr));
            if(e->event_id==HTTP_EVENT_ON_DATA&&e->data_len>0)self(e->user_data).http_received_+=e->data_len;
            return ESP_OK;
        };
        s.transport_=new(std::nothrow) MemoHttpTransport(cfg,deadline*1000);
        if(!s.transport_||!s.transport_->ready()){s.failure_=Code::Memory;return -1;}
        s.http_=esp_http_client_init(&cfg);if(!s.http_){s.failure_=Code::Memory;return -1;}
        esp_http_client_set_method(s.http_,HTTP_METHOD_POST);
        esp_http_client_set_header(s.http_,"Authorization",auth);
        esp_http_client_set_header(s.http_,"Content-Type","application/json");
        esp_err_t e=esp_http_client_open(s.http_,static_cast<int>(length));
        if(e!=ESP_OK){s.failure_=s.transport_->tls_verify_flags()?Code::Tls:Code::Transport;return -1;}
        return 0;
    };
    io_.http_write=[](void *p,const void *data,size_t n,int ms){auto &s=self(p);esp_http_client_set_timeout_ms(s.http_,ms);s.transport_->write_window();return esp_http_client_write(s.http_,static_cast<const char *>(data),static_cast<int>(n));};
    io_.http_headers=[](void *p,uint32_t *retry,int ms){
        auto &s=self(p);esp_http_client_set_timeout_ms(s.http_,ms);
        s.transport_->read_window();
        if(esp_http_client_fetch_headers(s.http_)<0)return -1;
        *retry=s.retry_after_;return esp_http_client_get_status_code(s.http_);
    };
    io_.http_read=[](void *p,void *data,size_t n,int ms){auto &s=self(p);esp_http_client_set_timeout_ms(s.http_,ms);s.transport_->read_window();int got=esp_http_client_read(s.http_,static_cast<char *>(data),n);if(got>0)s.http_returned_+=got;return got;};
    io_.http_complete=[](void *p){auto&s=self(p);return esp_http_client_is_complete_data_received(s.http_)&&s.http_received_==s.http_returned_;};
    io_.http_sent=[](void *p){auto &s=self(p);return s.transport_ && s.transport_->may_have_written();};
    io_.http_close=[](void *p){self(p).close_http();};
    io_.ws_open=[](void *p,const char *url,const char *auth,int64_t deadline){
        auto &s=self(p);if(!s.ready_clock(deadline))return -1;
        s.fragment_=static_cast<char *>(heap_caps_malloc(EVENT_MAX,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
        s.incoming_storage_=static_cast<uint8_t *>(heap_caps_malloc(EVENT_MAX+sizeof(size_t)+1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
        if(s.incoming_storage_)s.incoming_=xMessageBufferCreateStatic(EVENT_MAX+sizeof(size_t)+1,s.incoming_storage_,&s.incoming_control_);
        if(!s.fragment_||!s.incoming_){s.failure_=Code::Memory;return -1;}
        esp_http_client_config_t tls{};tls.url="https://cloud";tls.crt_bundle_attach=esp_crt_bundle_attach;
        s.ws_guard_=new(std::nothrow) MemoHttpTransport(tls,0,true);
        if(!s.ws_guard_||!s.ws_guard_->ready()){s.failure_=Code::Memory;return -1;}
        s.ws_transport_=esp_transport_ws_init(s.ws_guard_->handle());
        if(!s.ws_transport_){s.failure_=Code::Memory;return -1;}
        esp_transport_set_default_port(s.ws_transport_,443);
        esp_transport_ws_config_t options{};const char *path=strchr(url+6,'/');
        options.ws_path=path?path:"/";options.headers=auth;options.propagate_control_frames=true;
        if(esp_transport_ws_set_config(s.ws_transport_,&options)!=ESP_OK)return -1;
        esp_websocket_client_config_t cfg{};cfg.uri=url;cfg.ext_transport=s.ws_transport_;
        cfg.crt_bundle_attach=esp_crt_bundle_attach;cfg.disable_auto_reconnect=true;cfg.enable_close_reconnect=false;
        cfg.task_stack=8192;cfg.buffer_size=4096;cfg.network_timeout_ms=10000;
        cfg.ping_interval_sec=10;cfg.pingpong_timeout_sec=20;
        s.ws_=esp_websocket_client_init(&cfg);if(!s.ws_){s.failure_=Code::Memory;return -1;}
        if(esp_websocket_register_events(s.ws_,WEBSOCKET_EVENT_ANY,event,&s)!=ESP_OK || esp_websocket_client_start(s.ws_)!=ESP_OK)return -1;
        return 0;
    };
    io_.ws_send=[](void *p,const char *data,size_t n,int ms){
        auto &s=self(p);if(s.failed_)return -1;
        // SDK repeats its timeout for the TX lock and every frame write.
        // One absolute socket deadline covers the complete send operation.
        s.ws_guard_->send_until(esp_timer_get_time()+static_cast<int64_t>(ms)*1000);
        int sent=esp_websocket_client_send_text(s.ws_,data,n,pdMS_TO_TICKS(ms));
        s.ws_guard_->send_until(0);return sent;
    };
    io_.ws_read=[](void *p,char *data,size_t n,int ms){
        auto &s=self(p);if(s.failed_ && s.failure_.load()!=Code::Transport)return -1;
        size_t received=xMessageBufferReceive(s.incoming_,data,n,s.failed_?0:pdMS_TO_TICKS(ms));
        if(received)return static_cast<int>(received); // Drain final events before a peer close.
        return s.failed_?-1:0;
    };
    io_.ws_close=[](void *p){self(p).close_ws();};
}
void MemoCloudEsp::event(void *p,esp_event_base_t,int32_t type,void *event_data){
    auto &s=self(p);
    auto fail=[&](memo_cloud::Code code){
        auto old=s.failure_.load();
        if(!s.failed_||old==memo_cloud::Code::Transport||code==memo_cloud::Code::Tls)s.failure_=code;
        s.failed_=true;
    };
    if(type==WEBSOCKET_EVENT_ERROR||type==WEBSOCKET_EVENT_DISCONNECTED||type==WEBSOCKET_EVENT_CLOSED){
        auto *e=static_cast<esp_websocket_event_data_t *>(event_data);
        int status=e?e->error_handle.esp_ws_handshake_status_code:0;
        // SDK 1.5.0 leaves TLS members uninitialized for other error types,
        // including the first ERROR before abort_connection sets TCP type.
        int flags=e&&e->error_handle.error_type==WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT?
            e->error_handle.esp_tls_cert_verify_flags:0;
        if(s.ws_guard_)flags|=s.ws_guard_->tls_verify_flags();
        fail(flags?memo_cloud::Code::Tls:
            status==401||status==403?memo_cloud::Code::Credentials:
            status==429?memo_cloud::Code::Limited:memo_cloud::Code::Transport);
        return;
    }
    if(type!=WEBSOCKET_EVENT_DATA||s.failed_)return;
    auto &e=*static_cast<esp_websocket_event_data_t *>(event_data);
    if(e.op_code==8||e.op_code==9||e.op_code==10)return; // SDK handles close/ping/pong.
    if(e.op_code!=0 && e.op_code!=1){fail(memo_cloud::Code::Response);return;}
    if(e.payload_offset==0){
        if(e.op_code==1){if(s.fragmented_){fail(memo_cloud::Code::Response);return;}s.fragment_size_=0;}
        else if(!s.fragmented_){fail(memo_cloud::Code::Response);return;}
        s.frame_offset_=0;
    }
    if(e.data_len<0||(e.data_len&&!e.data_ptr)||e.payload_len<0||e.payload_offset<0||static_cast<size_t>(e.payload_offset)!=s.frame_offset_||e.data_len>e.payload_len-e.payload_offset||s.fragment_size_+e.data_len>EVENT_MAX){fail(memo_cloud::Code::TooLarge);return;}
    memcpy(s.fragment_+s.fragment_size_,e.data_ptr,e.data_len);s.fragment_size_+=e.data_len;s.frame_offset_+=e.data_len;
    if(s.frame_offset_==static_cast<size_t>(e.payload_len)){
        s.fragmented_=!e.fin;
        if(e.fin){if(!s.fragment_size_||xMessageBufferSend(s.incoming_,s.fragment_,s.fragment_size_,0)!=s.fragment_size_)fail(memo_cloud::Code::TooLarge);s.fragment_size_=0;}
    }
}
void MemoCloudEsp::close_http(){if(http_){esp_http_client_close(http_);esp_http_client_cleanup(http_);http_=nullptr;}delete transport_;transport_=nullptr;}
void MemoCloudEsp::close_ws(){
    // Interrupt owned socket before waiting for the SDK worker. Its recv loop
    // may otherwise keep renewing timeouts on a slowly delivered long frame.
    if(ws_guard_)ws_guard_->interrupt();
    if(ws_){esp_websocket_client_stop(ws_);esp_websocket_client_destroy(ws_);ws_=nullptr;}
    if(ws_transport_){esp_transport_destroy(ws_transport_);ws_transport_=nullptr;}
    delete ws_guard_;ws_guard_=nullptr;
    if(incoming_){vMessageBufferDelete(incoming_);incoming_=nullptr;}
    free(incoming_storage_);incoming_storage_=nullptr;
    free(fragment_);fragment_=nullptr;fragment_size_=frame_offset_=0;fragmented_=false;
}
MemoCloudEsp::~MemoCloudEsp(){close_http();close_ws();if(leased_)io_.release(this);}
