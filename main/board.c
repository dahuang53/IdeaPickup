/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * Display/pin setup adapted from espressif/esp-bsp, commit
 * b1bc71e7980663110055fbeba7eab59d5e689c2b, bsp/esp_vocat.
 * V1.0/V1.2 pin differences checked against the official schematics and
 * espressif/esp-board-manager 2beb9b22b0892b343bd555a1ebc9929a7edce8fc.
 * See THIRD_PARTY_NOTICES.md for source attribution. This project owns all peripheral setup.
 */
#include "board.h"
#include "memo_gauge_profile.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>

#define RECORD_MIC_GAIN_DB 37.5f
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st77916.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_memory_utils.h"
#include "esp_rom_sys.h"
#include "board_lcd_init.h"

#if CONFIG_MEMO_BOARD_V1_0
#define MEMO_BOARD_NAME "esp_vocat_1_0"
#define AUDIO_DIN GPIO_NUM_15
#define AUDIO_PA GPIO_NUM_4
#define LCD_RESET GPIO_NUM_3
#define LCD_RESET_ACTIVE_HIGH 0
#elif CONFIG_MEMO_BOARD_V1_2
#define MEMO_BOARD_NAME "esp_vocat_1_2"
#define AUDIO_DIN GPIO_NUM_3
#define AUDIO_PA GPIO_NUM_15
#define LCD_RESET GPIO_NUM_47
#define LCD_RESET_ACTIVE_HIGH 1
#else
#error "Select the confirmed ESP-VoCat PCB revision"
#endif

static const char *TAG = "memo_board";
static i2c_master_bus_handle_t s_i2c;
static sdmmc_card_t *s_card;
static bool s_lcd_sd_power;
static bool s_display_attempted;
static lv_display_t *s_display;
static i2s_chan_handle_t s_tx, s_rx;
static const audio_codec_if_t *s_adc, *s_dac;
static const audio_codec_ctrl_if_t *s_adc_ctrl, *s_dac_ctrl;
static const audio_codec_gpio_if_t *s_codec_gpio;
static bool s_audio_ready;
static enum { AUDIO_IDLE, AUDIO_RECORD, AUDIO_PLAY } s_audio_state;
static int16_t s_stereo[BOARD_AUDIO_MAX_SAMPLES * 2];

const char *board_name(void) { return MEMO_BOARD_NAME; }
const char *board_device_id_prefix(void) { return "meow-vocat"; }

static esp_err_t output_pin(gpio_num_t pin, int level)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "GPIO setup");
    return gpio_set_level(pin, level);
}

static esp_err_t lcd_sd_power_on(void)
{
    if (!s_lcd_sd_power) {
        ESP_RETURN_ON_ERROR(output_pin(GPIO_NUM_9, 0), TAG, "LCD/SD power");
        vTaskDelay(pdMS_TO_TICKS(30));
        s_lcd_sd_power = true;
    }
    return ESP_OK;
}

static esp_err_t i2c_init_once(void)
{
    if (s_i2c) return ESP_OK;
    const i2c_master_bus_config_t config = {
        .i2c_port = I2C_NUM_0, .sda_io_num = GPIO_NUM_2, .scl_io_num = GPIO_NUM_1,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&config, &s_i2c);
}

