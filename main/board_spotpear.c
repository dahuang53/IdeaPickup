/*
 * Board support for Spotpear ESP32-S3 Touch LCD 1.28 BOX (N16R8).
 *
 * Display, touch, audio and power pins are adapted from the upstream Xiaozhi
 * board definition at commit 184a688. SD pins come from Spotpear's published
 * SD player example. See THIRD_PARTY_NOTICES.md for source attribution.
 * This project owns every peripheral instance and never formats the card.
 */
#include "board.h"
#include "power_source.h"
#include <stdbool.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/rtc_io.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "memo_sd_session.h"
#include "driver/usb_serial_jtag.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MEMO_BOARD_NAME "sp-esp32-s3-1.28-box"
/* Match the gain used by Xiaozhi's maintained ES8311 driver for this board.
 * The higher 36 dB setting clips normal nearby speech on the production unit
 * and reduces MultiNet command accuracy even though WakeNet still triggers. */
#define RECORD_MIC_GAIN_DB 30.0f

#define AUDIO_MCLK GPIO_NUM_16
#define AUDIO_BCLK GPIO_NUM_9
#define AUDIO_WS GPIO_NUM_45
#define AUDIO_DIN GPIO_NUM_10
#define AUDIO_DOUT GPIO_NUM_8
#define AUDIO_PA GPIO_NUM_46

#define LCD_SCLK GPIO_NUM_4
#define LCD_MOSI GPIO_NUM_2
#define LCD_CS GPIO_NUM_5
#define LCD_DC GPIO_NUM_47
#define LCD_RESET GPIO_NUM_38
#define LCD_BACKLIGHT GPIO_NUM_42

#define TOUCH_SDA GPIO_NUM_11
#define TOUCH_SCL GPIO_NUM_7
#define TOUCH_RESET GPIO_NUM_6
#define TOUCH_INTERRUPT GPIO_NUM_12

#define BOOT_BUTTON GPIO_NUM_0
#define BATTERY_CHARGING GPIO_NUM_41
#define POWER_HOLD GPIO_NUM_3

#define SD_CLK GPIO_NUM_17
#define SD_CMD GPIO_NUM_18
#define SD_D0 GPIO_NUM_21
#define SD_D3 GPIO_NUM_13

static const char *TAG = "memo_board";
static i2c_master_bus_handle_t s_audio_i2c;
static i2c_master_bus_handle_t s_touch_i2c;
static sdmmc_card_t *s_card;
static lv_display_t *s_display;
static esp_lcd_panel_handle_t s_panel;
static bool s_display_attempted;
static bool s_display_enabled;
static bool s_power_monitor_ready;
static bool s_charging_signal_driven;
static adc_oneshot_unit_handle_t s_battery_adc;
static memo_power_source_t s_power_source;
static bool s_external_power_cached;
static int64_t s_last_battery_sample_us;
static int64_t s_last_power_log_us;
static uint32_t s_battery_poll_ms = 1000U;
static uint16_t s_rise_baseline_raw;
static int64_t s_rise_baseline_us;
static int64_t s_rising_until_us;
static i2s_chan_handle_t s_tx, s_rx;
static const audio_codec_if_t *s_codec;
static const audio_codec_ctrl_if_t *s_codec_ctrl;
static const audio_codec_gpio_if_t *s_codec_gpio;
static bool s_audio_ready;
static enum { AUDIO_IDLE, AUDIO_RECORD, AUDIO_PLAY } s_audio_state;
static volatile uint32_t s_rx_overflows;
static volatile uint32_t s_rx_eofs;
static volatile uint32_t s_rx_last_eof_us;
static volatile uint32_t s_rx_last_overflow_us;
static volatile int32_t s_rx_irq_core = -1;

const char *board_name(void) { return MEMO_BOARD_NAME; }
const char *board_device_id_prefix(void) { return "meow-spotpear"; }

static void touch_released(lv_event_t *event)
{
    lv_indev_t *input = lv_event_get_user_data(event);
    lv_point_t point = {0};
    lv_indev_get_point(input, &point);
    ESP_LOGI(TAG, "TOUCH_RELEASE x=%d y=%d", (int)point.x, (int)point.y);
}

static esp_err_t output_pin(gpio_num_t pin, int level)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "GPIO setup");
    return gpio_set_level(pin, level);
}

