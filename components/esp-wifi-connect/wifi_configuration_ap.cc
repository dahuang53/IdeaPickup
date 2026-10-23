// Provisioning transport rebuilt from the pinned ESP-IDF default-netif and
// softap_sta example, and 78/esp-wifi-connect's APSTA connection flow.
// See VOICE_MEMO_PATCHES.md for provenance and the retained project adapters.
#include "wifi_configuration_ap.h"
#include <cstring>
#include <cstdio>
#include <esp_err.h>
#include <esp_wifi.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <lwip/sockets.h>
#include <cJSON.h>
#include "ssid_manager.h"
#include "memo_service_portal.h"
#define TAG "WifiConfigurationAp"
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define AP_STARTED_BIT BIT2
#define AP_STOPPED_BIT BIT3
#define SCAN_DONE_BIT BIT4
extern const char index_html_start[] asm("_binary_wifi_configuration_html_start");
namespace {
memo_service_status_fn service_status;
memo_service_apply_fn service_apply;
char portal_csrf[33];
bool portal_origin(httpd_req_t *req,bool mutating) {
    char host[80]={},origin[100]={},csrf[40]={},type[64]={};
    if(httpd_req_get_hdr_value_str(req,"Host",host,sizeof(host))!=ESP_OK ||
       (strcmp(host,"192.168.4.1") && strcmp(host,"192.168.4.1:80")))return false;
    if(httpd_req_get_hdr_value_len(req,"Origin") &&
       (httpd_req_get_hdr_value_str(req,"Origin",origin,sizeof(origin))!=ESP_OK ||
        (strcmp(origin,"http://192.168.4.1") && strcmp(origin,"http://192.168.4.1:80"))))return false;
    return !mutating || (httpd_req_get_hdr_value_str(req,"Content-Type",type,sizeof(type))==ESP_OK &&
        !strcmp(type,"application/json") &&
        httpd_req_get_hdr_value_str(req,"X-Memo-CSRF",csrf,sizeof(csrf))==ESP_OK &&
        strlen(csrf)==32 && !memcmp(csrf,portal_csrf,32));
}

}
extern "C" void memo_service_portal_register(memo_service_status_fn status, memo_service_apply_fn apply) {
    service_status = status; service_apply = apply;
}
WifiConfigurationAp::WifiConfigurationAp() {
    event_group_ = xEventGroupCreate();
    ESP_ERROR_CHECK(event_group_ ? ESP_OK : ESP_ERR_NO_MEM);
}
WifiConfigurationAp::~WifiConfigurationAp() {
    Stop();
    vEventGroupDelete(event_group_);
}
void WifiConfigurationAp::SetLanguage(const std::string& language) { language_ = language; }
void WifiConfigurationAp::SetSsidPrefix(const std::string& prefix) { ssid_prefix_ = prefix; }
std::string WifiConfigurationAp::GetSsid() {
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));
    char ssid[33];
    snprintf(ssid, sizeof(ssid), "%s-%02X%02X", ssid_prefix_.c_str(), mac[4], mac[5]);
    return ssid;
}
std::string WifiConfigurationAp::GetWebServerUrl() { return "http://192.168.4.1"; }
void WifiConfigurationAp::OnExitRequested(std::function<void()> callback) {
    on_exit_requested_ = std::move(callback);
}
void WifiConfigurationAp::Start() {
    if (running_.load()) return;
    // WifiManager creates the default interfaces once, as in IDF's APSTA example.
    // The SDK's AP_START/AP_STOP actions own DHCP; do not stop/restart it by hand.
    ap_netif_ = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    sta_netif_ = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    ESP_ERROR_CHECK(ap_netif_ && sta_netif_ ? ESP_OK : ESP_ERR_INVALID_STATE);
    xEventGroupClearBits(event_group_, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT |
                        AP_STARTED_BIT | AP_STOPPED_BIT | SCAN_DONE_BIT);
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
        WifiEventHandler, this, &instance_any_id_));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
        IpEventHandler, this, &instance_got_ip_));
    // Retain R12's validated 8 KiB HTTP reservation before Wi-Fi allocations.
    StartWebServer();
    wifi_config_t config = {};
    const std::string ssid = GetSsid();
    memcpy(config.ap.ssid, ssid.data(), ssid.size());
    config.ap.ssid_len = ssid.size();
    config.ap.channel = 1;
    config.ap.max_connection = 4;
    config.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Registered after SDK handlers: this bit follows netif/DHCP startup.
    ESP_ERROR_CHECK(xEventGroupWaitBits(event_group_, AP_STARTED_BIT, pdFALSE,
        pdFALSE, pdMS_TO_TICKS(5000)) & AP_STARTED_BIT ? ESP_OK : ESP_ERR_TIMEOUT);
    esp_netif_ip_info_t ip = {};
    ESP_ERROR_CHECK(esp_netif_get_ip_info(ap_netif_, &ip));
    connected_.store(false);
    running_.store(true);
    const auto& saved = SsidManager::GetInstance().GetSsidList();
    if (!saved.empty()) RequestConnection(saved.front().ssid, saved.front().password, false);
    ESP_LOGI(TAG, "Provisioning AP ready");
}
std::vector<wifi_ap_record_t> WifiConfigurationAp::GetAccessPoints() {
    // Use the SDK scan API only when the existing web page requests a list.
    // No background timer keeps retuning the radio while a phone is idle.
    if (running_.load() && !is_connecting_.load()) {
        xEventGroupClearBits(event_group_, SCAN_DONE_BIT);
        if (esp_wifi_scan_start(nullptr, false) == ESP_OK)
            xEventGroupWaitBits(event_group_, SCAN_DONE_BIT, pdFALSE, pdFALSE,
                                pdMS_TO_TICKS(5000));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return ap_records_;
}
void WifiConfigurationAp::WifiEventHandler(void* arg, esp_event_base_t,
                                            int32_t id, void*) {
    auto* self = static_cast<WifiConfigurationAp*>(arg);
    if (id == WIFI_EVENT_AP_START) xEventGroupSetBits(self->event_group_, AP_STARTED_BIT);
    else if (id == WIFI_EVENT_AP_STOP) xEventGroupSetBits(self->event_group_, AP_STOPPED_BIT);
    else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        self->connected_.store(false);
        xEventGroupSetBits(self->event_group_, WIFI_FAIL_BIT);
    }
    else if (id == WIFI_EVENT_SCAN_DONE) {
        std::lock_guard<std::mutex> lock(self->mutex_);
        uint16_t count = 0;
        esp_wifi_scan_get_ap_num(&count);
        self->ap_records_.resize(count);
        if (count) {
            if (esp_wifi_scan_get_ap_records(&count, self->ap_records_.data()) != ESP_OK)
                count = 0;
            self->ap_records_.resize(count);
        } else esp_wifi_clear_ap_list();
        xEventGroupSetBits(self->event_group_, SCAN_DONE_BIT);
    }
}
void WifiConfigurationAp::IpEventHandler(void* arg, esp_event_base_t,
                                          int32_t id, void* data) {
    auto* self = static_cast<WifiConfigurationAp*>(arg);
    auto* event = static_cast<ip_event_got_ip_t*>(data);
    if (id == IP_EVENT_STA_GOT_IP && event && event->esp_netif == self->sta_netif_) {
        self->connected_.store(true);
        xEventGroupSetBits(self->event_group_, WIFI_CONNECTED_BIT);
    }
}
void WifiConfigurationAp::Stop() {
    if (!running_.exchange(false)) return;
    connected_.store(false);
    xEventGroupSetBits(event_group_, WIFI_FAIL_BIT); // Wake a connection wait on exit.
    // Join all HTTP callbacks before invalidating session state or stopping Wi-Fi.
    if (server_) { ESP_ERROR_CHECK(httpd_stop(server_)); server_ = nullptr; }
    memset(portal_csrf, 0, sizeof(portal_csrf));
    std::lock_guard<std::mutex> operation(connection_mutex_);
    { std::lock_guard<std::mutex> lock(mutex_);
      pending_password_.assign(pending_password_.size(), '\0'); pending_password_.clear();
      pending_ssid_.clear(); request_pending_ = request_active_ = false; connection_ = {}; }

    esp_wifi_scan_stop();
    esp_wifi_clear_ap_list();
    xEventGroupClearBits(event_group_, AP_STOPPED_BIT);
    ESP_ERROR_CHECK(esp_wifi_stop());
    // Drain the SDK netif/DHCP stop action before the next mode can start.
    ESP_ERROR_CHECK(xEventGroupWaitBits(event_group_, AP_STOPPED_BIT, pdFALSE,
        pdFALSE, pdMS_TO_TICKS(5000)) & AP_STOPPED_BIT ? ESP_OK : ESP_ERR_TIMEOUT);
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id_);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip_);
    instance_any_id_ = nullptr; instance_got_ip_ = nullptr;
    is_connecting_.store(false);
    { std::lock_guard<std::mutex> lock(mutex_); ap_records_.clear(); }
    ESP_LOGI(TAG, "Provisioning AP stopped");
}