esp_err_t board_init_display(void)
{
    if (s_display) return ESP_OK;
    if (s_display_attempted) return ESP_ERR_INVALID_STATE;
    s_display_attempted = true;
    ESP_RETURN_ON_ERROR(lcd_sd_power_on(), TAG, "display power");
    ESP_RETURN_ON_ERROR(i2c_init_once(), TAG, "touch I2C");
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t backlight = {
        .gpio_num = GPIO_NUM_44, .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");
    ESP_RETURN_ON_ERROR(ledc_channel_config(&backlight), TAG, "backlight");
    const spi_bus_config_t bus = {
        .sclk_io_num = GPIO_NUM_18, .data0_io_num = GPIO_NUM_46,
        .data1_io_num = GPIO_NUM_13, .data2_io_num = GPIO_NUM_11,
        .data3_io_num = GPIO_NUM_12, .max_transfer_sz = 360 * 40 * 2,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "LCD SPI");
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = GPIO_NUM_45, .cs_gpio_num = GPIO_NUM_14,
        .pclk_hz = 80 * 1000 * 1000, .lcd_cmd_bits = 32, .lcd_param_bits = 8,
        .spi_mode = 0, .trans_queue_depth = 10, .flags.quad_mode = true,
    };
    esp_lcd_panel_io_handle_t lcd_io;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                               &io_config, &lcd_io), TAG, "LCD IO");
    st77916_vendor_config_t vendor = {
        .init_cmds = disp_init_data,
        .init_cmds_size = sizeof(disp_init_data) / sizeof(disp_init_data[0]),
        .flags.use_qspi_interface = true,
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_RESET, .flags.reset_active_high = LCD_RESET_ACTIVE_HIGH,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    esp_lcd_panel_handle_t panel;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st77916(lcd_io, &panel_config, &panel), TAG, "LCD panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "LCD reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "LCD init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(panel, true), TAG, "LCD inversion");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(panel, false), TAG, "LCD orientation");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(panel, false, false), TAG, "LCD mirror");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "LCD on");
    lvgl_port_cfg_t lvgl_config = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_config), TAG, "LVGL init");
    const lvgl_port_display_cfg_t display_config = {
        .io_handle = lcd_io, .panel_handle = panel,
        .buffer_size = 360 * 20, .double_buffer = false, .hres = 360, .vres = 360,
        .color_format = LV_COLOR_FORMAT_RGB565,
        /* This panel's queued SPI path requires an internal DMA buffer.  A
           single 20-line canvas releases 43.2 KB for Wi-Fi without changing
           the panel transfer contract. */
        .flags = {.buff_dma = true, .swap_bytes = true},
    };
    lv_display_t *display = lvgl_port_add_disp(&display_config);
    ESP_RETURN_ON_FALSE(display, ESP_ERR_NO_MEM, TAG, "LVGL display");
    esp_lcd_panel_io_i2c_config_t touch_io_config = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    esp_lcd_panel_io_handle_t touch_io;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c, &touch_io_config, &touch_io), TAG, "touch IO");
    const esp_lcd_touch_config_t touch_config = {
        .x_max = 360, .y_max = 360, .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_10, .levels = {.reset = 0, .interrupt = 0},
    };
    esp_lcd_touch_handle_t touch;
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_cst816s(touch_io, &touch_config, &touch), TAG, "touch init");
    const lvgl_port_touch_cfg_t input_config = {.disp = display, .handle = touch};
    ESP_RETURN_ON_FALSE(lvgl_port_add_touch(&input_config), ESP_ERR_NO_MEM, TAG, "LVGL touch");
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 716), TAG, "brightness");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0), TAG, "brightness update");
    s_display = display;
#if CONFIG_MEMO_VOCAT16
    board_display_set_brightness(board_external_power_present() ? 70U : 15U);
#endif
    ESP_LOGI(TAG, "DISPLAY_READY board=%s 360x360 CST816S", board_name());
    return ESP_OK;
}

#if CONFIG_MEMO_VOCAT16
/* IDF 5.5.1 checks alignment and excludes PSRAM, but can still give SDMMC a
 * malloc buffer in RTC fast RAM. RTC RAM is CPU-readable, not SD DMA-capable.
 * Returning false selects the IDF sector bounce buffer for BOTH reads/writes.
 * Keep this adaptation in the project; do not patch the locked IDF checkout.
 * See evidence/vocat16-persistence-diagnosis-2026-09-16.md for the device A/B.
 */
static bool sd_buffer_dma_safe(int slot, const void *buffer, size_t bytes)
{
    if (!esp_ptr_dma_capable(buffer)) {
        if (buffer && !esp_ptr_external_ram(buffer)) {
            static unsigned reported;
            if (reported++ < 4U)
                ESP_LOGI(TAG, "SD_DMA_BOUNCE ptr=%p bytes=%u", buffer, (unsigned)bytes);
        }
        return false;
    }
    return sdmmc_host_check_buffer_alignment(slot, buffer, bytes);
}
#endif

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

