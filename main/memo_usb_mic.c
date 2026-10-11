#include "memo_usb_mic.h"
#include "memo_usb_pcm.h"
#include "memo_usb_gate.h"
#include "memo_usb_bus_watch.h"
#include "memo_usb_stdio.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_vfs.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/usb_dwc_struct.h"
#include "tusb.h"
#include "usb_device_uac.h"

#define CDC_RX_CAPACITY 1024U
#define CDC_TX_CAPACITY 16384U

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static memo_usb_status_t status = {.local_muted = true};
// These are task-only software queues, never DCD or DMA buffers. Keep them
// out of scarce internal RAM; TinyUSB's actual endpoint FIFOs stay internal.
static memo_usb_pcm_t *pcm_fifo;
static uint8_t *cdc_rx, *cdc_tx;
static size_t rx_head, rx_tail, rx_count, tx_head, tx_tail, tx_count;
static bool bulk_output;
static bool host_mute[2];
static memo_usb_bus_watch_t bus_watch;
static uint64_t bus_poll_us;

static bool gate_locked(void)
{
    return status.available &&
           memo_usb_gate_should_stream(status.connected, status.streaming,
                                       status.enabled, status.local_muted,
                                       status.host_muted);
}

static void clear_pcm_locked(void)
{
    if (pcm_fifo) memo_usb_pcm_clear(pcm_fifo);
}

static void update_gate_locked(void)
{
    if (!gate_locked()) clear_pcm_locked();
    uac_device_set_mic_output_enabled(gate_locked());
}

static esp_err_t audio_input_cb(uint8_t *buf, size_t len, size_t *bytes_read, void *ctx)
{
    (void)ctx;
    size_t samples = len / sizeof(int16_t);
    int16_t *out = (int16_t *)buf;
    portENTER_CRITICAL(&lock);
    bool enabled = gate_locked();
    memo_usb_pcm_pop(pcm_fifo, out, samples, enabled);
    portEXIT_CRITICAL(&lock);
    if (len & 1U) buf[len - 1] = 0;
    *bytes_read = len;
    return ESP_OK;
}

static void mic_stream_cb(bool active, void *ctx)
{
    (void)ctx;
    portENTER_CRITICAL(&lock);
    status.streaming = active;
    // A host may release the audio alternate setting while the user keeps
    // the USB microphone session open. Wait for a new stream, with no PCM.
    update_gate_locked();
    portEXIT_CRITICAL(&lock);
}

static void host_mute_cb(uint8_t channel, uint32_t mute, void *ctx)
{
    (void)ctx;
    if (channel > 1) return;
    portENTER_CRITICAL(&lock);
    host_mute[channel] = mute != 0;
    status.host_muted = host_mute[0] || host_mute[1];
    update_gate_locked();
    portEXIT_CRITICAL(&lock);
}

void tud_mount_cb(void)
{
    portENTER_CRITICAL(&lock);
    status.connected = false;
    bus_watch = (memo_usb_bus_watch_t){0};
    status.suspended = false;
    status.streaming = false;
    status.enabled = false;
    status.local_muted = true;
    host_mute[0] = host_mute[1] = false;
    status.host_muted = false;
    update_gate_locked();
    portEXIT_CRITICAL(&lock);
}

void tud_umount_cb(void)
{
    portENTER_CRITICAL(&lock);
    bus_watch = (memo_usb_bus_watch_t){0};
    status.connected = false;
    status.suspended = false;
    status.streaming = false;
    status.enabled = false;
    status.local_muted = true;
    host_mute[0] = host_mute[1] = false;
    status.host_muted = false;
    rx_head = rx_tail = rx_count = tx_head = tx_tail = tx_count = 0;
    update_gate_locked();
    portEXIT_CRITICAL(&lock);
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    portENTER_CRITICAL(&lock);
    bus_watch = (memo_usb_bus_watch_t){0};
    status.connected = false;
    status.suspended = true;
    status.local_muted = true;
    update_gate_locked();
    portEXIT_CRITICAL(&lock);
}

