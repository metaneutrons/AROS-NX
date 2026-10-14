/* Host tests for the production TinyUSB descriptor callbacks. */
#include "tusb.h"
#include "descriptors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { EXPECTED_CONFIG_LENGTH = 32 };

static tusb_speed_t selected_speed;

/* The descriptor implementation uses TinyUSB's real callback API; speed is
 * the only environmental input replaced by this host fixture. */
tusb_speed_t tud_speed_get(void)
{
    return selected_speed;
}

static void check(bool condition, const char *expression, int line)
{
    if (!condition)
    {
        fprintf(stderr, "test_descriptors:%d: check failed: %s\n", line,
                expression);
        exit(EXIT_FAILURE);
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

static uint16_t little_u16(const uint8_t *bytes)
{
    return (uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8);
}

static void check_vendor_configuration(const uint8_t *descriptor,
                                       uint16_t expected_packet_size,
                                       uint8_t expected_configuration_type)
{
    CHECK(descriptor != NULL);
    CHECK(descriptor[0] == 9);
    CHECK(descriptor[1] == expected_configuration_type);
    CHECK(little_u16(&descriptor[2]) == EXPECTED_CONFIG_LENGTH);
    CHECK(descriptor[4] == 1);

    CHECK(descriptor[9] == 9);
    CHECK(descriptor[10] == TUSB_DESC_INTERFACE);
    CHECK(descriptor[13] == 2);
    CHECK(descriptor[14] == TUSB_CLASS_VENDOR_SPECIFIC);

    CHECK(descriptor[18] == 7);
    CHECK(descriptor[19] == TUSB_DESC_ENDPOINT);
    CHECK(descriptor[20] == 0x01);
    CHECK(descriptor[21] == TUSB_XFER_BULK);
    CHECK(little_u16(&descriptor[22]) == expected_packet_size);

    CHECK(descriptor[25] == 7);
    CHECK(descriptor[26] == TUSB_DESC_ENDPOINT);
    CHECK(descriptor[27] == 0x81);
    CHECK(descriptor[28] == TUSB_XFER_BULK);
    CHECK(little_u16(&descriptor[29]) == expected_packet_size);
}

static void test_full_and_high_speed_configurations(void)
{
    selected_speed = TUSB_SPEED_FULL;
    check_vendor_configuration(tud_descriptor_configuration_cb(0), 64,
                               TUSB_DESC_CONFIGURATION);

    selected_speed = TUSB_SPEED_HIGH;
    check_vendor_configuration(tud_descriptor_configuration_cb(0), 512,
                               TUSB_DESC_CONFIGURATION);

    CHECK(tud_descriptor_configuration_cb(1) == NULL);
}

static void test_other_speed_descriptors(void)
{
    selected_speed = TUSB_SPEED_FULL;
    check_vendor_configuration(tud_descriptor_other_speed_configuration_cb(0),
                               512, TUSB_DESC_OTHER_SPEED_CONFIG);

    selected_speed = TUSB_SPEED_HIGH;
    check_vendor_configuration(tud_descriptor_other_speed_configuration_cb(0),
                               64, TUSB_DESC_OTHER_SPEED_CONFIG);

    CHECK(tud_descriptor_other_speed_configuration_cb(1) == NULL);
}

static void test_device_qualifier(void)
{
    const uint8_t *bytes = tud_descriptor_device_qualifier_cb();
    tusb_desc_device_qualifier_t qualifier;

    CHECK(bytes != NULL);
    memcpy(&qualifier, bytes, sizeof(qualifier));
    CHECK(qualifier.bLength == sizeof(qualifier));
    CHECK(qualifier.bDescriptorType == TUSB_DESC_DEVICE_QUALIFIER);
    CHECK(qualifier.bcdUSB == 0x0200);
    CHECK(qualifier.bDeviceClass == 0);
    CHECK(qualifier.bDeviceSubClass == 0);
    CHECK(qualifier.bDeviceProtocol == 0);
    CHECK(qualifier.bMaxPacketSize0 == 64);
    CHECK(qualifier.bNumConfigurations == 1);
    CHECK(qualifier.bReserved == 0);
}

static void test_string_indices_and_language(void)
{
    const uint16_t *language;
    const uint16_t *manufacturer;
    const uint16_t *product;

    language = tud_descriptor_string_cb(0, 0xffff);
    CHECK(language != NULL);
    CHECK(language[0] == (uint16_t)((TUSB_DESC_STRING << 8) | 4));
    CHECK(language[1] == 0x0409);

    manufacturer = tud_descriptor_string_cb(1, 0x0409);
    CHECK(manufacturer != NULL);
    CHECK(manufacturer[0] == (uint16_t)((TUSB_DESC_STRING << 8) | 10));
    CHECK(manufacturer[1] == 'A' && manufacturer[2] == 'R');
    CHECK(manufacturer[3] == 'O' && manufacturer[4] == 'S');

    product = tud_descriptor_string_cb(2, 0x0409);
    CHECK(product != NULL);
    CHECK(product[0] == (uint16_t)((TUSB_DESC_STRING << 8) | (2 + 2 * 21)));

    CHECK(tud_descriptor_string_cb(3, 0x0409) == NULL);
    CHECK(tud_descriptor_string_cb(1, 0x0407) == NULL);
    CHECK(tud_descriptor_string_cb(2, 0) == NULL);
}

static void test_identity_validation_is_atomic(void)
{
    uint8_t before[sizeof(tusb_desc_device_t)];
    tusb_desc_device_t device;

    CHECK(p4_usb_set_identity(0x1234, 0x5678));
    memcpy(before, tud_descriptor_device_cb(), sizeof(before));
    CHECK(!p4_usb_set_identity(0, 0x1111));
    CHECK(!p4_usb_set_identity(0x1111, 0));
    CHECK(!p4_usb_set_identity(0xffff, 0x1111));
    CHECK(!p4_usb_set_identity(0x1111, 0xffff));
    CHECK(!p4_usb_set_identity(0x303a, 0x1001));
    CHECK(memcmp(before, tud_descriptor_device_cb(), sizeof(before)) == 0);

    CHECK(p4_usb_set_identity(0x303a, 0x1002));
    memcpy(&device, tud_descriptor_device_cb(), sizeof(device));
    CHECK(device.idVendor == 0x303a);
    CHECK(device.idProduct == 0x1002);
}

int main(void)
{
    test_full_and_high_speed_configurations();
    test_other_speed_descriptors();
    test_device_qualifier();
    test_string_indices_and_language();
    test_identity_validation_is_atomic();
    puts("test_descriptors: pass");
    return EXIT_SUCCESS;
}
