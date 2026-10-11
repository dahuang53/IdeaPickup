#include "power_policy.h"
#include "sdkconfig.h"

#include "app.h"
#include "board.h"
#include "network_sync.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define POLICY_POLL_MS 250U
#define EXTERNAL_DEBOUNCE_MS 1000U
#define CRITICAL_RETRY_MS 5000U
#define SLEEP_CHECK_US (30ULL * 1000ULL * 1000ULL)
#define SHUTDOWN_MS (30ULL * 60ULL * 1000ULL)

static const char *TAG = "MEMO_POWER";
static atomic_int_fast64_t s_last_activity_us;
static atomic_int s_stage = MEMO_POWER_ACTIVE;
static atomic_bool s_initialized;
static atomic_bool s_external;
static atomic_bool s_busy;
static atomic_bool s_battery_valid;
static atomic_bool s_battery_low;
static atomic_bool s_critical_pending;
static atomic_uint_fast16_t s_battery_raw;
static atomic_uint_fast16_t s_battery_guard_raw;
static atomic_uint s_battery_percent;
static int64_t s_last_critical_attempt_us;
static memo_battery_guard_t s_battery_guard;

static int64_t now_us(void) { return esp_timer_get_time(); }

static void apply_active_brightness(bool external_power)
{
    unsigned brightness = memo_power_active_brightness(true, external_power);
    esp_err_t error = board_display_set_brightness(brightness);
    if (error != ESP_OK)
        ESP_LOGE(TAG, "brightness=%u%% failed: %s", brightness, esp_err_to_name(error));
}

void power_policy_note_activity(const char *source)
{
    if (!board_power_policy_supported()) return;
    atomic_store(&s_last_activity_us, now_us());
    if (source) ESP_LOGD(TAG, "ACTIVITY source=%s", source);
}

void power_policy_get_status(memo_power_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->supported = board_power_policy_supported();
    out->initialized = atomic_load(&s_initialized);
    out->external_power = atomic_load(&s_external);
    out->busy = atomic_load(&s_busy);
    out->battery_valid = atomic_load(&s_battery_valid);
    out->battery_low = atomic_load(&s_battery_low);
    out->critical_pending = atomic_load(&s_critical_pending);
    out->battery_raw = (uint16_t)atomic_load(&s_battery_raw);
    out->battery_guard_raw = (uint16_t)atomic_load(&s_battery_guard_raw);
    out->battery_percent = atomic_load(&s_battery_percent);
    out->stage = (memo_power_stage_t)atomic_load(&s_stage);
    int64_t elapsed = now_us() - atomic_load(&s_last_activity_us);
    out->idle_seconds = elapsed > 0 ? (uint32_t)(elapsed / 1000000LL) : 0;
}

bool power_policy_recovery_ready(void)
{
    if (!board_power_policy_supported()) return true;
    uint16_t guard = 0;
    bool valid = board_battery_protection_raw(&guard);
    return memo_battery_recovery_ready(true, board_external_power_present(), valid, guard);
}

static void update_battery(bool external, bool busy, int64_t now)
{
    uint16_t raw = 0;
    bool valid = board_battery_raw(&raw);
    uint16_t guard = 0;
    bool guard_valid = board_battery_protection_raw(&guard);
    bool low = (valid && memo_battery_low(raw)) || (guard_valid && memo_battery_low(guard));
    atomic_store(&s_battery_valid, valid);
    atomic_store(&s_battery_raw, raw);
    atomic_store(&s_battery_guard_raw, guard);
    atomic_store(&s_battery_percent, valid ? memo_battery_percent(raw) : 0U);
    atomic_store(&s_battery_low, low);
    board_battery_set_poll_ms((!external && (busy || low)) ? 1000U : 5000U);

    bool rising = board_battery_rising();
    bool due = memo_battery_shutdown_due(&s_battery_guard, external, rising,
                                         guard_valid, guard, (uint64_t)(now / 1000LL));
    atomic_store(&s_critical_pending, s_battery_guard.tracking);
    if (!due) return;
    if (s_last_critical_attempt_us &&
        now - s_last_critical_attempt_us < (int64_t)CRITICAL_RETRY_MS * 1000LL)
        return;
    s_last_critical_attempt_us = now;
    ESP_LOGW(TAG,
             "LOW_BATTERY_CRITICAL raw=%u guard=%u percent=%u busy=%d rising=%d; closing local work",
             raw, guard, memo_battery_percent(raw), busy, rising);
    if (!app_power_prepare_shutdown(8000)) {
        ESP_LOGE(TAG, "LOW_BATTERY shutdown preparation deferred");
        return;
    }
    if (board_external_power_present() || board_battery_rising()) {
        ESP_LOGI(TAG, "LOW_BATTERY shutdown cancelled: external power detected");
        app_power_cancel_shutdown();
        app_power_recover_deferred();
        s_battery_guard.tracking = false;
        s_battery_guard.since_ms = 0;
        atomic_store(&s_critical_pending, false);
        return;
    }
    atomic_store(&s_stage, MEMO_POWER_SHUTDOWN);
    esp_err_t error = board_sd_unmount();
    if (error == ESP_OK) board_power_off();
    ESP_LOGE(TAG, "LOW_BATTERY power off cancelled: SD unmount %s",
             esp_err_to_name(error));
    app_power_cancel_shutdown();
}