static esp_err_t audio_i2c_init_once(void)
{
    if (s_audio_i2c) return ESP_OK;
    const i2c_master_bus_config_t config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = GPIO_NUM_15,
        .scl_io_num = GPIO_NUM_14,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&config, &s_audio_i2c);
}

static esp_err_t touch_i2c_init_once(void)
{
    if (s_touch_i2c) return ESP_OK;
    const i2c_master_bus_config_t config = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = TOUCH_SDA,
        .scl_io_num = TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&config, &s_touch_i2c);
}

esp_err_t board_init_display(void)
{
    if (s_display) return ESP_OK;
    if (s_display_attempted) return ESP_ERR_INVALID_STATE;
    s_display_attempted = true;

    /* GPIO3 holds the product's power latch. It is set before the display or
     * Wi-Fi allocation so the board cannot turn itself off during startup. */
    ESP_RETURN_ON_ERROR(rtc_gpio_init(POWER_HOLD), TAG, "power hold init");
    ESP_RETURN_ON_ERROR(rtc_gpio_set_direction(POWER_HOLD, RTC_GPIO_MODE_OUTPUT_ONLY), TAG,
                        "power hold direction");
    ESP_RETURN_ON_ERROR(rtc_gpio_set_level(POWER_HOLD, 1), TAG, "power hold high");
    ESP_RETURN_ON_ERROR(touch_i2c_init_once(), TAG, "touch I2C");

    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    /* The backlight is active low. Start dark until panel initialization has
     * completed, then select roughly 70 percent brightness. */
    const ledc_channel_config_t backlight = {
        .gpio_num = LCD_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 1023,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");
    ESP_RETURN_ON_ERROR(ledc_channel_config(&backlight), TAG, "backlight");

    const spi_bus_config_t bus = {
        .sclk_io_num = LCD_SCLK,
        .mosi_io_num = LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = 240 * 40 * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "LCD SPI");

    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_DC,
        .cs_gpio_num = LCD_CS,
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    esp_lcd_panel_io_handle_t lcd_io;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI3_HOST,
                                                 &io_config, &lcd_io), TAG, "LCD IO");
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_RESET,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    esp_lcd_panel_handle_t panel;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_gc9a01(lcd_io, &panel_config, &panel), TAG, "LCD panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "LCD reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "LCD init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(panel, true), TAG, "LCD inversion");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "LCD on");
    s_panel = panel;
    s_display_enabled = true;

    /* Spotpear's known-good panel setup adds these vendor power/gamma values
     * after the stock GC9A01 initialization. */
    const uint8_t reg62[] = {0x18, 0x0D, 0x71, 0xED, 0x70, 0x70,
                             0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70};
    const uint8_t reg63[] = {0x18, 0x11, 0x71, 0xF1, 0x70, 0x70,
                             0x18, 0x13, 0x71, 0xF3, 0x70, 0x70};
    const uint8_t regc3[] = {0x1F};
    const uint8_t regc4[] = {0x1F};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(lcd_io, 0x62, reg62, sizeof(reg62)), TAG, "LCD reg62");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(lcd_io, 0x63, reg63, sizeof(reg63)), TAG, "LCD reg63");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(lcd_io, 0xC3, regc3, sizeof(regc3)), TAG, "LCD regC3");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(lcd_io, 0xC4, regc4, sizeof(regc4)), TAG, "LCD regC4");

    lvgl_port_cfg_t lvgl_config = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_config), TAG, "LVGL init");
    const lvgl_port_display_cfg_t display_config = {
        .io_handle = lcd_io,
        .panel_handle = panel,
        .buffer_size = 240 * 20,
        .double_buffer = false,
        .hres = 240,
        .vres = 240,
        .color_format = LV_COLOR_FORMAT_RGB565,
        /* esp_lvgl_port reapplies its own rotation after adding the display.
         * Keep the authoritative orientation here instead of setting the
         * panel beforehand, which the port would immediately overwrite.  The
         * production BOX-THIN unit needs the Y axis mirrored at the final
         * panel owner. */
        .rotation = {.swap_xy = false, .mirror_x = false, .mirror_y = true},
        .flags = {.buff_dma = true, .swap_bytes = true},
    };
    lv_display_t *display = lvgl_port_add_disp(&display_config);
    ESP_RETURN_ON_FALSE(display, ESP_ERR_NO_MEM, TAG, "LVGL display");

    esp_lcd_panel_io_i2c_config_t touch_io_config = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    esp_lcd_panel_io_handle_t touch_io;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_touch_i2c, &touch_io_config, &touch_io), TAG, "touch IO");
    const esp_lcd_touch_config_t touch_config = {
        .x_max = 240,
        .y_max = 240,
        .rst_gpio_num = TOUCH_RESET,
        .int_gpio_num = TOUCH_INTERRUPT,
        .levels = {.reset = 0, .interrupt = 0},
        /* CST816D and GC9A01 have different native X directions on this BOX.
         * Actual left/right taps reported X=208/55 without touch X mirroring;
         * mirror the input only so it matches the upright displayed controls. */
        .flags = {.swap_xy = 0, .mirror_x = 1, .mirror_y = 1},
    };
    esp_lcd_touch_handle_t touch;
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_cst816s(touch_io, &touch_config, &touch), TAG, "touch init");
    const lvgl_port_touch_cfg_t input_config = {.disp = display, .handle = touch};
    lv_indev_t *input = lvgl_port_add_touch(&input_config);
    ESP_RETURN_ON_FALSE(input, ESP_ERR_NO_MEM, TAG, "LVGL touch");
    if (lvgl_port_lock(0)) {
        lv_indev_add_event_cb(input, touch_released, LV_EVENT_RELEASED, input);
        lvgl_port_unlock();
    }

    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 307), TAG, "brightness");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0), TAG, "brightness update");
    s_display = display;
    ESP_LOGI(TAG, "DISPLAY_READY board=%s 240x240 GC9A01 CST816D display_swap=0 display_mirror_x=0 display_mirror_y=1 touch_swap=0 touch_mirror_x=1 touch_mirror_y=1",
             board_name());
    return ESP_OK;
}

