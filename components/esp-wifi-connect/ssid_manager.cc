#include "ssid_manager.h"

#include <algorithm>
#include <cstring>
#include <esp_log.h>
#include <nvs_flash.h>

#define TAG "SsidManager"
#define NVS_NAMESPACE "wifi"
#define MAX_WIFI_SSID_COUNT 10

static std::string MakeWifiKey(const char* prefix, int index) {
    if (index <= 0) {
        return prefix;
    }
    return std::string(prefix) + std::to_string(index);
}

SsidManager::SsidManager() {
    LoadFromNvs();
}

SsidManager::~SsidManager() {
}

void SsidManager::Clear() {
    if(needs_reload_ && !LoadFromNvs())return;
    ssid_list_.clear();
    SaveToNvs();
}

bool SsidManager::LoadFromNvs() {
    std::vector<SsidItem> loaded;

    // Load ssid / password / channel from NVS namespace "wifi"
    // ssid, ssid1, ... ssid9
    // password, password1, ... password9
    // channel, channel1, ... channel9 (uint8, optional; missing means unknown)
    nvs_handle_t nvs_handle;
    auto ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        if(ret==ESP_ERR_NVS_NOT_FOUND){ssid_list_.clear();needs_reload_=false;return true;}
        ESP_LOGW(TAG, "NVS namespace %s doesn't exist", NVS_NAMESPACE);
        needs_reload_=true;return false;
    }
    for (int i = 0; i < MAX_WIFI_SSID_COUNT; i++) {
        auto ssid_key = MakeWifiKey("ssid", i);
        auto password_key = MakeWifiKey("password", i);
        auto channel_key = MakeWifiKey("channel", i);

        char ssid[33];
        char password[65];
        size_t length = sizeof(ssid);
        ret=nvs_get_str(nvs_handle, ssid_key.c_str(), ssid, &length);
        if(ret==ESP_ERR_NVS_NOT_FOUND)continue;
        if(ret!=ESP_OK){nvs_close(nvs_handle);needs_reload_=true;return false;}
        length = sizeof(password);
        ret=nvs_get_str(nvs_handle, password_key.c_str(), password, &length);
        if(ret==ESP_ERR_NVS_NOT_FOUND)continue;
        if(ret!=ESP_OK){nvs_close(nvs_handle);needs_reload_=true;return false;}
        uint8_t channel = 0;
        ret=nvs_get_u8(nvs_handle, channel_key.c_str(), &channel);
        if(ret!=ESP_OK && ret!=ESP_ERR_NVS_NOT_FOUND){nvs_close(nvs_handle);needs_reload_=true;return false;}
        loaded.push_back({ssid, password, channel});
    }
    nvs_close(nvs_handle);
    ssid_list_=std::move(loaded);needs_reload_=false;return true;
}

bool SsidManager::SaveToNvs() {
    if(commit_begin_ && (!commit_end_ || !commit_begin_())){LoadFromNvs();return false;}
    struct Release { std::function<void()> end; ~Release(){if(end)end();} }
        release{commit_begin_?commit_end_:std::function<void()>{}};
    nvs_handle_t handle;
    if(nvs_open(NVS_NAMESPACE,NVS_READWRITE,&handle)!=ESP_OK){LoadFromNvs();return false;}
    bool ok=true;
    for(int i=0;ok && i<MAX_WIFI_SSID_COUNT;++i){
        auto ssid=MakeWifiKey("ssid",i),password=MakeWifiKey("password",i),channel=MakeWifiKey("channel",i);
        if(i<(int)ssid_list_.size()){
            const auto &item=ssid_list_[i];
            ok=nvs_set_str(handle,ssid.c_str(),item.ssid.c_str())==ESP_OK &&
               nvs_set_str(handle,password.c_str(),item.password.c_str())==ESP_OK &&
               nvs_set_u8(handle,channel.c_str(),item.channel)==ESP_OK;
        } else {
            for(const auto &key:{ssid,password,channel}){
                esp_err_t e=nvs_erase_key(handle,key.c_str());
                if(e!=ESP_OK && e!=ESP_ERR_NVS_NOT_FOUND){ok=false;break;}
            }
        }
    }
    if(ok)ok=nvs_commit(handle)==ESP_OK;
    nvs_close(handle);
    // Read through a fresh handle; RAM state is not evidence of durability.
    if(ok){
        if(nvs_open(NVS_NAMESPACE,NVS_READONLY,&handle)!=ESP_OK)ok=false;
        else {
            for(int i=0;ok && i<MAX_WIFI_SSID_COUNT;++i){
                auto key=MakeWifiKey("ssid",i);char ssid[33];size_t length=sizeof(ssid);
                esp_err_t e=nvs_get_str(handle,key.c_str(),ssid,&length);
                if(i>=(int)ssid_list_.size()){ok=e==ESP_ERR_NVS_NOT_FOUND;continue;}
                const auto &item=ssid_list_[i];
                ok=e==ESP_OK && length==item.ssid.size()+1 && !memcmp(ssid,item.ssid.c_str(),length);
                key=MakeWifiKey("password",i);char password[65];length=sizeof(password);
                if(ok)ok=nvs_get_str(handle,key.c_str(),password,&length)==ESP_OK && length==item.password.size()+1 && !memcmp(password,item.password.c_str(),length);
                key=MakeWifiKey("channel",i);uint8_t channel=0;
                if(ok)ok=nvs_get_u8(handle,key.c_str(),&channel)==ESP_OK && channel==item.channel;
            }
            nvs_close(handle);
        }
    }
    if(!ok)LoadFromNvs();
    return ok;
}