static void ui_sleep_face(void)
{
    if (lvgl_port_lock(500)) {
        ui_power_show_sleep();
        lvgl_port_unlock();
    }
}

static void ui_awake_face(void)
{
    if (lvgl_port_lock(500)) {
        ui_power_show_awake();
        lvgl_port_unlock();
    }
}

static void enter_active(bool visible_wake)
{
    memo_power_stage_t old = (memo_power_stage_t)atomic_exchange(&s_stage, MEMO_POWER_ACTIVE);
    if (old == MEMO_POWER_BUTTON_SLEEP) {
        board_display_set_enabled(true);
        esp_err_t lvgl_error = lvgl_port_resume();
        if (lvgl_error != ESP_OK && lvgl_error != ESP_ERR_INVALID_STATE)
            ESP_LOGE(TAG, "LVGL resume failed: %s", esp_err_to_name(lvgl_error));
    }
    apply_active_brightness(atomic_load(&s_external));
    network_sync_power_set_saving(false);
    network_sync_power_resume();
    app_power_set_listening(true, 0);
    if (visible_wake) ui_awake_face();
    if (old != MEMO_POWER_ACTIVE)
        ESP_LOGI(TAG, "STAGE active from=%d voice=on display=on", (int)old);
}

static bool enter_button_sleep(void)
{
    if (!app_power_set_listening(false, 2000)) {
        ESP_LOGW(TAG, "BUTTON_SLEEP deferred: audio did not become idle");
        return false;
    }
    if (!network_sync_power_suspend()) {
        app_power_set_listening(true, 0);
        ESP_LOGI(TAG, "BUTTON_SLEEP deferred: network work active");
        return false;
    }
    ui_sleep_face();
    esp_err_t lvgl_error = lvgl_port_stop();
    if (lvgl_error != ESP_OK && lvgl_error != ESP_ERR_INVALID_STATE) {
        network_sync_power_resume();
        app_power_set_listening(true, 0);
        ESP_LOGE(TAG, "BUTTON_SLEEP deferred: LVGL stop %s", esp_err_to_name(lvgl_error));
        return false;
    }
    if (board_display_set_enabled(false) != ESP_OK) {
        lvgl_port_resume();
        network_sync_power_resume();
        app_power_set_listening(true, 0);
        return false;
    }
    atomic_store(&s_stage, MEMO_POWER_BUTTON_SLEEP);
    ESP_LOGI(TAG, "STAGE button_sleep voice=off wifi=off display=off wake=GPIO0");
    return true;
}

static void button_sleep_loop(void)
{
    for (;;) {
        int64_t now = now_us();
        uint64_t idle_ms = now > atomic_load(&s_last_activity_us) ?
            (uint64_t)(now - atomic_load(&s_last_activity_us)) / 1000ULL : 0;
        bool external = board_external_power_present();
        atomic_store(&s_external, external);
        if (external) {
            power_policy_note_activity("external_power");
            enter_active(true);
            return;
        }
        if (app_power_is_busy() || network_sync_power_busy()) {
            power_policy_note_activity("late_busy");
            enter_active(true);
            return;
        }
        if (idle_ms >= SHUTDOWN_MS) {
            atomic_store(&s_stage, MEMO_POWER_SHUTDOWN);
            ESP_LOGI(TAG, "STAGE shutdown idle_ms=%" PRIu64, idle_ms);
            esp_err_t error = board_sd_unmount();
            if (error == ESP_OK) board_power_off();
            ESP_LOGE(TAG, "POWER_OFF cancelled: SD unmount %s", esp_err_to_name(error));
            power_policy_note_activity("unmount_failed");
            enter_active(true);
            return;
        }
        uint64_t remaining_us = (SHUTDOWN_MS - idle_ms) * 1000ULL;
        uint64_t sleep_us = remaining_us < SLEEP_CHECK_US ? remaining_us : SLEEP_CHECK_US;
        board_sleep_wake_t wake = board_light_sleep(sleep_us);
        if (wake == BOARD_SLEEP_WAKE_TIMER) continue;
        if (wake == BOARD_SLEEP_WAKE_BUTTON) {
            vTaskDelay(pdMS_TO_TICKS(120));
            power_policy_note_activity("GPIO0");
            enter_active(true);
            return;
        }
        if (wake == BOARD_SLEEP_WAKE_EXTERNAL_POWER) {
            power_policy_note_activity("external_power");
            atomic_store(&s_external, true);
            enter_active(true);
            return;
        }
        ESP_LOGW(TAG, "unexpected light-sleep exit; restoring active mode");
        power_policy_note_activity("sleep_error");
        enter_active(true);
        return;
    }
}