bool board_power_policy_supported(void) { return true; }

static bool battery_adc_read(uint16_t *raw)
{
    if (!s_battery_adc || !raw) return false;
    uint32_t sum = 0;
    for (unsigned i = 0; i < 8; ++i) {
        int value = 0;
        if (adc_oneshot_read(s_battery_adc, ADC_CHANNEL_0, &value) != ESP_OK)
            return false;
        sum += (uint32_t)value;
    }
    *raw = (uint16_t)((sum + 4U) / 8U);
    return true;
}

esp_err_t board_power_monitor_init(void)
{
    if (s_power_monitor_ready) return ESP_OK;
    const gpio_config_t button = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&button), TAG, "power button input");

    /* Some revisions of this product advertise GPIO41 as a charging signal,
     * while the schematic for the production BOX-THIN unit only breaks it out
     * to a header.  Do not mistake a floating input plus our own pull-up for
     * external power: sample it once with both weak pulls and only trust a
     * level which the board itself holds against both of them. */
    const gpio_config_t charging = {
        .pin_bit_mask = 1ULL << BATTERY_CHARGING,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&charging), TAG, "charging input");
    ESP_RETURN_ON_ERROR(gpio_set_pull_mode(BATTERY_CHARGING, GPIO_PULLUP_ONLY), TAG,
                        "charging probe pull-up");
    vTaskDelay(1);
    int charge_pull_up = gpio_get_level(BATTERY_CHARGING);
    ESP_RETURN_ON_ERROR(gpio_set_pull_mode(BATTERY_CHARGING, GPIO_PULLDOWN_ONLY), TAG,
                        "charging probe pull-down");
    vTaskDelay(1);
    int charge_pull_down = gpio_get_level(BATTERY_CHARGING);
    s_charging_signal_driven = charge_pull_up == charge_pull_down;
    ESP_RETURN_ON_ERROR(gpio_set_pull_mode(BATTERY_CHARGING, GPIO_PULLUP_ONLY), TAG,
                        "charging input pull-up");
    const adc_oneshot_unit_init_cfg_t adc_unit = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t adc_error = adc_oneshot_new_unit(&adc_unit, &s_battery_adc);
    if (adc_error == ESP_OK) {
        const adc_oneshot_chan_cfg_t adc_channel = {
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_12,
        };
        adc_error = adc_oneshot_config_channel(s_battery_adc, ADC_CHANNEL_0,
                                               &adc_channel);
    }
    if (adc_error != ESP_OK) {
        ESP_LOGW(TAG, "battery ADC unavailable: %s", esp_err_to_name(adc_error));
        if (s_battery_adc) {
            adc_oneshot_del_unit(s_battery_adc);
            s_battery_adc = NULL;
        }
    }
    memo_power_source_init(&s_power_source);
    s_power_monitor_ready = true;
    s_external_power_cached = board_external_power_present();
    uint16_t battery_raw = s_power_source.last_raw;
    ESP_LOGI(TAG,
             "POWER_INPUTS boot=%d charge_up=%d charge_down=%d charge_driven=%d usb_host=%d battery_raw=%u external=%d",
             gpio_get_level(BOOT_BUTTON), charge_pull_up, charge_pull_down,
             s_charging_signal_driven, usb_serial_jtag_is_connected(),
             battery_raw, s_external_power_cached);
    return ESP_OK;
}