esp_err_t board_sd_mount(void)
{
    if (s_card) return ESP_OK;
    ESP_RETURN_ON_ERROR(lcd_sd_power_on(), TAG, "SD power");
    const esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 6, .allocation_unit_size = 16 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.do_transaction = sd_presence_transaction;
#if CONFIG_MEMO_VOCAT16
    host.check_buffer_alignment = sd_buffer_dma_safe;
#endif
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = GPIO_NUM_16;
    slot.cmd = GPIO_NUM_38;
    slot.d0 = GPIO_NUM_17;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    s_sd_response_seen=false;s_sd_no_response=false;s_sd_probe_active=true;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(BOARD_SD_PATH, &host, &slot, &mount, &s_card);
    s_sd_probe_active=false;
    s_sd_no_response=(err==ESP_ERR_TIMEOUT && !s_sd_response_seen);
    ESP_LOGI(TAG,"SD_PRESENCE response=%d absent=%d",s_sd_response_seen,s_sd_no_response);
    ESP_LOGI(TAG, "SD_MOUNT result=%s format_if_mount_failed=false", esp_err_to_name(err));
    return err;
}

esp_err_t board_sd_unmount(void)
{
    if (!s_card) return ESP_OK;
    sdmmc_card_t *card = s_card;
    s_card = NULL; // IDF releases card before its final VFS cleanup result.
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
        .sample_rate = BOARD_AUDIO_RATE, .channel = 2, .bits_per_sample = 16,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1),
    };
}