void WifiConfigurationAp::StartWebServer()
{
    // Start the web server
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // HTTPD owns three control/listener sockets. With the locked lwIP limit
    // of ten, keep three slots for concurrent device HTTP/TLS requests; LRU
    // must run before the global descriptor pool is exhausted.
    config.max_open_sockets = 4;
    config.max_uri_handlers = 6; // service GET/POST, index, scan, submit, exit.
    config.open_fn = [](httpd_handle_t, int socket) -> esp_err_t {
        sockaddr_storage local{}; socklen_t size = sizeof(local);
        esp_netif_ip_info_t ap{};
        auto* netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (!netif || esp_netif_get_ip_info(netif, &ap) != ESP_OK ||
            getsockname(socket, reinterpret_cast<sockaddr*>(&local), &size) != 0)
            return ESP_FAIL;
        if (local.ss_family == AF_INET && size >= sizeof(sockaddr_in)) {
            const auto* address = reinterpret_cast<const sockaddr_in*>(&local);
            return address->sin_addr.s_addr == ap.ip.addr ? ESP_OK : ESP_FAIL;
        }
        // IDF HTTPD uses a dual-stack listener with CONFIG_LWIP_IPV6. lwIP's
        // getsockname then returns IPv4 destinations as ::ffff:a.b.c.d.
        if (local.ss_family == AF_INET6 && size >= sizeof(sockaddr_in6)) {
            const auto* address = reinterpret_cast<const sockaddr_in6*>(&local);
            return IN6_IS_ADDR_V4MAPPED(&address->sin6_addr) &&
                !memcmp(&address->sin6_addr.s6_addr[12], &ap.ip.addr, sizeof(ap.ip.addr))
                ? ESP_OK : ESP_FAIL;
        }
        return ESP_FAIL;
    };
    config.lru_purge_enable = true;
    // Current-service save calls the bounded NVS adapter synchronously; leave
    // stack space for its URL validation and durable readback. No HTTP pairing here.
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard;
    // 5G Network takes longer to connect
    config.recv_wait_timeout = 15;
    config.send_wait_timeout = 15;
    ESP_ERROR_CHECK(httpd_start(&server_, &config));

    uint8_t random[16];esp_fill_random(random,sizeof(random));
    for(unsigned i=0;i<sizeof(random);++i)snprintf(portal_csrf+i*2,3,"%02x",random[i]);
    httpd_uri_t service_get={
        .uri="/service",.method=HTTP_GET,
        .handler=[](httpd_req_t *req)->esp_err_t {
            if(!portal_origin(req,false))return httpd_resp_send_err(req,HTTPD_403_FORBIDDEN,"Origin rejected");
            char response[1024]={};
            if(!service_status || !service_status(response,sizeof(response)))
                return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Status unavailable");
            cJSON *json=cJSON_Parse(response);
            if(!json)return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Status unavailable");
            cJSON_AddStringToObject(json,"csrf",portal_csrf);
            auto wifi = static_cast<WifiConfigurationAp*>(req->user_ctx)->GetConnectionStatus();
            cJSON *state = cJSON_AddObjectToObject(json, "wifi");
            cJSON_AddStringToObject(state, "state", wifi.state.c_str());
            cJSON_AddStringToObject(state, "ssid", wifi.ssid.c_str());
            cJSON_AddStringToObject(state, "error", wifi.error.c_str());
            cJSON_AddBoolToObject(state, "saved", wifi.saved);
            cJSON_AddBoolToObject(state, "connected", wifi.connected);
            char *text=cJSON_PrintUnformatted(json);cJSON_Delete(json);
            if(!text)return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Status unavailable");
            httpd_resp_set_type(req,"application/json");httpd_resp_set_hdr(req,"Cache-Control","no-store");
            esp_err_t result=httpd_resp_sendstr(req,text);cJSON_free(text);return result;
        },.user_ctx=this
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_,&service_get));
    httpd_uri_t service_post={
        .uri="/service",.method=HTTP_POST,
        .handler=[](httpd_req_t *req)->esp_err_t {
            if(!portal_origin(req,true))return httpd_resp_send_err(req,HTTPD_403_FORBIDDEN,"Origin or session rejected");
            if(!service_apply || req->content_len==0 || req->content_len>1024)
                return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Invalid service request");
            char body[1025]={},response[1024]={};size_t at=0;
            while(at<req->content_len) {
                int n=httpd_req_recv(req,body+at,req->content_len-at);
                if(n<=0)return httpd_resp_send_408(req);
                at+=static_cast<size_t>(n);
            }
            bool accepted=service_apply(body,at,response,sizeof(response));
            httpd_resp_set_type(req,"application/json");httpd_resp_set_hdr(req,"Cache-Control","no-store");
            if(!accepted)httpd_resp_set_status(req,"409 Conflict");
            return httpd_resp_sendstr(req,response[0]?response:"{\"success\":false,\"error\":\"设置暂不可用，请重试\"}");
        },.user_ctx=this
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_,&service_post));

    // Register the index.html file
    httpd_uri_t index_html = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            httpd_resp_set_hdr(req, "Connection", "close");
            httpd_resp_send(req, index_html_start, strlen(index_html_start));
            return ESP_OK;
        },
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &index_html));

    // Register the /scan URI
    httpd_uri_t scan = {
        .uri = "/scan",
        .method = HTTP_GET,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            auto *this_ = static_cast<WifiConfigurationAp *>(req->user_ctx);
            auto records = this_->GetAccessPoints();

            // Check if 5G is supported
            bool support_5g = false;
#ifdef CONFIG_SOC_WIFI_SUPPORT_5G
            support_5g = true;
#endif

            cJSON *root = cJSON_CreateObject();
            cJSON *aps = root ? cJSON_AddArrayToObject(root, "aps") : nullptr;
            if (!root || !aps) {
                cJSON_Delete(root);
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to encode scan result");
                return ESP_FAIL;
            }
            cJSON_AddBoolToObject(root, "support_5g", support_5g);
            for (size_t i = 0; i < records.size(); i++) {
                ESP_LOGI(TAG, "SSID: %s, RSSI: %d, Authmode: %d",
                    (char *)records[i].ssid, records[i].rssi, records[i].authmode);
                cJSON *item = cJSON_CreateObject();
                if (!item) continue;
                cJSON_AddStringToObject(item, "ssid", reinterpret_cast<const char *>(records[i].ssid));
                cJSON_AddNumberToObject(item, "rssi", records[i].rssi);
                cJSON_AddNumberToObject(item, "authmode", records[i].authmode);
                cJSON_AddItemToArray(aps, item);
            }
            char *payload = cJSON_PrintUnformatted(root);
            cJSON_Delete(root);
            if (!payload) {
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to encode scan result");
                return ESP_FAIL;
            }
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            httpd_resp_set_hdr(req, "Connection", "close");
            httpd_resp_sendstr(req, payload);
            cJSON_free(payload);
            return ESP_OK;
        },
        .user_ctx = this
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &scan));

    // Register the form submission
    httpd_uri_t form_submit = {
        .uri = "/submit",
        .method = HTTP_POST,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            if(!portal_origin(req,true))return httpd_resp_send_err(req,HTTPD_403_FORBIDDEN,"Origin or session rejected");
            httpd_resp_set_type(req,"application/json");
            httpd_resp_set_hdr(req,"Cache-Control","no-store");
            char *buf;
            size_t buf_len = req->content_len;
            if (buf_len > 1024) { // 限制最大请求体大小
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Payload too large");
                return ESP_FAIL;
            }

            buf = (char *)malloc(buf_len + 1);
            if (!buf) {
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to allocate memory");
                return ESP_FAIL;
            }

            size_t received = 0;
            while (received < buf_len) {
                int ret = httpd_req_recv(req, buf + received, buf_len - received);
                if (ret <= 0) {
                    free(buf);
                    if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                        httpd_resp_send_408(req);
                    } else {
                        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive request");
                    }
                    return ESP_FAIL;
                }
                received += static_cast<size_t>(ret);
            }
            buf[received] = '\0';

            // 解析 JSON 数据
            cJSON *json = memo_service_json_text_safe(buf,received)?cJSON_ParseWithLengthOpts(buf,received+1,nullptr,true):nullptr;
            free(buf);
            if (!json) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
                return ESP_FAIL;
            }

            cJSON *ssid_item = cJSON_GetObjectItemCaseSensitive(json, "ssid");
            cJSON *password_item = cJSON_GetObjectItemCaseSensitive(json, "password");

            if (!cJSON_IsString(ssid_item) || (ssid_item->valuestring == NULL) || (!strlen(ssid_item->valuestring)) || (strlen(ssid_item->valuestring) >= 33)) {
                cJSON_Delete(json);
                httpd_resp_send(req, "{\"success\":false,\"error\":\"Wi-Fi 名称为空或超过32字节\"}", HTTPD_RESP_USE_STRLEN);
                return ESP_OK;
            }

            if(!cJSON_IsString(password_item) || strlen(password_item->valuestring)>64) {
                cJSON_Delete(json);
                return httpd_resp_sendstr(req,"{\"success\":false,\"error\":\"请检查 Wi-Fi 密码\"}");
            }
            std::string ssid_str=ssid_item->valuestring,password_str=password_item->valuestring;

            // 获取当前对象
            auto *this_ = static_cast<WifiConfigurationAp *>(req->user_ctx);
            bool accepted = this_->RequestConnection(ssid_str, password_str);
            password_str.assign(password_str.size(), '\0');
            cJSON_Delete(json);
            if (!accepted) {
                httpd_resp_set_status(req, "409 Conflict");
                return httpd_resp_sendstr(req,"{\"success\":false,\"error\":\"连接正在处理，请等待当前结果\"}");
            }
            httpd_resp_set_status(req, "202 Accepted");
            return httpd_resp_sendstr(req,"{\"success\":true,\"message\":\"已接收，正在连接 Wi-Fi\"}");
        },
        .user_ctx = this
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &form_submit));

    // Register the exit endpoint - exits config mode without rebooting
    httpd_uri_t exit_config = {
        .uri = "/exit",
        .method = HTTP_POST,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            if(!portal_origin(req,true))return httpd_resp_send_err(req,HTTPD_403_FORBIDDEN,"Origin or session rejected");
            auto* this_ = static_cast<WifiConfigurationAp*>(req->user_ctx);

            // 设置响应头，防止浏览器缓存
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            httpd_resp_set_hdr(req, "Connection", "close");
            // 发送响应
            httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);

            // 延迟调用回调，确保HTTP响应完全发送
            ESP_LOGI(TAG, "Exiting config mode...");
            xTaskCreate([](void *ctx) {
                // 等待200ms确保HTTP响应完全发送
                vTaskDelay(pdMS_TO_TICKS(200));

                auto* self = static_cast<WifiConfigurationAp*>(ctx);
                // 通知回调退出配网模式
                if (self->on_exit_requested_) {
                    self->on_exit_requested_();
                }
                vTaskDelete(NULL);
            }, "exit_config_task", 4096, this_, 5, NULL);

            return ESP_OK;
        },
        .user_ctx = this
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &exit_config));

    // No captive DNS, connectivity-probe redirects, or synthetic Internet success.
    // Unknown paths use httpd's ordinary 404; the page is opened by its literal IP.
    ESP_LOGI(TAG, "Web server started");
}

