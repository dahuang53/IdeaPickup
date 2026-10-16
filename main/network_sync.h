#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "memo_sync_cache.h"
#include "memo_network_config.h"
#if defined(MEMO_HOST_TEST) || defined(CAT_ANIM_TEST)
typedef int esp_err_t;
#else
#include "esp_err.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool initialized;
    bool configured;
    bool connected;
    bool provisioning;
    bool syncing;
    uint32_t cursor_id;
    uint32_t last_uploaded_id;
    uint32_t active_sync_id;
    uint32_t queued_sync_id;
    bool queued_sync_manual;
    char device_id[40];
    char ap_ssid[33];
    char error[80];
} memo_network_status_t;

/* Existing recordings are not uploaded implicitly. Service configuration is
 * separate from Wi-Fi; an unconfigured/local-only device keeps offline audio. */
esp_err_t network_sync_init(uint32_t existing_latest_id);
/* R05 portal adapter uses these; no credentials are returned to the UI.
 * OK means durable settings / queued pairing, not server-confirmed pairing. */
memo_config_result_t network_sync_configure_service(const char *url, const char *pairing_code, bool local_only);
void network_sync_get_service_config(memo_config_public_t *out);
void network_sync_get_status(memo_network_status_t *out);
void network_sync_notify_record(uint32_t record_id);
/* Called by the audio owner after an exact device ID reservation, before capture. */
bool network_sync_track_record(uint32_t record_id);
/* No credential is returned. Binding retained with each actual task. */
bool network_sync_current_target(memo_sync_target_t *out);
void network_sync_set_storage_available(bool available);
/* Raise the valid local ID boundary without scheduling any upload. */
void network_sync_observe_latest(uint32_t record_id);

typedef struct {
    uint32_t id;
    memo_job_state_t state;
    unsigned upload_percent;
    bool request_pending;
    bool cardless;
    bool can_retry;
    bool interrupted;
    char error[80];
} memo_record_sync_t;
/* UI-only snapshots: no SD or network IO on the LVGL task. */
void network_sync_select_record(uint32_t id);
void network_sync_record_status(memo_record_sync_t *out);
bool network_sync_retry_record(uint32_t id);

/* Used by the cardless worker only. Adds the device-scoped authorization and
 * identity headers. It does not read SD or block the audio acquisition task. */
int network_sync_stream_request(const char *method, const char *path,
                                uint32_t record_id, const void *body, size_t length,
                                const char *content_type, const char *chunk_sha,
                                char *response, size_t capacity);

/* Toggle the manual browser configuration hotspot. Safe only while recording/playback are idle. */
void network_sync_toggle_provisioning(void);
/* Temporary read-only AP observer; numeric results only, no credentials. */

/* Prevent SD upload reads from overlapping recording or playback startup. */
bool network_sync_prepare_audio(uint32_t timeout_ms);
void network_sync_audio_done(void);
/* Exclusive, non-reentrant background media query. No retry or SD access if busy. */
bool network_sync_try_sd_probe(void);
void network_sync_sd_probe_done(void);
/* Caller must hold AUDIO; hashes a local file only, also while offline. */
bool network_sync_hash_local_audio(const char *path,memo_audio_identity_t *out);
bool network_sync_sd_work_pending(void);
uint32_t network_sync_sd_scan_count(void);

/* Power policy hooks. They never interrupt provisioning, an active upload or
 * an SD reader. Historical records stay local until selected manually. */
bool network_sync_power_busy(void);
bool network_sync_power_suspend(void);
void network_sync_power_resume(void);
void network_sync_power_set_saving(bool saving);

#ifdef __cplusplus
}
#endif