static volatile uint32_t s_rx_overflows;
static volatile uint32_t s_rx_eofs;
static volatile uint32_t s_rx_last_eof_us;
static volatile uint32_t s_rx_last_overflow_us;
static volatile int32_t s_rx_irq_core = -1;
static bool rx_eof(i2s_chan_handle_t channel,i2s_event_data_t *event,void *ctx) {
    (void)channel;(void)event;(void)ctx;
    s_rx_last_eof_us = (uint32_t)esp_timer_get_time();
    s_rx_irq_core = xPortGetCoreID();
    s_rx_eofs++;
    return false;
}
static bool rx_overflow(i2s_chan_handle_t channel,i2s_event_data_t *event,void *ctx) {
    (void)channel;(void)event;(void)ctx;
    s_rx_last_overflow_us = (uint32_t)esp_timer_get_time();
    s_rx_overflows++;
    return false;
}
esp_err_t board_audio_init(void)
{
    if (s_audio_ready) return ESP_OK;
    /* Failed partial initialization is recovered by a reboot, never by creating
     * a second owner of the same I2S peripheral. */
    if (s_tx || s_rx || s_adc || s_dac) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(i2c_init_once(), TAG, "audio I2C");
    ESP_RETURN_ON_ERROR(output_pin(AUDIO_PA, 0), TAG, "PA off");
#if CONFIG_MEMO_BOARD_V1_2
    ESP_RETURN_ON_ERROR(output_pin(GPIO_NUM_48, 1), TAG, "codec power");
    vTaskDelay(pdMS_TO_TICKS(30));
#endif
    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.auto_clear = true;
    // 128 ms of DMA headroom for short SD/recognizer scheduling stalls.
    channel_config.dma_desc_num = 16;
    channel_config.dma_frame_num = 128;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_config, &s_tx, &s_rx), TAG, "I2S channels");
    const i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BOARD_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = GPIO_NUM_42, .bclk = GPIO_NUM_40, .ws = GPIO_NUM_39,
                     .dout = GPIO_NUM_41, .din = AUDIO_DIN},
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &config), TAG, "I2S TX init");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &config), TAG, "I2S RX init");
    const i2s_event_callbacks_t callbacks={.on_recv=rx_eof,.on_recv_q_ovf=rx_overflow};
    ESP_RETURN_ON_ERROR(i2s_channel_register_event_callback(s_rx,&callbacks,NULL),TAG,"RX overflow monitor");
    /* TX supplies common MCLK/BCLK even while recording. DMA auto-clear sends
     * silence and the ES8311/PA remain disabled outside playback. */
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "I2S clock enable");
    audio_codec_i2c_cfg_t control = {.port = I2C_NUM_0, .bus_handle = s_i2c, .addr = ES7210_CODEC_DEFAULT_ADDR};
    s_adc_ctrl = audio_codec_new_i2c_ctrl(&control);
    control.addr = ES8311_CODEC_DEFAULT_ADDR;
    s_dac_ctrl = audio_codec_new_i2c_ctrl(&control);
    s_codec_gpio = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(s_adc_ctrl && s_dac_ctrl && s_codec_gpio, ESP_ERR_NO_MEM, TAG, "codec controls");
    es7210_codec_cfg_t adc_config = {
        .ctrl_if = s_adc_ctrl, .master_mode = false,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2, .mclk_div = 256,
    };
    es8311_codec_cfg_t dac_config = {
        .ctrl_if = s_dac_ctrl, .gpio_if = s_codec_gpio,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC, .pa_pin = AUDIO_PA,
        .use_mclk = true, .mclk_div = 256,
    };
    s_adc = es7210_codec_new(&adc_config);
    s_dac = es8311_codec_new(&dac_config);
    ESP_RETURN_ON_FALSE(s_adc && s_dac, ESP_FAIL, TAG, "codec initialization");
    esp_codec_dev_sample_info_t format = audio_format();
    ESP_RETURN_ON_ERROR(codec_result(s_adc->set_fs(s_adc, &format), "ADC format"), TAG, "ADC format");
    ESP_RETURN_ON_ERROR(codec_result(s_dac->set_fs(s_dac, &format), "DAC format"), TAG, "DAC format");
    /* Codec construction initializes registers but does not set the driver's
     * enabled flag. ES8311 construction also enables PA. Establish a genuine
     * enabled->disabled transition so standby is effective. */
    ESP_RETURN_ON_ERROR(codec_result(s_adc->enable(s_adc, true), "ADC first enable"), TAG, "ADC first enable");
    ESP_RETURN_ON_ERROR(codec_result(s_dac->enable(s_dac, true), "DAC first enable"), TAG, "DAC first enable");
    ESP_RETURN_ON_ERROR(codec_result(s_dac->mute(s_dac, true), "DAC first mute"), TAG, "DAC first mute");
    ESP_RETURN_ON_ERROR(codec_result(s_adc->enable(s_adc, false), "ADC standby"), TAG, "ADC standby");
    ESP_RETURN_ON_ERROR(codec_result(s_dac->enable(s_dac, false), "DAC standby"), TAG, "DAC standby");
    ESP_RETURN_ON_ERROR(gpio_set_level(AUDIO_PA, 0), TAG, "PA standby");
    s_audio_ready = true;
    ESP_LOGI(TAG, "AUDIO_READY board=%s rate=16000 bits=16 mic_gain=%.1fdB MIC1=left MIC2=right; WAV uses MIC1; AEC MIC3 disabled",
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
static esp_err_t drain_rx_queue(void)
{
    /* While WakeNet listens, opening the WAV may briefly stop the audio
     * worker. Discard queued pre-recording DMA buffers before setting the
     * session overflow baseline; once recording is visible, any new overflow
     * still invalidates that file. See the same transition in Spotpear. */
    int16_t discard[128];
    size_t bytes = 0;
    unsigned buffers = 0;
    while (buffers < 64) {
        esp_err_t err = i2s_channel_read(s_rx, discard, sizeof(discard), &bytes, 0);
        if (err == ESP_ERR_TIMEOUT) break;
        if (err != ESP_OK) return err;
        buffers++;
    }
    ESP_LOGI(TAG, "RX_RESYNC drained=%u overflows=%" PRIu32, buffers, s_rx_overflows);
    return buffers < 64 ? ESP_OK : ESP_ERR_INVALID_STATE;
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
    ESP_RETURN_ON_ERROR(codec_result(s_adc->set_fs(s_adc, &format), "ADC format restore"), TAG, "ADC format restore");
    ESP_RETURN_ON_ERROR(codec_result(s_adc->enable(s_adc, true), "ADC enable"), TAG, "ADC enable");
    esp_err_t err = codec_result(s_adc->set_mic_gain(s_adc, RECORD_MIC_GAIN_DB), "mic gain");
    if (err == ESP_OK) err = i2s_channel_enable(s_rx);
    if (err == ESP_OK) err = drain_rx_queue();
    if (err != ESP_OK) {
        i2s_channel_disable(s_rx);
        s_adc->enable(s_adc, false);
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
    const size_t expected = samples * 2 * sizeof(int16_t);
    esp_err_t err = i2s_channel_read(s_rx, s_stereo, expected, &bytes, 200);
    if (err != ESP_OK) return err;
    if (bytes != expected) return ESP_ERR_INVALID_SIZE;
    /* Official schematic: MIC1/MIC2 go to the physical microphone board;
     * MIC3 is the DAC echo reference. Use MIC1, not an ambiguous ADC default. */
    for (size_t i = 0; i < samples; ++i) mono[i] = s_stereo[2 * i];
    return ESP_OK;
}

esp_err_t board_record_close(void)
{
    if (s_audio_state != AUDIO_RECORD) return ESP_ERR_INVALID_STATE;
    esp_err_t err = i2s_channel_disable(s_rx);
    esp_err_t codec_err = codec_result(s_adc->enable(s_adc, false), "ADC stop");
    s_audio_state = AUDIO_IDLE;
    if (err != ESP_OK || codec_err != ESP_OK) s_audio_ready = false;
    return err != ESP_OK ? err : codec_err;
}

esp_err_t board_play_open(void)
{
    if (!s_audio_ready || s_audio_state != AUDIO_IDLE) return ESP_ERR_INVALID_STATE;
    esp_codec_dev_sample_info_t format = audio_format();
    ESP_RETURN_ON_ERROR(codec_result(s_dac->set_fs(s_dac, &format), "DAC format restore"), TAG, "DAC format restore");
    ESP_RETURN_ON_ERROR(codec_result(s_dac->enable(s_dac, true), "DAC enable"), TAG, "DAC enable");
    esp_err_t err = codec_result(s_dac->set_vol(s_dac, -12.0f), "DAC volume");
    if (err == ESP_OK) err = codec_result(s_dac->mute(s_dac, false), "DAC unmute");
    if (err != ESP_OK) { s_dac->enable(s_dac, false); return err; }
    s_audio_state = AUDIO_PLAY;
    return ESP_OK;
}

esp_err_t board_play_write(const int16_t *mono, size_t samples)
{
    if (s_audio_state != AUDIO_PLAY) return ESP_ERR_INVALID_STATE;
    if (!mono || !samples || samples > BOARD_AUDIO_MAX_SAMPLES) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < samples; ++i) s_stereo[2 * i] = s_stereo[2 * i + 1] = mono[i];
    size_t bytes = 0;
    const size_t expected = samples * 2 * sizeof(int16_t);
    esp_err_t err = i2s_channel_write(s_tx, s_stereo, expected, &bytes, 200);
    if (err != ESP_OK) return err;
    return bytes == expected ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t board_play_close(void)
{
    if (s_audio_state != AUDIO_PLAY) return ESP_ERR_INVALID_STATE;
    /* Sixteen 128-frame DMA descriptors represent 128 ms at 16 kHz. Allow the last
     * accepted samples to reach the DAC; automatic DMA clearing prevents loops. */
    vTaskDelay(pdMS_TO_TICKS(140));
    esp_err_t err = codec_result(s_dac->mute(s_dac, true), "DAC mute");
    esp_err_t stop_err = codec_result(s_dac->enable(s_dac, false), "DAC stop");
    esp_err_t pa_err = gpio_set_level(AUDIO_PA, 0);
    s_audio_state = AUDIO_IDLE;
    if (err != ESP_OK || stop_err != ESP_OK || pa_err != ESP_OK) s_audio_ready = false;
    return err != ESP_OK ? err : (stop_err != ESP_OK ? stop_err : pa_err);
}

#if CONFIG_MEMO_VOCAT16
/* Keep the existing IDF 5 I2C owner. Apply the documented 700 mAh capacity in
 * gauge RAM at startup; no OTP writes or chemistry/calibration replacement. */
static i2c_master_dev_handle_t s_gauge;
static SemaphoreHandle_t s_gauge_lock;
static int64_t s_gauge_read_us;
static atomic_uint_fast16_t s_gauge_soc;
static atomic_uint_fast16_t s_gauge_mv;
static atomic_bool s_gauge_valid, s_gauge_external, s_gauge_rising;
static atomic_bool s_gauge_profile_ready;
static memo_gauge_profile_result_t s_gauge_profile_result;
static atomic_uint_fast32_t s_gauge_poll_ms = 5000;

static esp_err_t gauge_word(uint8_t reg, uint16_t *out)
{
    uint8_t data[2];
    esp_err_t err = i2c_master_transmit_receive(s_gauge, &reg, 1, data, 2, 100);
    esp_rom_delay_us(100);
    if (err == ESP_OK) *out = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    return err;
}

static int gauge_bus_read(void *context, uint8_t reg, uint8_t *data, size_t length)
{
    (void)context;
    esp_err_t err = i2c_master_transmit_receive(s_gauge, &reg, 1, data, length, 100);
    esp_rom_delay_us(100);
    ESP_LOGD(TAG, "GAUGE_CONFIG read reg=%02x size=%u result=%d", reg, (unsigned)length, err);
    if (err == ESP_OK) ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, length, ESP_LOG_DEBUG);
    return err == ESP_OK ? 0 : -1;
}
static int gauge_bus_write(void *context, uint8_t reg, const uint8_t *data, size_t length)
{
    (void)context;
    uint8_t packet[9];
    if (length > sizeof(packet) - 1) return -1;
    packet[0] = reg; memcpy(packet + 1, data, length);
    esp_err_t err = i2c_master_transmit(s_gauge, packet, length + 1, 100);
    esp_rom_delay_us(100);
    ESP_LOGD(TAG, "GAUGE_CONFIG write reg=%02x size=%u result=%d", reg, (unsigned)length, err);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, length, ESP_LOG_DEBUG);
    return err == ESP_OK ? 0 : -1;
}
static void gauge_bus_delay(void *context, unsigned ms)
{
    (void)context;
    vTaskDelay(pdMS_TO_TICKS(ms) > 0 ? pdMS_TO_TICKS(ms) : 1);
}

