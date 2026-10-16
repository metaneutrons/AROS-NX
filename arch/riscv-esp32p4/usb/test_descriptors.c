/* Host tests for the production TinyUSB descriptor callbacks. */
#include "tusb.h"
#include "descriptors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    EXPECTED_CONFIG_LENGTH = TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN + TUD_CDC_DESC_LEN,
    EXPECTED_INTERFACE_COUNT = 3
};

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

static void check_descriptor(const uint8_t *bytes, size_t offset,
                             uint8_t length, uint8_t type)
{
    CHECK(offset + length <= EXPECTED_CONFIG_LENGTH);
    CHECK(bytes[offset] == length);
    CHECK(bytes[offset + 1] == type);
}

static void check_endpoint(const uint8_t *bytes, size_t offset,
                           uint8_t address, uint8_t transfer_type,
                           uint16_t packet_size, uint8_t interval)
{
    check_descriptor(bytes, offset, 7, TUSB_DESC_ENDPOINT);
    CHECK(bytes[offset + 2] == address);
    CHECK((bytes[offset + 3] & 0x03) == transfer_type);
    CHECK(little_u16(&bytes[offset + 4]) == packet_size);
    CHECK(bytes[offset + 6] == interval);
}

static void check_composite_configuration(const uint8_t *bytes,
                                          uint16_t packet_size,
                                          uint8_t configuration_type)
{
    size_t offset = 0;

    CHECK(bytes != NULL);
    check_descriptor(bytes, offset, 9, configuration_type);
    CHECK(little_u16(&bytes[2]) == EXPECTED_CONFIG_LENGTH);
    CHECK(bytes[4] == EXPECTED_INTERFACE_COUNT);
    CHECK(bytes[5] == 1);
    CHECK(bytes[6] == 0);
    CHECK(bytes[7] == 0x80);
    CHECK(bytes[8] == 50); /* 100 mA, in 2 mA units */
    offset += 9;

    /* Existing vendor-bulk ABI remains interface 0, OUT 01 / IN 81. */
    check_descriptor(bytes, offset, 9, TUSB_DESC_INTERFACE);
    CHECK(bytes[offset + 2] == P4_USB_VENDOR_INTERFACE);
    CHECK(bytes[offset + 3] == 0);
    CHECK(bytes[offset + 4] == 2);
    CHECK(bytes[offset + 5] == TUSB_CLASS_VENDOR_SPECIFIC);
    CHECK(bytes[offset + 6] == 0);
    CHECK(bytes[offset + 7] == 0);
    CHECK(bytes[offset + 8] == 0);
    offset += 9;
    check_endpoint(bytes, offset, P4_USB_VENDOR_EP_OUT, TUSB_XFER_BULK,
                   packet_size, 0);
    offset += 7;
    check_endpoint(bytes, offset, P4_USB_VENDOR_EP_IN, TUSB_XFER_BULK,
                   packet_size, 0);
    offset += 7;

    /* CDC IAD owns the following control/data pair, interfaces 1 and 2. */
    check_descriptor(bytes, offset, 8, TUSB_DESC_INTERFACE_ASSOCIATION);
    CHECK(bytes[offset + 2] == P4_USB_CDC_CONTROL_INTERFACE);
    CHECK(bytes[offset + 3] == 2);
    CHECK(bytes[offset + 4] == TUSB_CLASS_CDC);
    CHECK(bytes[offset + 5] == CDC_COMM_SUBCLASS_ABSTRACT_CONTROL_MODEL);
    CHECK(bytes[offset + 6] == CDC_COMM_PROTOCOL_NONE);
    CHECK(bytes[offset + 7] == 0);
    offset += 8;

    check_descriptor(bytes, offset, 9, TUSB_DESC_INTERFACE);
    CHECK(bytes[offset + 2] == P4_USB_CDC_CONTROL_INTERFACE);
    CHECK(bytes[offset + 3] == 0);
    CHECK(bytes[offset + 4] == 1);
    CHECK(bytes[offset + 5] == TUSB_CLASS_CDC);
    CHECK(bytes[offset + 6] == CDC_COMM_SUBCLASS_ABSTRACT_CONTROL_MODEL);
    CHECK(bytes[offset + 7] == CDC_COMM_PROTOCOL_NONE);
    CHECK(bytes[offset + 8] == 0);
    offset += 9;

    check_descriptor(bytes, offset, 5, TUSB_DESC_CS_INTERFACE);
    CHECK(bytes[offset + 2] == CDC_FUNC_DESC_HEADER);
    CHECK(little_u16(&bytes[offset + 3]) == 0x0120);
    offset += 5;

    check_descriptor(bytes, offset, 5, TUSB_DESC_CS_INTERFACE);
    CHECK(bytes[offset + 2] == CDC_FUNC_DESC_CALL_MANAGEMENT);
    CHECK(bytes[offset + 3] == 0);
    CHECK(bytes[offset + 4] == P4_USB_CDC_DATA_INTERFACE);
    offset += 5;

    check_descriptor(bytes, offset, 4, TUSB_DESC_CS_INTERFACE);
    CHECK(bytes[offset + 2] == CDC_FUNC_DESC_ABSTRACT_CONTROL_MANAGEMENT);
    CHECK(bytes[offset + 3] == 6);
    offset += 4;

    check_descriptor(bytes, offset, 5, TUSB_DESC_CS_INTERFACE);
    CHECK(bytes[offset + 2] == CDC_FUNC_DESC_UNION);
    CHECK(bytes[offset + 3] == P4_USB_CDC_CONTROL_INTERFACE);
    CHECK(bytes[offset + 4] == P4_USB_CDC_DATA_INTERFACE);
    offset += 5;

    check_endpoint(bytes, offset, P4_USB_CDC_EP_NOTIFY, TUSB_XFER_INTERRUPT,
                   8, 1);
    offset += 7;

    check_descriptor(bytes, offset, 9, TUSB_DESC_INTERFACE);
    CHECK(bytes[offset + 2] == P4_USB_CDC_DATA_INTERFACE);
    CHECK(bytes[offset + 3] == 0);
    CHECK(bytes[offset + 4] == 2);
    CHECK(bytes[offset + 5] == TUSB_CLASS_CDC_DATA);
    CHECK(bytes[offset + 6] == 0);
    CHECK(bytes[offset + 7] == 0);
    CHECK(bytes[offset + 8] == 0);
    offset += 9;

    check_endpoint(bytes, offset, P4_USB_CDC_EP_OUT, TUSB_XFER_BULK,
                   packet_size, 0);
    offset += 7;
    check_endpoint(bytes, offset, P4_USB_CDC_EP_IN, TUSB_XFER_BULK,
                   packet_size, 0);
    offset += 7;

    CHECK(offset == EXPECTED_CONFIG_LENGTH);
}