bool WifiConfigurationAp::ConnectToWifi(const std::string &ssid, const std::string &password)
{
    if (!running_.load() || ssid.empty()) {
        ESP_LOGE(TAG, "SSID cannot be empty");
        return false;
    }

    if (ssid.length() > 32) {  // WiFi SSID 最大长度
        ESP_LOGE(TAG, "SSID too long");
        return false;
    }

    if (password.length() > 64) {
        ESP_LOGE(TAG, "Password too long");
        return false;
    }

    is_connecting_ = true;
    esp_wifi_scan_stop();
    last_connected_channel_ = 0;

    // esp_wifi_disconnect queues its event on the default event loop. Consume
    // that old-link event before starting another attempt, or it can be mistaken
    // for the new attempt's failure while the driver is still connecting.
    auto disconnect_station = [this]() {
        wifi_ap_record_t previous = {};
        const bool had_link = esp_wifi_sta_get_ap_info(&previous) == ESP_OK;
        xEventGroupClearBits(event_group_, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        connected_.store(false);
        auto ret = esp_wifi_disconnect();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "WiFi cancellation failed: %d", ret);
            return false;
        }
        // An already idle station need not generate a disconnect event. Bound
        // the drain in that case; an existing link must acknowledge disconnect.
        EventBits_t bits = xEventGroupWaitBits(event_group_, WIFI_FAIL_BIT,
            pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
        xEventGroupClearBits(event_group_, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        connected_.store(false);
        if (had_link && !(bits & WIFI_FAIL_BIT)) {
            ESP_LOGW(TAG, "WiFi disconnect event timed out");
            return false;
        }
        return running_.load();
    };
    if (!disconnect_station()) return false;

    // Retain the upstream bounded retry/backoff for temporary AP refusal.
    constexpr int kMaxAttempts = 2;
    constexpr int kRetryDelayMs = 3000;
    bool connected = false;

    for (int attempt = 1; attempt <= kMaxAttempts && !connected && running_.load(); ++attempt) {
        if (attempt > 1) {
            ESP_LOGI(TAG,
                "WiFi attempt %d/%d after %d ms delay "
                "(waiting for AP comeback timer + state settle)",
                attempt, kMaxAttempts, kRetryDelayMs);
            for (int elapsed = 0; elapsed < kRetryDelayMs && running_.load(); elapsed += 100)
                vTaskDelay(pdMS_TO_TICKS(100));
            if (!running_.load()) break;
        }

        xEventGroupClearBits(event_group_, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        esp_wifi_scan_stop();

        wifi_config_t wifi_config;
        bzero(&wifi_config, sizeof(wifi_config));
        size_t ssid_len = ssid.size();
        if (ssid_len > sizeof(wifi_config.sta.ssid)) {
            ssid_len = sizeof(wifi_config.sta.ssid);
        }
        memcpy(wifi_config.sta.ssid, ssid.data(), ssid_len);

        size_t password_len = password.size();
        if (password_len > sizeof(wifi_config.sta.password)) {
            password_len = sizeof(wifi_config.sta.password);
        }
        memcpy(wifi_config.sta.password, password.data(), password_len);
        wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        wifi_config.sta.failure_retry_cnt = 1;

        auto ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        if (ret != ESP_OK) {
            // User-driven network changes can legitimately fail. Report failure
            // through the existing portal status; never abort/reboot here.
            ESP_LOGW(TAG, "esp_wifi_set_config failed: %d (attempt %d/%d)",
                ret, attempt, kMaxAttempts);
            if (!disconnect_station()) break;
            continue;
        }
        ret = esp_wifi_connect();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG,
                "esp_wifi_connect failed: %d (attempt %d/%d)",
                ret, attempt, kMaxAttempts);
            if (!disconnect_station()) break;
            continue;
        }
        ESP_LOGI(TAG, "Connecting to WiFi %s (attempt %d/%d)",
            ssid.c_str(), attempt, kMaxAttempts);

        // Wait for the connection to complete for 10 or 25 seconds.
        EventBits_t bits = xEventGroupWaitBits(
            event_group_,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdTRUE,
            pdFALSE,
#ifdef CONFIG_SOC_WIFI_SUPPORT_5G
            pdMS_TO_TICKS(25000)
#else
            pdMS_TO_TICKS(10000)
#endif
        );

        if (bits & WIFI_CONNECTED_BIT) {
            connected = true;
        } else {
            const bool timed_out = (bits == 0);
            ESP_LOGW(TAG,
                "Attempt %d/%d %s%s",
                attempt, kMaxAttempts,
                timed_out ? "timed out (driver may still be connecting)" : "failed",
                attempt < kMaxAttempts ? " — will retry" : "");
            // Also cancel after a failure notification: a delayed disconnect
            // must not leave an in-flight connection alive across the retry.
            if (!disconnect_station()) break;
        }
    }
    if (connected && running_.load()) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            last_connected_channel_ = ap_info.primary;
        }
        ESP_LOGI(TAG, "Connected to WiFi %s, channel %u", ssid.c_str(), last_connected_channel_);
        return true;
    } else {
        ESP_LOGE(TAG,
            "Failed to connect to WiFi %s after %d attempts",
            ssid.c_str(), kMaxAttempts);
        return false;
    }
}