bool SsidManager::AddSsid(const std::string& ssid, const std::string& password, uint8_t channel) {
    if(needs_reload_ && !LoadFromNvs())return false;
    for (auto it = ssid_list_.begin(); it != ssid_list_.end(); ++it) {
        auto& item = *it;
        ESP_LOGI(TAG, "compare [%s:%d] [%s:%d]", item.ssid.c_str(), item.ssid.size(), ssid.c_str(),
                 ssid.size());
        if (item.ssid == ssid) {
            ESP_LOGW(TAG, "SSID %s already exists, overwrite it", ssid.c_str());
            item.password = password;
            if (channel != 0) {
                item.channel = channel;
            }
            // The network just selected on the configuration page is the default
            // on exit/re-entry too; commit order and credentials together once.
            std::rotate(ssid_list_.begin(), it, it + 1);
            return SaveToNvs();
        }
    }

    if (ssid_list_.size() >= MAX_WIFI_SSID_COUNT) {
        ESP_LOGW(TAG, "SSID list is full, pop one");
        ssid_list_.pop_back();
    }
    // Add the new ssid to the front of the list
    ssid_list_.insert(ssid_list_.begin(), {ssid, password, channel});
    return SaveToNvs();
}

void SsidManager::UpdateSsidChannel(const std::string& ssid, uint8_t channel) {
    if(needs_reload_ && !LoadFromNvs())return;
    if (channel == 0) {
        return;
    }
    for (auto& item : ssid_list_) {
        if (item.ssid != ssid) {
            continue;
        }
        if (item.channel != channel) {
            ESP_LOGI(TAG, "Updated channel for %s: %u -> %u", ssid.c_str(), item.channel, channel);
            item.channel = channel;
            SaveToNvs();
        }
        return;
    }
}

void SsidManager::RemoveSsid(int index) {
    if(needs_reload_ && !LoadFromNvs())return;
    if (index < 0 || static_cast<size_t>(index) >= ssid_list_.size()) {
        ESP_LOGW(TAG, "Invalid index %d", index);
        return;
    }
    ssid_list_.erase(ssid_list_.begin() + index);
    SaveToNvs();
}

void SsidManager::SetDefaultSsid(int index) {
    if(needs_reload_ && !LoadFromNvs())return;
    if (index < 0 || static_cast<size_t>(index) >= ssid_list_.size()) {
        ESP_LOGW(TAG, "Invalid index %d", index);
        return;
    }
    // Move the ssid at index to the front of the list
    auto item = ssid_list_[index];
    ssid_list_.erase(ssid_list_.begin() + index);
    ssid_list_.insert(ssid_list_.begin(), item);
    SaveToNvs();
}

std::vector<uint8_t> SsidManager::GetSavedChannels() const {
    std::vector<uint8_t> channels;
    for (const auto& item : ssid_list_) {
        if (item.channel == 0) {
            continue;
        }
        if (std::find(channels.begin(), channels.end(), item.channel) == channels.end()) {
            channels.push_back(item.channel);
        }
    }
    return channels;
}