bool board_external_power_present(void)
{
    if (!s_power_monitor_ready) return false;
    int64_t now_us = esp_timer_get_time();
    bool usb_host = usb_serial_jtag_is_connected();
    bool charge_high = gpio_get_level(BATTERY_CHARGING) == 1;
    bool adc_valid = false;
    uint16_t raw = s_power_source.last_raw;
    if (!s_last_battery_sample_us ||
        now_us - s_last_battery_sample_us >= (int64_t)s_battery_poll_ms * 1000LL) {
        adc_valid = battery_adc_read(&raw);
        s_last_battery_sample_us = now_us;
        if (adc_valid) {
            if (!s_rise_baseline_us) {
                s_rise_baseline_us = now_us;
                s_rise_baseline_raw = raw;
            } else if (now_us - s_rise_baseline_us >= 5000000LL) {
                if (raw >= (uint16_t)(s_rise_baseline_raw + 8U))
                    s_rising_until_us = now_us + 12000000LL;
                s_rise_baseline_us = now_us;
                s_rise_baseline_raw = raw;
            }
        }
    }
    bool old_external = s_external_power_cached;
    s_external_power_cached = memo_power_source_update(
        &s_power_source, (uint64_t)(now_us / 1000LL), usb_host,
        s_charging_signal_driven, charge_high, adc_valid, raw);
    if (old_external != s_external_power_cached || !s_last_power_log_us ||
        now_us - s_last_power_log_us >= 60000000LL) {
        ESP_LOGI(TAG,
                 "POWER_SOURCE usb_host=%d charge_valid=%d charge_high=%d battery_raw=%u delta=%d adc_external=%d external=%d",
                 usb_host, s_charging_signal_driven, charge_high,
                 s_power_source.last_raw, s_power_source.window_delta,
                 s_power_source.adc_external, s_external_power_cached);
        s_last_power_log_us = now_us;
    }
    return s_external_power_cached;
}

bool board_battery_raw(uint16_t *raw)
{
    if (!raw || !s_power_monitor_ready || !s_power_source.initialized) return false;
    *raw = s_power_source.last_raw;
    return true;
}

bool board_battery_protection_raw(uint16_t *raw) { return board_battery_raw(raw); }

esp_err_t board_battery_diagnostics(board_battery_diagnostics_t *out)
{
    (void)out;
    return ESP_ERR_NOT_SUPPORTED;
}

bool board_battery_rising(void)
{
    return s_power_monitor_ready && esp_timer_get_time() < s_rising_until_us;
}

void board_battery_set_poll_ms(uint32_t interval_ms)
{
    if (interval_ms < 1000U) interval_ms = 1000U;
    if (interval_ms > 30000U) interval_ms = 30000U;
    s_battery_poll_ms = interval_ms;
}

esp_err_t board_display_set_brightness(unsigned percent)
{
    if (!s_display) return ESP_ERR_INVALID_STATE;
    if (percent > 100) percent = 100;
    if (s_power_monitor_ready)
        memo_power_source_load_changed(&s_power_source,
                                       (uint64_t)(esp_timer_get_time() / 1000LL));
    uint32_t duty = (1023U * (100U - percent) + 50U) / 100U;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty), TAG,
                        "brightness");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