static void gauge_refresh(void)
{
    if (!s_gauge || !s_gauge_lock || xSemaphoreTake(s_gauge_lock, pdMS_TO_TICKS(250)) != pdTRUE)
        return;
    int64_t now = esp_timer_get_time();
    if (s_gauge_read_us && now - s_gauge_read_us < (int64_t)atomic_load(&s_gauge_poll_ms) * 1000LL) {
        xSemaphoreGive(s_gauge_lock);
        return;
    }
    s_gauge_read_us = now;
    uint16_t soc = 0, mv = 0, current = 0, average = 0, flags = 0, full = 0, design = 0, op = 0;
    esp_err_t err = gauge_word(0x2c, &soc);
    if (err == ESP_OK) err = gauge_word(0x08, &mv);
    if (err == ESP_OK) err = gauge_word(0x0c, &current);
    if (err == ESP_OK) err = gauge_word(0x14, &average);
    if (err == ESP_OK) err = gauge_word(0x0a, &flags);
    if (err == ESP_OK) err = gauge_word(0x12, &full);
    if (err == ESP_OK) err = gauge_word(0x3c, &design);
    if (err == ESP_OK) err = gauge_word(0x3a, &op);
    bool valid = err == ESP_OK && soc <= 100 && mv >= 2500 && mv <= 5000;
    if (valid) {
        bool rising = (int16_t)current > 30 || (int16_t)average > 30;
        atomic_store(&s_gauge_soc, soc);
        atomic_store(&s_gauge_mv, mv);
        bool profile_ready = memo_gauge_capacity_valid(design, full) && (op & 0x20) && !(op & 0x400);
        atomic_store(&s_gauge_profile_ready, profile_ready);
        atomic_store(&s_gauge_rising, rising);
        atomic_store(&s_gauge_external, !(flags & 1U) || rising);
        static int64_t last_log_us;
        if (!last_log_us || now - last_log_us >= 30000000LL) {
            ESP_LOGI(TAG, "GAUGE soc=%u mv=%u ma=%d flags=0x%04x external=%d fcc=%u design=%u profile=%d",
                     soc, mv, (int16_t)current, flags, !(flags & 1U) || rising,
                     full, design, profile_ready);
            last_log_us = now;
        }
    } else {
        ESP_LOGW(TAG, "GAUGE read unavailable: %s", esp_err_to_name(err));
    }
    atomic_store(&s_gauge_valid, valid);
    xSemaphoreGive(s_gauge_lock);
}