bool WifiConfigurationAp::Save(const std::string &ssid, const std::string &password)
{
    uint8_t channel = last_connected_channel_;
    if (channel == 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& rec : ap_records_) {
            if (ssid == reinterpret_cast<const char*>(rec.ssid)) {
                channel = rec.primary;
                break;
            }
        }
    }
    ESP_LOGI(TAG, "Save SSID %s channel %u", ssid.c_str(), channel);
    return SsidManager::GetInstance().AddSsid(ssid, password, channel);
}

bool WifiConfigurationAp::RequestConnection(const std::string& ssid, const std::string& password, bool save) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_.load() || ssid.empty() || ssid.size() > 32 || password.size() > 64) return false;
    if (request_active_) return pending_ssid_ == ssid && pending_password_ == password;
    pending_ssid_ = ssid; pending_password_ = password; persist_request_ = save;
    request_pending_ = request_active_ = true;
    is_connecting_.store(true);
    connection_ = {"connecting", ssid, "", !save, false};
    return true;
}
WifiPortalStatus WifiConfigurationAp::GetConnectionStatus() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = connection_;
    result.connected = connected_.load() && running_.load();
    if (result.state == "connected" && !result.connected) result.state = "disconnected";
    return result;
}
void WifiConfigurationAp::ProcessConnection() {
    std::lock_guard<std::mutex> operation(connection_mutex_);
    std::string ssid, password; bool persist;
    { std::lock_guard<std::mutex> lock(mutex_);
      if (!running_.load() || !request_pending_) return;
      ssid = pending_ssid_; password = pending_password_; persist = persist_request_;
      request_pending_ = false; }
    bool connected = ConnectToWifi(ssid, password) && running_.load();
    bool saved = connected && (!persist || Save(ssid, password));
    if (!saved) { connected_.store(false); esp_wifi_disconnect(); }
    password.assign(password.size(), '\0');
    { std::lock_guard<std::mutex> lock(mutex_);
      connection_.state = saved ? "connected" : connected ? "save_failed" : "failed";
      connection_.saved = saved;
      connection_.error = saved ? "" : connected ? "Wi-Fi 设置尚未确认保存，请重试" :
          "Wi-Fi 连接失败，请检查名称、密码和2.4GHz网络";
      pending_password_.assign(pending_password_.size(), '\0'); pending_password_.clear();
      pending_ssid_.clear(); request_active_ = false;
      is_connecting_.store(false); }
}