esp_err_t board_display_set_enabled(bool enabled)
{
    if (!s_panel) return ESP_ERR_INVALID_STATE;
    if (enabled == s_display_enabled) return ESP_OK;
    if (!enabled) ESP_RETURN_ON_ERROR(board_display_set_brightness(0), TAG, "backlight off");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, enabled), TAG, "display power");
    s_display_enabled = enabled;
    return ESP_OK;
}

board_sleep_wake_t board_light_sleep(uint64_t timeout_us)
{
    if (!s_power_monitor_ready || !timeout_us) return BOARD_SLEEP_WAKE_OTHER;
    if (gpio_get_level(BOOT_BUTTON) == 0) return BOARD_SLEEP_WAKE_BUTTON;
    if (board_external_power_present()) return BOARD_SLEEP_WAKE_EXTERNAL_POWER;

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (gpio_wakeup_enable(BOOT_BUTTON, GPIO_INTR_LOW_LEVEL) != ESP_OK ||
        (s_charging_signal_driven &&
         gpio_wakeup_enable(BATTERY_CHARGING, GPIO_INTR_HIGH_LEVEL) != ESP_OK) ||
        esp_sleep_enable_gpio_wakeup() != ESP_OK ||
        esp_sleep_enable_timer_wakeup(timeout_us) != ESP_OK) {
        gpio_wakeup_disable(BOOT_BUTTON);
        if (s_charging_signal_driven) gpio_wakeup_disable(BATTERY_CHARGING);
        return BOARD_SLEEP_WAKE_OTHER;
    }
    rtc_gpio_hold_en(POWER_HOLD);
    ESP_LOGI(TAG, "LIGHT_SLEEP_ENTER timeout_ms=%" PRIu64, timeout_us / 1000ULL);
    esp_err_t error = esp_light_sleep_start();
    rtc_gpio_hold_dis(POWER_HOLD);
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    gpio_wakeup_disable(BOOT_BUTTON);
    if (s_charging_signal_driven) gpio_wakeup_disable(BATTERY_CHARGING);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    ESP_LOGI(TAG, "LIGHT_SLEEP_EXIT result=%s cause=%d boot=%d charge=%d usb_host=%d external=%d",
             esp_err_to_name(error), (int)cause, gpio_get_level(BOOT_BUTTON),
             gpio_get_level(BATTERY_CHARGING), usb_serial_jtag_is_connected(),
             board_external_power_present());
    if (error != ESP_OK) return BOARD_SLEEP_WAKE_OTHER;
    if (cause == ESP_SLEEP_WAKEUP_GPIO && gpio_get_level(BOOT_BUTTON) == 0)
        return BOARD_SLEEP_WAKE_BUTTON;
    if (cause == ESP_SLEEP_WAKEUP_GPIO && board_external_power_present())
        return BOARD_SLEEP_WAKE_EXTERNAL_POWER;
    if (cause == ESP_SLEEP_WAKEUP_TIMER) return BOARD_SLEEP_WAKE_TIMER;
    return BOARD_SLEEP_WAKE_OTHER;
}

void board_power_off(void)
{
    ESP_LOGI(TAG, "POWER_OFF safe SD unmount complete");
    fflush(stdout);
    board_display_set_enabled(false);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    rtc_gpio_hold_dis(POWER_HOLD);
    rtc_gpio_set_level(POWER_HOLD, 0);
    rtc_gpio_hold_en(POWER_HOLD);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_deep_sleep_start();
}

static bool s_sd_no_response;
static bool s_sd_response_seen;
static bool s_sd_probe_active;
static esp_err_t sd_presence_transaction(int slot, sdmmc_command_t *cmd)
{
    esp_err_t err=sdmmc_host_do_transaction(slot,cmd);
    esp_err_t response_error=err==ESP_OK?cmd->error:err;
    if(s_sd_probe_active && (cmd->flags & SCF_RSP_PRESENT) && response_error!=ESP_ERR_TIMEOUT)
        s_sd_response_seen=true; // CRC/bus errors are ambiguous, never "no card".
    return err;
}
bool board_sd_absent(void) { return s_sd_no_response; }
esp_err_t board_sd_check(void) { return s_card?sdmmc_get_status(s_card):ESP_ERR_INVALID_STATE; }
esp_err_t board_sd_verify_session(void) { return memo_sd_session_check(s_card); }