void tud_resume_cb(void)
{
    portENTER_CRITICAL(&lock);
    bus_watch = (memo_usb_bus_watch_t){0};
    status.connected = false;
    status.suspended = true;
    // Keep the session muted until the user explicitly unmutes or reopens it.
    status.local_muted = true;
    update_gate_locked();
    portEXIT_CRITICAL(&lock);
}

static void poll_host_bus(void)
{
    const uint64_t now = (uint64_t)esp_timer_get_time();
    if (now - bus_poll_us < 20000) return;
    bus_poll_us = now;
    // ESP32-S3 DSTS.SOFFN is the last received SOF number, not a free-running
    // timer. Read it without enabling 1000 extra SOF events per second.
    const uint16_t frame = USB_DWC.dsts_reg.soffn;
    const bool mounted = tud_mounted();
    portENTER_CRITICAL(&lock);
    const bool live = memo_usb_bus_watch_update(&bus_watch, mounted, frame, now);
    if (status.connected != live) {
        if (status.connected && !live) ++status.bus_watchdog_losses;
        status.connected = live;
        status.suspended = mounted && !live;
        // A short bus outage may retain the session, but never resumes audio
        // automatically. The worker ends a continuous suspension after 5s.
        status.local_muted = true;
        update_gate_locked();
    }
    portEXIT_CRITICAL(&lock);
}

void tud_cdc_rx_cb(uint8_t itf)
{
    uint8_t chunk[64];
    while (tud_cdc_n_available(itf)) {
        uint32_t n = tud_cdc_n_read(itf, chunk, sizeof(chunk));
        if (!n) break;
        portENTER_CRITICAL(&lock);
        for (uint32_t i = 0; i < n; ++i) {
            if (rx_count == CDC_RX_CAPACITY) break;
            cdc_rx[rx_head] = chunk[i];
            rx_head = (rx_head + 1) % CDC_RX_CAPACITY;
            ++rx_count;
        }
        portEXIT_CRITICAL(&lock);
    }
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    (void)rts;
    if (itf != 0 || dtr) return;
    // A terminal reopen must not receive stale log or partial WAV bytes.
    portENTER_CRITICAL(&lock);
    rx_head = rx_tail = rx_count = tx_head = tx_tail = tx_count = 0;
    portEXIT_CRITICAL(&lock);
}

// Called by the local UAC component on its TinyUSB task, never by the VFS writer.
void memo_usb_cdc_pump(void)
{
    poll_host_bus();
    if (!tud_cdc_n_connected(0)) return;
    size_t room = tud_cdc_n_write_available(0);
    if (!room) return;
    uint8_t chunk[128];
    if (room > sizeof(chunk)) room = sizeof(chunk);
    portENTER_CRITICAL(&lock);
    size_t n = room < tx_count ? room : tx_count;
    for (size_t i = 0; i < n; ++i) {
        chunk[i] = cdc_tx[tx_tail];
        tx_tail = (tx_tail + 1) % CDC_TX_CAPACITY;
    }
    tx_count -= n;
    portEXIT_CRITICAL(&lock);
    if (n) {
        tud_cdc_n_write(0, chunk, n);
        tud_cdc_n_write_flush(0);
    }
}

static int cdc_close(int fd) { (void)fd; return 0; }

static int cdc_fstat(int fd, struct stat *st)
{
    (void)fd;
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR;
    return 0;
}

static ssize_t cdc_read(int fd, void *buf, size_t len)
{
    (void)fd;
    portENTER_CRITICAL(&lock);
    size_t n = len < rx_count ? len : rx_count;
    for (size_t i = 0; i < n; ++i) {
        ((uint8_t *)buf)[i] = cdc_rx[rx_tail];
        rx_tail = (rx_tail + 1) % CDC_RX_CAPACITY;
    }
    rx_count -= n;
    portEXIT_CRITICAL(&lock);
    if (n) return (ssize_t)n;
    errno = EAGAIN;
    return -1;
}

