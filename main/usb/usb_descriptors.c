/* Composite microphone/maintenance CDC descriptors. Audio topology is from
 * Espressif usb_device_uac 1.3.1 (Apache-2.0); TinyUSB CDC macro is MIT. */
#include <string.h>
#include <stdio.h>
#include "tusb.h"
#include "uac_descriptors.h"
#include "esp_mac.h"

enum {
    ITF_AUDIO_CONTROL,
    ITF_AUDIO_STREAMING_MIC,
    ITF_CDC_CONTROL,
    ITF_CDC_DATA,
    ITF_TOTAL,
};

enum {
    STR_LANG,
    STR_MANUFACTURER,
    STR_PRODUCT,
    STR_SERIAL,
    STR_AUDIO,
    STR_MIC,
    STR_CDC,
};

#define EP_AUDIO_IN 0x81
#define EP_CDC_NOTIFICATION 0x82
#define EP_CDC_OUT 0x01
#define EP_CDC_IN 0x83
#define TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_AUDIO_DEVICE_DESC_LEN + TUD_CDC_DESC_LEN)

static const tusb_desc_device_t device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,
    .idProduct = 0x8000,
    .bcdDevice = 0x0100,
    .iManufacturer = STR_MANUFACTURER,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1,
};

uint8_t const *tud_descriptor_device_cb(void) { return (const uint8_t *)&device_desc; }

static const uint8_t config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_TOTAL, 0, TOTAL_LEN, 0, 100),
    TUD_AUDIO_DESCRIPTOR(ITF_AUDIO_CONTROL, STR_AUDIO, 0, EP_AUDIO_IN, 0),
    TUD_CDC_DESCRIPTOR(ITF_CDC_CONTROL, STR_CDC, EP_CDC_NOTIFICATION, 8,
                       EP_CDC_OUT, EP_CDC_IN, 64),
};

_Static_assert(sizeof(config_desc) == TOTAL_LEN, "USB configuration length mismatch");
uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    return index == 0 ? config_desc : NULL;
}

static const char *const strings[] = {
    NULL, "Espressif", "VoiceMemo USB Microphone", NULL,
    "VoiceMemo Audio", "VoiceMemo Microphone", "VoiceMemo Maintenance",
};

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static uint16_t desc[32];
    if (index == STR_LANG) {
        desc[0] = (TUSB_DESC_STRING << 8) | 4;
        desc[1] = 0x0409;
        return desc;
    }
    if (index >= sizeof(strings) / sizeof(strings[0])) return NULL;
    char serial[13];
    if (index == STR_SERIAL) {
        uint8_t mac[6];
        if (esp_efuse_mac_get_default(mac) != ESP_OK) return NULL;
        snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    const char *s = index == STR_SERIAL ? serial : strings[index];
    size_t n = strlen(s);
    if (n > 31) n = 31;
    for (size_t i = 0; i < n; ++i) desc[i + 1] = (uint8_t)s[i];
    desc[0] = (TUSB_DESC_STRING << 8) | (uint8_t)(2 * n + 2);
    return desc;
}