bool board_power_policy_supported(void) { return true; }
esp_err_t board_power_monitor_init(void)
{
    if (s_gauge) { gauge_refresh(); return atomic_load(&s_gauge_valid) ? ESP_OK : ESP_FAIL; }
    ESP_RETURN_ON_ERROR(i2c_init_once(), TAG, "gauge I2C");
    if (!s_gauge_lock) s_gauge_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_gauge_lock, ESP_ERR_NO_MEM, TAG, "gauge mutex");
    i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x55,
        .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &config, &s_gauge), TAG, "BQ27220");
    const memo_gauge_bus_t bus = {.read=gauge_bus_read, .write=gauge_bus_write, .delay_ms=gauge_bus_delay};
    int configured = memo_gauge_configure(&bus, &s_gauge_profile_result);
    ESP_LOGI(TAG, "GAUGE_PROFILE result=%d changed=%d old_fcc=%u old_design=%u target=700 rollback=%d/%d reinit=%d ram_backup=%u/%u",
             configured, s_gauge_profile_result.changed, s_gauge_profile_result.previous_full_mah,
             s_gauge_profile_result.previous_design_mah, s_gauge_profile_result.rollback_attempted,
             s_gauge_profile_result.rollback_verified, s_gauge_profile_result.reinitialized,
             s_gauge_profile_result.previous_ram_full_mah, s_gauge_profile_result.previous_ram_design_mah);
    gauge_refresh();
    return atomic_load(&s_gauge_valid) ? ESP_OK : ESP_FAIL;
}
bool board_external_power_present(void)
{
    gauge_refresh();
    /* Unknown power source must never trigger automatic shutdown. */
    return !atomic_load(&s_gauge_valid) || atomic_load(&s_gauge_external);
}
bool board_battery_raw(uint16_t *raw)
{
    gauge_refresh();
    if (raw) *raw = (uint16_t)atomic_load(&s_gauge_soc);
    return atomic_load(&s_gauge_valid) && atomic_load(&s_gauge_profile_ready);
}
bool board_battery_protection_raw(uint16_t *raw)
{
    gauge_refresh();
    if (raw) *raw = memo_gauge_protection_soc((uint16_t)atomic_load(&s_gauge_soc),
        atomic_load(&s_gauge_profile_ready), (uint16_t)atomic_load(&s_gauge_mv));
    return atomic_load(&s_gauge_valid);
}
esp_err_t board_battery_diagnostics(board_battery_diagnostics_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_gauge || !s_gauge_lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_gauge_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    board_battery_diagnostics_t result = {0};
    uint16_t current = 0, average = 0;
    esp_err_t err = gauge_word(0x2c, &result.soc_percent);
    if (err == ESP_OK) err = gauge_word(0x08, &result.voltage_mv);
    if (err == ESP_OK) err = gauge_word(0x0c, &current);
    if (err == ESP_OK) err = gauge_word(0x14, &average);
    if (err == ESP_OK) err = gauge_word(0x10, &result.remaining_mah);
    if (err == ESP_OK) err = gauge_word(0x12, &result.full_mah);
    if (err == ESP_OK) err = gauge_word(0x3c, &result.design_mah);
    if (err == ESP_OK) err = gauge_word(0x16, &result.time_to_empty_min);
    if (err == ESP_OK) err = gauge_word(0x2a, &result.cycle_count);
    if (err == ESP_OK) err = gauge_word(0x0a, &result.flags);
    if (err == ESP_OK) err = gauge_word(0x3a, &result.operation_status);
    xSemaphoreGive(s_gauge_lock);
    if (err != ESP_OK) return err;
    result.current_ma = (int16_t)current;
    result.average_ma = (int16_t)average;
    result.profile_ready = memo_gauge_capacity_valid(result.design_mah, result.full_mah) &&
        (result.operation_status & 0x20) && !(result.operation_status & 0x400);
    result.previous_full_mah = s_gauge_profile_result.previous_full_mah;
    result.previous_design_mah = s_gauge_profile_result.previous_design_mah;
    result.previous_ram_full_mah = s_gauge_profile_result.previous_ram_full_mah;
    result.previous_ram_design_mah = s_gauge_profile_result.previous_ram_design_mah;
    result.profile_changed = s_gauge_profile_result.changed;
    result.profile_reinitialized = s_gauge_profile_result.reinitialized;
    result.rollback_attempted = s_gauge_profile_result.rollback_attempted;
    result.rollback_verified = s_gauge_profile_result.rollback_verified;
    *out = result;
    return ESP_OK;
}
bool board_battery_rising(void) { gauge_refresh(); return atomic_load(&s_gauge_rising); }
void board_battery_set_poll_ms(uint32_t interval_ms)
{
    atomic_store(&s_gauge_poll_ms, interval_ms < 1000 ? 1000 : interval_ms);
}
esp_err_t board_display_set_brightness(unsigned percent)
{
    if (!s_display) return ESP_ERR_INVALID_STATE;
    if (percent > 100) percent = 100;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0,
                                     percent * 1023U / 100U), TAG, "backlight duty");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}