esp_err_t board_sd_mount(void)
{
    if (s_card) return ESP_OK;
    ESP_RETURN_ON_ERROR(output_pin(SD_D3, 1), TAG, "SD D3 pull-up");
    const esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 6,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.do_transaction = sd_presence_transaction;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = SD_CLK;
    slot.cmd = SD_CMD;
    slot.d0 = SD_D0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    s_sd_response_seen=false;s_sd_no_response=false;s_sd_probe_active=true;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(BOARD_SD_PATH, &host, &slot, &mount, &s_card);
    s_sd_probe_active=false;
    s_sd_no_response=(err==ESP_ERR_TIMEOUT && !s_sd_response_seen);
    ESP_LOGI(TAG,"SD_PRESENCE response=%d absent=%d",s_sd_response_seen,s_sd_no_response);
    ESP_LOGI(TAG, "SD_MOUNT result=%s mode=1bit clk=17 cmd=18 d0=21 format_if_mount_failed=false",
             esp_err_to_name(err));
    return err;
}

esp_err_t board_sd_unmount(void)
{
    if (!s_card) return ESP_OK;
    sdmmc_card_t *card = s_card;
    s_card = NULL;
    esp_err_t err = esp_vfs_fat_sdcard_unmount(BOARD_SD_PATH, card);
    ESP_LOGI(TAG, "SD_UNMOUNT result=%s", esp_err_to_name(err));
    return err;
}

static esp_err_t codec_result(int result, const char *action)
{
    if (result == ESP_CODEC_DEV_OK) return ESP_OK;
    ESP_LOGE(TAG, "%s: codec error %d", action, result);
    return ESP_FAIL;
}

static esp_codec_dev_sample_info_t audio_format(void)
{
    return (esp_codec_dev_sample_info_t) {
        .sample_rate = BOARD_AUDIO_RATE,
        .channel = 1,
        .bits_per_sample = 16,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
    };
}

static bool rx_eof(i2s_chan_handle_t channel, i2s_event_data_t *event, void *ctx)
{
    (void)channel;
    (void)event;
    (void)ctx;
    s_rx_last_eof_us = (uint32_t)esp_timer_get_time();
    s_rx_irq_core = xPortGetCoreID();
    s_rx_eofs++;
    return false;
}

static bool rx_overflow(i2s_chan_handle_t channel, i2s_event_data_t *event, void *ctx)
{
    (void)channel;
    (void)event;
    (void)ctx;
    s_rx_last_overflow_us = (uint32_t)esp_timer_get_time();
    s_rx_overflows++;
    return false;
}

