/* Copyright (c) 2026, The AROS Development Team. */
#include "tusb.h"
#include "descriptors.h"
#include <string.h>
static tusb_desc_device_t device = {
    .bLength = sizeof(tusb_desc_device_t), .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200, .bMaxPacketSize0 = 64,
    .bcdDevice = 0x0001, .iManufacturer = 1, .iProduct = 2,
    .bNumConfigurations = 1
};
enum { CONFIG_LENGTH = TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN };
static const uint8_t fs_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_LENGTH, 0, 100),
    TUD_VENDOR_DESCRIPTOR(0, 0, 0x01, 0x81, 64)
};
static const uint8_t hs_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_LENGTH, 0, 100),
    TUD_VENDOR_DESCRIPTOR(0, 0, 0x01, 0x81, 512)
};
static const tusb_desc_device_qualifier_t qualifier = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER, .bcdUSB = 0x0200,
    .bMaxPacketSize0 = 64, .bNumConfigurations = 1
};
int p4_usb_set_identity(uint16_t vid, uint16_t pid)
{
    if (!vid || vid == 0xffff || !pid || pid == 0xffff ||
        (vid == 0x303a && pid == 0x1001)) return 0;
    device.idVendor = vid; device.idProduct = pid;
    return 1;
}
uint8_t const *tud_descriptor_device_cb(void) { return (const uint8_t *)&device; }
uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    if (index) return NULL;
    return tud_speed_get() == TUSB_SPEED_HIGH ? hs_config : fs_config;
}
uint8_t const *tud_descriptor_device_qualifier_cb(void) { return (const uint8_t *)&qualifier; }
uint8_t const *tud_descriptor_other_speed_configuration_cb(uint8_t index)
{
    static uint8_t other[CONFIG_LENGTH];
    if (index) return NULL;
    memcpy(other, tud_speed_get() == TUSB_SPEED_HIGH ? fs_config : hs_config, CONFIG_LENGTH);
    other[1] = TUSB_DESC_OTHER_SPEED_CONFIG;
    return other;
}
uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    static uint16_t str[32];
    static const char *const strings[] = { "", "AROS", "P4 USB echo prototype" };
    size_t n, i;
    if (index == 0) { str[0] = (TUSB_DESC_STRING << 8) | 4; str[1] = 0x0409; return str; }
    if (langid != 0x0409 || index >= sizeof(strings)/sizeof(strings[0])) return NULL;
    n = strlen(strings[index]);
    if (n > 31) n = 31;
    str[0] = (TUSB_DESC_STRING << 8) | (uint16_t)(2 + n * 2);
    for (i = 0; i < n; ++i) str[i + 1] = (unsigned char)strings[index][i];
    return str;
}