esp_err_t board_display_set_enabled(bool enabled)
{
    return board_display_set_brightness(enabled ? 15U : 0U);
}
#else
bool board_power_policy_supported(void) { return false; }
esp_err_t board_power_monitor_init(void) { return ESP_OK; }
bool board_external_power_present(void) { return true; }
bool board_battery_raw(uint16_t *raw) { (void)raw; return false; }
bool board_battery_protection_raw(uint16_t *raw) { return board_battery_raw(raw); }
esp_err_t board_battery_diagnostics(board_battery_diagnostics_t *out)
{ (void)out; return ESP_ERR_NOT_SUPPORTED; }
bool board_battery_rising(void) { return false; }
void board_battery_set_poll_ms(uint32_t interval_ms) { (void)interval_ms; }
esp_err_t board_display_set_brightness(unsigned percent) { (void)percent; return ESP_OK; }
esp_err_t board_display_set_enabled(bool enabled) { (void)enabled; return ESP_OK; }
#endif
board_sleep_wake_t board_light_sleep(uint64_t timeout_us)
{
    (void)timeout_us;
    return BOARD_SLEEP_WAKE_OTHER;
}
void board_power_off(void)
{
#if CONFIG_MEMO_VOCAT16
    /* The POWER key belongs to SAM8108, not a firmware GPIO. Timed deep sleep
     * retries after a charger is attached while keeping SD safely unmounted. */
    ESP_LOGW(TAG, "LOW_BATTERY_DEEP_SLEEP retry=60s");
    esp_sleep_enable_timer_wakeup(60ULL * 1000000ULL);
    esp_deep_sleep_start();
#else
    ESP_LOGE(TAG, "power off unsupported on %s", board_name());
#endif
}