static esp_err_t drain_rx_queue(void)
{
    /* The IDF I2S message queue survives disable/enable.  A buffer completed
     * just before RX was stopped for playback can therefore make the first
     * new DMA completion look like a queue overflow.  Drain only before the
     * recording session becomes visible; real overflows after this function
     * remain counted and still invalidate the recording. */
    int16_t discard[128];
    size_t bytes = 0;
    unsigned buffers = 0;
    while (buffers < 64) {
        esp_err_t err = i2s_channel_read(s_rx, discard, sizeof(discard), &bytes, 0);
        if (err == ESP_ERR_TIMEOUT) break;
        if (err != ESP_OK) return err;
        buffers++;
    }
    ESP_LOGD(TAG, "RX queue drained buffers=%u", buffers);
    return buffers < 64 ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t board_audio_init(void)
{
    if (s_audio_ready) return ESP_OK;
    if (s_tx || s_rx || s_codec) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(audio_i2c_init_once(), TAG, "audio I2C");
    ESP_RETURN_ON_ERROR(output_pin(AUDIO_PA, 0), TAG, "PA off");

    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.auto_clear = true;
    /* A 60-second durability checkpoint may briefly block on an SD metadata
     * flush. Keep roughly 512 ms of microphone DMA headroom for that bounded
     * operation; overflow monitoring still rejects any actual loss. */
    channel_config.dma_desc_num = 64;
    channel_config.dma_frame_num = 128;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_config, &s_tx, &s_rx), TAG, "I2S channels");
    i2s_std_slot_config_t slot = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    slot.slot_mask = I2S_STD_SLOT_LEFT;
    const i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BOARD_AUDIO_RATE),
        .slot_cfg = slot,
        .gpio_cfg = {
            .mclk = AUDIO_MCLK,
            .bclk = AUDIO_BCLK,
            .ws = AUDIO_WS,
            .dout = AUDIO_DOUT,
            .din = AUDIO_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &config), TAG, "I2S TX init");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &config), TAG, "I2S RX init");
    const i2s_event_callbacks_t callbacks = {.on_recv = rx_eof, .on_recv_q_ovf = rx_overflow};
    ESP_RETURN_ON_ERROR(i2s_channel_register_event_callback(s_rx, &callbacks, NULL), TAG, "RX overflow monitor");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "I2S clock enable");

    audio_codec_i2c_cfg_t control = {
        .port = I2C_NUM_0,
        .bus_handle = s_audio_i2c,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
    };
    s_codec_ctrl = audio_codec_new_i2c_ctrl(&control);
    s_codec_gpio = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(s_codec_ctrl && s_codec_gpio, ESP_ERR_NO_MEM, TAG, "codec controls");
    uint8_t reset = 0x1F;
    ESP_RETURN_ON_ERROR(codec_result(s_codec_ctrl->write_reg(s_codec_ctrl, 0x00, 1, &reset, 1),
                                     "ES8311 reset"), TAG, "ES8311 reset");
    vTaskDelay(pdMS_TO_TICKS(5));
    es8311_codec_cfg_t codec_config = {
        .ctrl_if = s_codec_ctrl,
        .gpio_if = s_codec_gpio,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = AUDIO_PA,
        .use_mclk = true,
    };
    s_codec = es8311_codec_new(&codec_config);
    ESP_RETURN_ON_FALSE(s_codec, ESP_FAIL, TAG, "ES8311 initialization");
    esp_codec_dev_sample_info_t format = audio_format();
    ESP_RETURN_ON_ERROR(codec_result(s_codec->set_fs(s_codec, &format), "codec format"), TAG, "codec format");

    /* Force a real enable->mute->disable transition. The ES8311 constructor
     * initializes the PA before its enabled flag is established. */
    ESP_RETURN_ON_ERROR(codec_result(s_codec->enable(s_codec, true), "codec first enable"), TAG, "codec first enable");
    ESP_RETURN_ON_ERROR(codec_result(s_codec->mute(s_codec, true), "codec first mute"), TAG, "codec first mute");
    ESP_RETURN_ON_ERROR(codec_result(s_codec->enable(s_codec, false), "codec standby"), TAG, "codec standby");
    ESP_RETURN_ON_ERROR(gpio_set_level(AUDIO_PA, 0), TAG, "PA standby");
    s_audio_ready = true;
    ESP_LOGI(TAG, "AUDIO_READY board=%s rate=16000 bits=16 mic_gain=%.1fdB ES8311 mono left",
             board_name(), (double)RECORD_MIC_GAIN_DB);
    return ESP_OK;
}

uint32_t board_record_overflows(void) { return s_rx_overflows; }
void board_record_rx_diagnostics(board_rx_diagnostics_t *out)
{
    if (!out) return;
    out->eof_count = s_rx_eofs;
    out->overflow_count = s_rx_overflows;
    out->last_eof_us = s_rx_last_eof_us;
    out->last_overflow_us = s_rx_last_overflow_us;
    out->irq_core = s_rx_irq_core;
}

esp_err_t board_record_resync(void)
{
    if (!s_audio_ready || s_audio_state != AUDIO_RECORD) return ESP_ERR_INVALID_STATE;
    return drain_rx_queue();
}