static ssize_t cdc_write(int fd, const void *buf, size_t len)
{
    const uint8_t *src = buf;
    size_t written = 0;
    // Only an explicit idle-mode WAV export may wait for a slow host. Even
    // another task's stdout call could otherwise hold the shared FILE lock
    // and indirectly delay the 16 kHz capture worker.
    portENTER_CRITICAL(&lock);
    const bool exporting = bulk_output;
    portEXIT_CRITICAL(&lock);
    const int64_t deadline_us = esp_timer_get_time() + (exporting ? 200000 : 0);
    // Writes may be large (WAV dump). Backpressure preserves framing while CDC
    // is open; a detached terminal is treated as a discarded log sink.
    while (written < len) {
        if (!tud_cdc_n_connected(0)) {
            if (!exporting) return (ssize_t)len;
            portENTER_CRITICAL(&lock);
            status.cdc_tx_dropped_bytes += (uint32_t)(len - written);
            portEXIT_CRITICAL(&lock);
            errno = EIO;
            return written ? (ssize_t)written : -1;
        }
        portENTER_CRITICAL(&lock);
        size_t space = CDC_TX_CAPACITY - tx_count;
        size_t n = len - written;
        if (n > space) n = space;
        if (n > 256) n = 256;
        for (size_t i = 0; i < n; ++i) {
            cdc_tx[tx_head] = src[written + i];
            tx_head = (tx_head + 1) % CDC_TX_CAPACITY;
        }
        tx_count += n;
        portEXIT_CRITICAL(&lock);
        written += n;
        if (!n) {
            if (esp_timer_get_time() >= deadline_us) {
                portENTER_CRITICAL(&lock);
                status.cdc_tx_dropped_bytes += (uint32_t)(len - written);
                portEXIT_CRITICAL(&lock);
                errno = EAGAIN;
                return written ? (ssize_t)written : -1;
            }
            vTaskDelay(1);
        }
    }
    return (ssize_t)written;
}

static const esp_vfs_t cdc_vfs = {
    .flags = ESP_VFS_FLAG_DEFAULT,
    .close = cdc_close,
    .read = cdc_read,
    .write = cdc_write,
    .fstat = cdc_fstat,
};

