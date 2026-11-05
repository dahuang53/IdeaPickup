#ifndef _WIFI_CONFIGURATION_AP_H_
#define _WIFI_CONFIGURATION_AP_H_
#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include <atomic>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_http_server.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_wifi_types_generic.h>
struct WifiPortalStatus {
    std::string state = "not_configured", ssid, error;
    bool saved = false, connected = false;
};
class WifiConfigurationAp {
public:
    WifiConfigurationAp();
    ~WifiConfigurationAp();
    WifiConfigurationAp(const WifiConfigurationAp&) = delete;
    WifiConfigurationAp& operator=(const WifiConfigurationAp&) = delete;
    void SetSsidPrefix(const std::string& prefix);
    bool Start();
    bool RotatePassword();
    const char* GetPassword() const { return password_; }
    void Stop();
    bool RequestConnection(const std::string& ssid, const std::string& password, bool save = true);
    void ProcessConnection(); // Called only by the existing application network worker.
    WifiPortalStatus GetConnectionStatus();
    bool IsConnected() const { return running_.load() && connected_.load(); }
    bool ConnectToWifi(const std::string& ssid, const std::string& password);
    bool Save(const std::string& ssid, const std::string& password);
    std::vector<wifi_ap_record_t> GetAccessPoints();
    std::string GetSsid();
    std::string GetWebServerUrl();
    bool ExitRequested() const { return exit_requested_.load(); }
private:
    std::mutex mutex_;
    std::mutex connection_mutex_; // Joins the worker before Stop tears down the driver.
    WifiPortalStatus connection_;
    std::string pending_ssid_, pending_password_;
    bool request_pending_ = false, request_active_ = false, persist_request_ = false;
    std::atomic<bool> connected_{false};
    httpd_handle_t server_ = nullptr;
    EventGroupHandle_t event_group_;
    std::string ssid_prefix_;
    esp_event_handler_instance_t instance_any_id_ = nullptr, instance_got_ip_ = nullptr;
    esp_netif_t* ap_netif_ = nullptr;
    esp_netif_t* sta_netif_ = nullptr;
    std::atomic<bool> running_{false}, is_connecting_{false};
    std::vector<wifi_ap_record_t> ap_records_;
    uint8_t last_connected_channel_ = 0;
    std::atomic<bool> exit_requested_{false};
    char password_[9] = {};
    bool StoreFreshPassword();
    void StartWebServer();
    static void WifiEventHandler(void*, esp_event_base_t, int32_t, void*);
    static void IpEventHandler(void*, esp_event_base_t, int32_t, void*);
};
#endif