esp_err_t board_record_open(void)
{
    if (!s_audio_ready || s_audio_state != AUDIO_IDLE) return ESP_ERR_INVALID_STATE;
    esp_codec_dev_sample_info_t format = audio_format();
    ESP_RETURN_ON_ERROR(codec_result(s_codec->set_fs(s_codec, &format), "codec format restore"), TAG, "codec format restore");
    ESP_RETURN_ON_ERROR(codec_result(s_codec->enable(s_codec, true), "ADC enable"), TAG, "ADC enable");
    esp_err_t err = codec_result(s_codec->mute(s_codec, true), "DAC mute while recording");
    bool rx_started = false;
    if (err == ESP_OK) err = gpio_set_level(AUDIO_PA, 0);
    if (err == ESP_OK) err = codec_result(s_codec->set_mic_gain(s_codec, RECORD_MIC_GAIN_DB), "mic gain");
    if (err == ESP_OK) {
        err = i2s_channel_enable(s_rx);
        rx_started = err == ESP_OK;
    }
    if (err == ESP_OK) err = drain_rx_queue();
    if (err != ESP_OK) {
        if (rx_started) i2s_channel_disable(s_rx);
        s_codec->enable(s_codec, false);
        gpio_set_level(AUDIO_PA, 0);
        return err;
    }
    s_audio_state = AUDIO_RECORD;
    return ESP_OK;
}

esp_err_t board_record_read(int16_t *mono, size_t samples)
{
    if (s_audio_state != AUDIO_RECORD) return ESP_ERR_INVALID_STATE;
    if (!mono || !samples || samples > BOARD_AUDIO_MAX_SAMPLES) return ESP_ERR_INVALID_ARG;
    size_t bytes = 0;
    const size_t expected = samples * sizeof(int16_t);
    esp_err_t err = i2s_channel_read(s_rx, mono, expected, &bytes, 200);
    if (err != ESP_OK) return err;
    return bytes == expected ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t board_record_close(void)
{
    if (s_audio_state != AUDIO_RECORD) return ESP_ERR_INVALID_STATE;
    esp_err_t err = i2s_channel_disable(s_rx);
    esp_err_t codec_err = codec_result(s_codec->enable(s_codec, false), "ADC stop");
    esp_err_t pa_err = gpio_set_level(AUDIO_PA, 0);
    s_audio_state = AUDIO_IDLE;
    if (err != ESP_OK || codec_err != ESP_OK || pa_err != ESP_OK) s_audio_ready = false;
    return err != ESP_OK ? err : (codec_err != ESP_OK ? codec_err : pa_err);
}

esp_err_t board_play_open(void)
{
    if (!s_audio_ready || s_audio_state != AUDIO_IDLE) return ESP_ERR_INVALID_STATE;
    esp_codec_dev_sample_info_t format = audio_format();
    ESP_RETURN_ON_ERROR(codec_result(s_codec->set_fs(s_codec, &format), "codec format restore"), TAG, "codec format restore");
    ESP_RETURN_ON_ERROR(codec_result(s_codec->enable(s_codec, true), "DAC enable"), TAG, "DAC enable");
    esp_err_t err = codec_result(s_codec->set_vol(s_codec, -12.0f), "DAC volume");
    if (err == ESP_OK) err = codec_result(s_codec->mute(s_codec, false), "DAC unmute");
    if (err != ESP_OK) {
        s_codec->enable(s_codec, false);
        gpio_set_level(AUDIO_PA, 0);
        return err;
    }
    s_audio_state = AUDIO_PLAY;
    return ESP_OK;
}

esp_err_t board_play_write(const int16_t *mono, size_t samples)
{
    if (s_audio_state != AUDIO_PLAY) return ESP_ERR_INVALID_STATE;
    if (!mono || !samples || samples > BOARD_AUDIO_MAX_SAMPLES) return ESP_ERR_INVALID_ARG;
    size_t bytes = 0;
    const size_t expected = samples * sizeof(int16_t);
    esp_err_t err = i2s_channel_write(s_tx, mono, expected, &bytes, 200);
    if (err != ESP_OK) return err;
    return bytes == expected ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t board_play_close(void)
{
    if (s_audio_state != AUDIO_PLAY) return ESP_ERR_INVALID_STATE;
    vTaskDelay(pdMS_TO_TICKS(140));
    esp_err_t err = codec_result(s_codec->mute(s_codec, true), "DAC mute");
    esp_err_t stop_err = codec_result(s_codec->enable(s_codec, false), "DAC stop");
    esp_err_t pa_err = gpio_set_level(AUDIO_PA, 0);
    s_audio_state = AUDIO_IDLE;
    if (err != ESP_OK || stop_err != ESP_OK || pa_err != ESP_OK) s_audio_ready = false;
    return err != ESP_OK ? err : (stop_err != ESP_OK ? stop_err : pa_err);
}