esp_err_t memo_usb_init(void)
{
    if (pcm_fifo) return status.available ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (!stdin || !stdout || !stderr) return ESP_ERR_INVALID_STATE;
    const int stdio_fds[3] = {fileno(stdin), fileno(stdout), fileno(stderr)};
    int first_fd, end_fd;
    if (!memo_usb_stdio_range(stdio_fds, FD_SETSIZE, &first_fd, &end_fd))
        return ESP_ERR_INVALID_STATE;
    memo_usb_pcm_t *new_pcm = heap_caps_calloc(1, sizeof(*new_pcm),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *new_rx = heap_caps_malloc(CDC_RX_CAPACITY,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *new_tx = heap_caps_malloc(CDC_TX_CAPACITY,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!new_pcm || !new_rx || !new_tx) {
        heap_caps_free(new_pcm);
        heap_caps_free(new_rx);
        heap_caps_free(new_tx);
        return ESP_ERR_NO_MEM;
    }
    // Publish before TinyUSB starts: mount/CDC/audio callbacks can run during
    // uac_device_init(). No ISR reads these software queues.
    pcm_fifo = new_pcm;
    cdc_rx = new_rx;
    cdc_tx = new_tx;
    uac_device_config_t cfg = {
        .input_cb = audio_input_cb,
        .set_mute_cb = host_mute_cb,
        .mic_stream_cb = mic_stream_cb,
        .mic_itf_num = 1,
    };
    esp_err_t err = uac_device_init(&cfg);
    if (err != ESP_OK) return err;
    // The SDK console opens its primary/secondary descriptors before stdio.
    // Thus FILE descriptors need not be 0/1/2. Close exactly the validated
    // real streams so the console releases its underlying descriptors too;
    // a retained console writer would forward one buffer twice into CDC.
    if (fileno(stdin) != stdio_fds[0] || fileno(stdout) != stdio_fds[1] ||
        fileno(stderr) != stdio_fds[2]) return ESP_ERR_INVALID_STATE;
    for (unsigned i = 0; i < 3; ++i)
        if (close(stdio_fds[i]) != 0) return ESP_FAIL;
    err = esp_vfs_register_fd_range(&cdc_vfs, NULL, first_fd, end_fd);
    if (err != ESP_OK) return err;
    clearerr(stdin);
    clearerr(stdout);
    clearerr(stderr);
    portENTER_CRITICAL(&lock);
    status.available = true;
    status.local_muted = true;
    update_gate_locked();
    portEXIT_CRITICAL(&lock);
    return ESP_OK;
}

void memo_usb_get_status(memo_usb_status_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&lock);
    *out = status;
    out->dropped_samples = pcm_fifo ? pcm_fifo->dropped_samples : 0;
    out->underrun_samples = pcm_fifo ? pcm_fifo->underrun_samples : 0;
    out->bus_frame = bus_watch.frame;
    out->bus_age_ms = bus_watch.observed ?
        (uint32_t)(((uint64_t)esp_timer_get_time() - bus_watch.changed_us) / 1000) : UINT32_MAX;
    portEXIT_CRITICAL(&lock);
}

bool memo_usb_open(void)
{
    portENTER_CRITICAL(&lock);
    bool ready = status.available && status.connected;
    if (ready) {
        clear_pcm_locked();
        status.enabled = true;
        status.local_muted = false;
        update_gate_locked();
    }
    portEXIT_CRITICAL(&lock);
    return ready;
}

void memo_usb_close(void)
{
    portENTER_CRITICAL(&lock);
    status.enabled = false;
    status.local_muted = true;
    update_gate_locked();
    portEXIT_CRITICAL(&lock);
}

void memo_usb_set_muted(bool muted)
{
    portENTER_CRITICAL(&lock);
    status.local_muted = muted || !status.enabled;
    update_gate_locked();
    portEXIT_CRITICAL(&lock);
}

void memo_usb_submit(const int16_t *samples, size_t count)
{
    if (!samples || !count) return;
    portENTER_CRITICAL(&lock);
    if (gate_locked()) {
        memo_usb_pcm_push(pcm_fifo, samples, count);
    }
    portEXIT_CRITICAL(&lock);
}

void memo_usb_set_bulk_output(bool enabled)
{
    portENTER_CRITICAL(&lock);
    bulk_output = enabled;
    portEXIT_CRITICAL(&lock);
}

esp_err_t memo_usb_enter_bootloader(void)
{
    portENTER_CRITICAL(&lock);
    bool idle = status.available && status.connected && !status.enabled &&
                !status.suspended && !bulk_output;
    portEXIT_CRITICAL(&lock);
    if (!idle || !tud_cdc_n_connected(0)) return ESP_ERR_INVALID_STATE;
    if (esp_efuse_read_field_bit(ESP_EFUSE_DIS_FORCE_DOWNLOAD) ||
        esp_efuse_read_field_bit(ESP_EFUSE_DIS_DOWNLOAD_MODE)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    memo_usb_close();
    esp_err_t err = uac_device_prepare_rom_download();
    if (err != ESP_OK) return err;

    // The ROM's CDC/USB Serial-JTAG descriptors differ from this composite
    // UAC+CDC device. Deliberately do not set USBDC_PERSIST_ENA.
    REG_SET_BIT(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
    REG_CLR_BIT(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    return ESP_FAIL; // esp_restart() should not return
}