static void test_full_and_high_speed_configurations(void)
{
    selected_speed = TUSB_SPEED_FULL;
    check_composite_configuration(tud_descriptor_configuration_cb(0), 64,
                                  TUSB_DESC_CONFIGURATION);

    selected_speed = TUSB_SPEED_HIGH;
    check_composite_configuration(tud_descriptor_configuration_cb(0), 512,
                                  TUSB_DESC_CONFIGURATION);

    CHECK(tud_descriptor_configuration_cb(1) == NULL);
}

static void test_other_speed_descriptors(void)
{
    selected_speed = TUSB_SPEED_FULL;
    check_composite_configuration(tud_descriptor_other_speed_configuration_cb(0),
                                  512, TUSB_DESC_OTHER_SPEED_CONFIG);

    selected_speed = TUSB_SPEED_HIGH;
    check_composite_configuration(tud_descriptor_other_speed_configuration_cb(0),
                                  64, TUSB_DESC_OTHER_SPEED_CONFIG);

    CHECK(tud_descriptor_other_speed_configuration_cb(1) == NULL);
}

static void test_composite_device_and_qualifier_classes(void)
{
    tusb_desc_device_t device;
    tusb_desc_device_qualifier_t qualifier;

    memcpy(&device, tud_descriptor_device_cb(), sizeof(device));
    CHECK(device.bLength == sizeof(device));
    CHECK(device.bDescriptorType == TUSB_DESC_DEVICE);
    CHECK(device.bDeviceClass == 0xef);
    CHECK(device.bDeviceSubClass == 0x02);
    CHECK(device.bDeviceProtocol == 0x01);
    CHECK(device.bNumConfigurations == 1);

    memcpy(&qualifier, tud_descriptor_device_qualifier_cb(), sizeof(qualifier));
    CHECK(qualifier.bLength == sizeof(qualifier));
    CHECK(qualifier.bDescriptorType == TUSB_DESC_DEVICE_QUALIFIER);
    CHECK(qualifier.bcdUSB == 0x0200);
    CHECK(qualifier.bDeviceClass == 0xef);
    CHECK(qualifier.bDeviceSubClass == 0x02);
    CHECK(qualifier.bDeviceProtocol == 0x01);
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
    test_composite_device_and_qualifier_classes();
    test_string_indices_and_language();
    test_identity_validation_is_atomic();
    puts("test_descriptors: pass");
    return EXIT_SUCCESS;
}