static void policy_task(void *unused)
{
    (void)unused;
    bool raw_external = board_external_power_present();
    bool stable_external = raw_external;
    int64_t raw_changed_us = now_us();
    bool was_busy = false;
    atomic_store(&s_external, stable_external);
    atomic_store(&s_initialized, true);
    apply_active_brightness(stable_external);
#if CONFIG_MEMO_VOCAT16
    ESP_LOGI(TAG, "READY supported=1 external=%d brightness=%u%% idle=2m dim; convenient wake under validation",
#else
    ESP_LOGI(TAG, "READY supported=1 external=%d brightness=%u%% idle=2m/5m/30m",
#endif
             stable_external, memo_power_active_brightness(true, stable_external));

    for (;;) {
        int64_t now = now_us();
        bool raw = board_external_power_present();
        if (raw != raw_external) {
            raw_external = raw;
            raw_changed_us = now;
        } else if (raw != stable_external &&
                   now - raw_changed_us >= (int64_t)EXTERNAL_DEBOUNCE_MS * 1000LL) {
            stable_external = raw;
            atomic_store(&s_external, stable_external);
            ESP_LOGI(TAG, "EXTERNAL_POWER present=%d", stable_external);
            if (stable_external) app_power_recover_deferred();
            if ((memo_power_stage_t)atomic_load(&s_stage) == MEMO_POWER_ACTIVE) {
                apply_active_brightness(stable_external);
                ESP_LOGI(TAG, "BRIGHTNESS source=%s value=%u%%",
                         stable_external ? "external" : "battery",
                         memo_power_active_brightness(true, stable_external));
            }
            power_policy_note_activity("power_change");
        }

        bool busy = app_power_is_busy() || network_sync_power_busy();
        atomic_store(&s_busy, busy);
        update_battery(stable_external, busy, now);
        if (busy) {
            was_busy = true;
        } else if (was_busy) {
            was_busy = false;
            power_policy_note_activity("work_complete");
        }
        if (stable_external) power_policy_note_activity("external_power");

        uint64_t idle_ms = now > atomic_load(&s_last_activity_us) ?
            (uint64_t)(now - atomic_load(&s_last_activity_us)) / 1000ULL : 0;
        memo_power_stage_t desired = memo_power_stage(true, stable_external, busy, idle_ms);
#if CONFIG_MEMO_VOCAT16
        /* VoCat POWER controls SAM8108, while BOOT is inconvenient. Keep voice
         * wake available until touch wake from light sleep is proven on device. */
        if (desired == MEMO_POWER_BUTTON_SLEEP || desired == MEMO_POWER_SHUTDOWN)
            desired = MEMO_POWER_VOICE_SLEEP;
#endif
        memo_power_stage_t current = (memo_power_stage_t)atomic_load(&s_stage);
        if (desired == MEMO_POWER_ACTIVE && current != MEMO_POWER_ACTIVE) {
            enter_active(true);
        } else if (desired == MEMO_POWER_VOICE_SLEEP && current == MEMO_POWER_ACTIVE) {
            ui_sleep_face();
            board_display_set_brightness(15);
            network_sync_power_set_saving(true);
            atomic_store(&s_stage, MEMO_POWER_VOICE_SLEEP);
            ESP_LOGI(TAG, "STAGE voice_sleep voice=on display=15%% wifi=max_modem");
        } else if (desired == MEMO_POWER_BUTTON_SLEEP &&
                   current != MEMO_POWER_BUTTON_SLEEP) {
            if (enter_button_sleep()) button_sleep_loop();
            else power_policy_note_activity("sleep_deferred");
        }
        vTaskDelay(pdMS_TO_TICKS(POLICY_POLL_MS));
    }
}

void power_policy_init(void)
{
    atomic_store(&s_last_activity_us, now_us());
    if (!board_power_policy_supported()) {
        atomic_store(&s_external, true);
        atomic_store(&s_initialized, true);
        ESP_LOGI(TAG, "READY supported=0 board=%s unchanged", board_name());
        return;
    }
    if (board_power_monitor_init() != ESP_OK) {
        ESP_LOGE(TAG, "power monitor unavailable; saving disabled");
        return;
    }
    if (xTaskCreate(policy_task, "memo_power", 4096, NULL, 2, NULL) != pdPASS)
        ESP_LOGE(TAG, "power policy task unavailable; saving disabled");
}
