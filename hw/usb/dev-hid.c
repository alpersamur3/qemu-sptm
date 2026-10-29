/*
 * QEMU USB HID devices
 *
 * Copyright (c) 2005 Fabrice Bellard
 * Copyright (c) 2007 OpenMoko, Inc.  (andrew@openedhand.com)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "ui/console.h"
#include "hw/usb/usb.h"
#include "migration/vmstate.h"
#include "desc.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/bswap.h"
#include "hw/input/hid.h"
#include "hw/usb/hid.h"
#include "hw/core/qdev-properties.h"
#include "qom/object.h"
#include "trace.h"

struct USBHIDState {
    USBDevice dev;
    USBEndpoint *intr;
    HIDState hid;
    uint32_t usb_version;
    char *display;
    uint32_t head;
    bool is_touchscreen;  /* usb-touchscreen: digitizer descriptor + report rewrite */
    uint32_t ts_variant;  /* usb-touchscreen: TS_VARIANT_* bits */
    uint8_t ts_desc[96];  /* usb-touchscreen: report descriptor built for ts_variant */
    int ts_desc_len;
};

/* usb-touchscreen report layout variants (the "variant" property, a bitmask) */
#define TS_VARIANT_UNTOUCH  0x1  /* add Untouch: IOHIDEventDriver dispatches a collection */
#define TS_VARIANT_CONTACT  0x2  /* add Contact Identifier and Contact Count */

#define TYPE_USB_HID "usb-hid"
OBJECT_DECLARE_SIMPLE_TYPE(USBHIDState, USB_HID)

enum {
    STR_MANUFACTURER = 1,
    STR_PRODUCT_MOUSE,
    STR_PRODUCT_TABLET,
    STR_PRODUCT_KEYBOARD,
    STR_SERIAL_COMPAT,
    STR_CONFIG_MOUSE,
    STR_CONFIG_TABLET,
    STR_CONFIG_KEYBOARD,
    STR_SERIAL_MOUSE,
    STR_SERIAL_TABLET,
    STR_SERIAL_KEYBOARD,
    STR_PRODUCT_TOUCHSCREEN,
    STR_CONFIG_TOUCHSCREEN,
    STR_SERIAL_TOUCHSCREEN,
    STR_PRODUCT_CONSUMER,
    STR_CONFIG_CONSUMER,
    STR_SERIAL_CONSUMER,
};

static const USBDescStrings desc_strings = {
    [STR_MANUFACTURER]     = "QEMU",
    [STR_PRODUCT_MOUSE]    = "QEMU USB Mouse",
    [STR_PRODUCT_TABLET]   = "QEMU USB Tablet",
    [STR_PRODUCT_KEYBOARD] = "QEMU USB Keyboard",
    [STR_SERIAL_COMPAT]    = "42",
    [STR_CONFIG_MOUSE]     = "HID Mouse",
    [STR_CONFIG_TABLET]    = "HID Tablet",
    [STR_CONFIG_KEYBOARD]  = "HID Keyboard",
    [STR_SERIAL_MOUSE]     = "89126",
    [STR_SERIAL_TABLET]    = "28754",
    [STR_SERIAL_KEYBOARD]  = "68284",
    [STR_PRODUCT_TOUCHSCREEN] = "QEMU USB Touchscreen",
    [STR_CONFIG_TOUCHSCREEN]  = "HID Touchscreen",
    [STR_SERIAL_TOUCHSCREEN]  = "38512",
    [STR_PRODUCT_CONSUMER]    = "QEMU USB Consumer Control",
    [STR_CONFIG_CONSUMER]     = "HID Consumer",
    [STR_SERIAL_CONSUMER]     = "51937",
};

static const USBDescIface desc_iface_mouse = {
    .bInterfaceNumber              = 0,
    .bNumEndpoints                 = 1,
    .bInterfaceClass               = USB_CLASS_HID,
    .bInterfaceSubClass            = 0x01, /* boot */
    .bInterfaceProtocol            = 0x02,
    .ndesc                         = 1,
    .descs = (USBDescOther[]) {
        {
            /* HID descriptor */
            .data = (uint8_t[]) {
                0x09,          /*  u8  bLength */
                USB_DT_HID,    /*  u8  bDescriptorType */
                0x01, 0x00,    /*  u16 HID_class */
                0x00,          /*  u8  country_code */
                0x01,          /*  u8  num_descriptors */
                USB_DT_REPORT, /*  u8  type: Report */
                52, 0,         /*  u16 len */
            },
        },
    },
    .eps = (USBDescEndpoint[]) {
        {
            .bEndpointAddress      = USB_DIR_IN | 0x01,
            .bmAttributes          = USB_ENDPOINT_XFER_INT,
            .wMaxPacketSize        = 4,
            .bInterval             = 0x0a,
        },
    },
};

static const USBDescIface desc_iface_mouse2 = {
    .bInterfaceNumber              = 0,
    .bNumEndpoints                 = 1,
    .bInterfaceClass               = USB_CLASS_HID,
    .bInterfaceSubClass            = 0x01, /* boot */
    .bInterfaceProtocol            = 0x02,
    .ndesc                         = 1,
    .descs = (USBDescOther[]) {
        {
            /* HID descriptor */
            .data = (uint8_t[]) {
                0x09,          /*  u8  bLength */
                USB_DT_HID,    /*  u8  bDescriptorType */
                0x01, 0x00,    /*  u16 HID_class */
                0x00,          /*  u8  country_code */
                0x01,          /*  u8  num_descriptors */
                USB_DT_REPORT, /*  u8  type: Report */
                52, 0,         /*  u16 len */
            },
        },
    },
    .eps = (USBDescEndpoint[]) {
        {
            .bEndpointAddress      = USB_DIR_IN | 0x01,
            .bmAttributes          = USB_ENDPOINT_XFER_INT,
            .wMaxPacketSize        = 4,
            .bInterval             = 7, /* 2 ^ (8-1) * 125 usecs = 8 ms */
        },
    },
};

static const USBDescIface desc_iface_tablet = {
    .bInterfaceNumber              = 0,
    .bNumEndpoints                 = 1,
    .bInterfaceClass               = USB_CLASS_HID,
    .bInterfaceProtocol            = 0x00,
    .ndesc                         = 1,
    .descs = (USBDescOther[]) {
        {
            /* HID descriptor */
            .data = (uint8_t[]) {
                0x09,          /*  u8  bLength */
                USB_DT_HID,    /*  u8  bDescriptorType */
                0x01, 0x00,    /*  u16 HID_class */
                0x00,          /*  u8  country_code */
                0x01,          /*  u8  num_descriptors */
                USB_DT_REPORT, /*  u8  type: Report */
                74, 0,         /*  u16 len */
            },
        },
    },
    .eps = (USBDescEndpoint[]) {
        {
            .bEndpointAddress      = USB_DIR_IN | 0x01,
            .bmAttributes          = USB_ENDPOINT_XFER_INT,
            .wMaxPacketSize        = 8,
            .bInterval             = 0x0a,
        },
    },
};

static const USBDescIface desc_iface_tablet2 = {
    .bInterfaceNumber              = 0,
    .bNumEndpoints                 = 1,
    .bInterfaceClass               = USB_CLASS_HID,
    .bInterfaceProtocol            = 0x00,
    .ndesc                         = 1,
    .descs = (USBDescOther[]) {
        {
            /* HID descriptor */
            .data = (uint8_t[]) {
                0x09,          /*  u8  bLength */
                USB_DT_HID,    /*  u8  bDescriptorType */
                0x01, 0x00,    /*  u16 HID_class */
                0x00,          /*  u8  country_code */
                0x01,          /*  u8  num_descriptors */
                USB_DT_REPORT, /*  u8  type: Report */
                74, 0,         /*  u16 len */
            },
        },
    },
    .eps = (USBDescEndpoint[]) {
        {
            .bEndpointAddress      = USB_DIR_IN | 0x01,
            .bmAttributes          = USB_ENDPOINT_XFER_INT,
            .wMaxPacketSize        = 8,
            .bInterval             = 4, /* 2 ^ (4-1) * 125 usecs = 1 ms */
        },
    },
};

/*
 * HID class descriptor of usb-touchscreen. The report descriptor length
 * (bytes 7-8) depends on the variant and is filled in at realize.
 */
static uint8_t touchscreen_hid_class_desc[] = {
    0x09,          /*  u8  bLength */
    USB_DT_HID,    /*  u8  bDescriptorType */
    0x01, 0x00,    /*  u16 HID_class */
    0x00,          /*  u8  country_code */
    0x01,          /*  u8  num_descriptors */
    USB_DT_REPORT, /*  u8  type: Report */
    55, 0,         /*  u16 len (digitizer report descriptor) */
};

static const USBDescIface desc_iface_touchscreen = {
    .bInterfaceNumber              = 0,
    .bNumEndpoints                 = 1,
    .bInterfaceClass               = USB_CLASS_HID,
    .bInterfaceProtocol            = 0x00,
    .ndesc                         = 1,
    .descs = (USBDescOther[]) {
        {
            /* HID descriptor */
            .data = touchscreen_hid_class_desc,
        },
    },
    .eps = (USBDescEndpoint[]) {
        {
            .bEndpointAddress      = USB_DIR_IN | 0x01,
            .bmAttributes          = USB_ENDPOINT_XFER_INT,
            .wMaxPacketSize        = 8,
            .bInterval             = 0x0a,
        },
    },
};

static const USBDescIface desc_iface_touchscreen2 = {
    .bInterfaceNumber              = 0,
    .bNumEndpoints                 = 1,
    .bInterfaceClass               = USB_CLASS_HID,
    .bInterfaceProtocol            = 0x00,
    .ndesc                         = 1,
    .descs = (USBDescOther[]) {
        {
            /* HID descriptor */
            .data = touchscreen_hid_class_desc,
        },
    },
    .eps = (USBDescEndpoint[]) {
        {
            .bEndpointAddress      = USB_DIR_IN | 0x01,
            .bmAttributes          = USB_ENDPOINT_XFER_INT,
            .wMaxPacketSize        = 8,
            .bInterval             = 4, /* 2 ^ (4-1) * 125 usecs = 1 ms */
        },
    },
};

static const USBDescIface desc_iface_keyboard = {
    .bInterfaceNumber              = 0,
    .bNumEndpoints                 = 1,
    .bInterfaceClass               = USB_CLASS_HID,
    .bInterfaceSubClass            = 0x01, /* boot */
    .bInterfaceProtocol            = 0x01, /* keyboard */
    .ndesc                         = 1,
    .descs = (USBDescOther[]) {
        {
            /* HID descriptor */
            .data = (uint8_t[]) {
                0x09,          /*  u8  bLength */
                USB_DT_HID,    /*  u8  bDescriptorType */
                0x11, 0x01,    /*  u16 HID_class */
                0x00,          /*  u8  country_code */
                0x01,          /*  u8  num_descriptors */
                USB_DT_REPORT, /*  u8  type: Report */
                0x3f, 0,       /*  u16 len */
            },
        },
    },
    .eps = (USBDescEndpoint[]) {
        {
            .bEndpointAddress      = USB_DIR_IN | 0x01,
            .bmAttributes          = USB_ENDPOINT_XFER_INT,
            .wMaxPacketSize        = 8,
            .bInterval             = 0x0a,
        },
    },
};

static const USBDescIface desc_iface_keyboard2 = {
    .bInterfaceNumber              = 0,
    .bNumEndpoints                 = 1,
    .bInterfaceClass               = USB_CLASS_HID,
    .bInterfaceSubClass            = 0x01, /* boot */
    .bInterfaceProtocol            = 0x01, /* keyboard */
    .ndesc                         = 1,
    .descs = (USBDescOther[]) {
        {
            /* HID descriptor */
            .data = (uint8_t[]) {
                0x09,          /*  u8  bLength */
                USB_DT_HID,    /*  u8  bDescriptorType */
                0x11, 0x01,    /*  u16 HID_class */
                0x00,          /*  u8  country_code */
                0x01,          /*  u8  num_descriptors */
                USB_DT_REPORT, /*  u8  type: Report */
                0x3f, 0,       /*  u16 len */
            },
        },
    },
    .eps = (USBDescEndpoint[]) {
        {
            .bEndpointAddress      = USB_DIR_IN | 0x01,
            .bmAttributes          = USB_ENDPOINT_XFER_INT,
            .wMaxPacketSize        = 8,
            .bInterval             = 7, /* 2 ^ (8-1) * 125 usecs = 8 ms */
        },
    },
};

static const USBDescDevice desc_device_mouse = {
    .bcdUSB                        = 0x0100,
    .bMaxPacketSize0               = 8,
    .bNumConfigurations            = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces        = 1,
            .bConfigurationValue   = 1,
            .iConfiguration        = STR_CONFIG_MOUSE,
            .bmAttributes          = USB_CFG_ATT_ONE | USB_CFG_ATT_WAKEUP,
            .bMaxPower             = 50,
            .nif = 1,
            .ifs = &desc_iface_mouse,
        },
    },
};

static const USBDescDevice desc_device_mouse2 = {
    .bcdUSB                        = 0x0200,
    .bMaxPacketSize0               = 64,
    .bNumConfigurations            = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces        = 1,
            .bConfigurationValue   = 1,
            .iConfiguration        = STR_CONFIG_MOUSE,
            .bmAttributes          = USB_CFG_ATT_ONE | USB_CFG_ATT_WAKEUP,
            .bMaxPower             = 50,
            .nif = 1,
            .ifs = &desc_iface_mouse2,
        },
    },
};

static const USBDescDevice desc_device_tablet = {
    .bcdUSB                        = 0x0100,
    .bMaxPacketSize0               = 8,
    .bNumConfigurations            = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces        = 1,
            .bConfigurationValue   = 1,
            .iConfiguration        = STR_CONFIG_TABLET,
            .bmAttributes          = USB_CFG_ATT_ONE | USB_CFG_ATT_WAKEUP,
            .bMaxPower             = 50,
            .nif = 1,
            .ifs = &desc_iface_tablet,
        },
    },
};

static const USBDescDevice desc_device_tablet2 = {
    .bcdUSB                        = 0x0200,
    .bMaxPacketSize0               = 64,
    .bNumConfigurations            = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces        = 1,
            .bConfigurationValue   = 1,
            .iConfiguration        = STR_CONFIG_TABLET,
            .bmAttributes          = USB_CFG_ATT_ONE | USB_CFG_ATT_WAKEUP,
            .bMaxPower             = 50,
            .nif = 1,
            .ifs = &desc_iface_tablet2,
        },
    },
};

static const USBDescDevice desc_device_touchscreen = {
    .bcdUSB                        = 0x0100,
    .bMaxPacketSize0               = 8,
    .bNumConfigurations            = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces        = 1,
            .bConfigurationValue   = 1,
            .iConfiguration        = STR_CONFIG_TOUCHSCREEN,
            .bmAttributes          = USB_CFG_ATT_ONE | USB_CFG_ATT_WAKEUP,
            .bMaxPower             = 50,
            .nif = 1,
            .ifs = &desc_iface_touchscreen,
        },
    },
};

static const USBDescDevice desc_device_touchscreen2 = {
    .bcdUSB                        = 0x0200,
    .bMaxPacketSize0               = 64,
    .bNumConfigurations            = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces        = 1,
            .bConfigurationValue   = 1,
            .iConfiguration        = STR_CONFIG_TOUCHSCREEN,
            .bmAttributes          = USB_CFG_ATT_ONE | USB_CFG_ATT_WAKEUP,
            .bMaxPower             = 50,
            .nif = 1,
            .ifs = &desc_iface_touchscreen2,
        },
    },
};

static const USBDescDevice desc_device_keyboard = {
    .bcdUSB                        = 0x0100,
    .bMaxPacketSize0               = 8,
    .bNumConfigurations            = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces        = 1,
            .bConfigurationValue   = 1,
            .iConfiguration        = STR_CONFIG_KEYBOARD,
            .bmAttributes          = USB_CFG_ATT_ONE | USB_CFG_ATT_WAKEUP,
            .bMaxPower             = 50,
            .nif = 1,
            .ifs = &desc_iface_keyboard,
        },
    },
};

static const USBDescDevice desc_device_keyboard2 = {
    .bcdUSB                        = 0x0200,
    .bMaxPacketSize0               = 64,
    .bNumConfigurations            = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces        = 1,
            .bConfigurationValue   = 1,
            .iConfiguration        = STR_CONFIG_KEYBOARD,
            .bmAttributes          = USB_CFG_ATT_ONE | USB_CFG_ATT_WAKEUP,
            .bMaxPower             = 50,
            .nif = 1,
            .ifs = &desc_iface_keyboard2,
        },
    },
};

static const USBDescMSOS desc_msos_suspend = {
    .SelectiveSuspendEnabled = true,
};

static const USBDesc desc_mouse = {
    .id = {
        .idVendor          = 0x0627,
        .idProduct         = 0x0001,
        .bcdDevice         = 0,
        .iManufacturer     = STR_MANUFACTURER,
        .iProduct          = STR_PRODUCT_MOUSE,
        .iSerialNumber     = STR_SERIAL_MOUSE,
    },
    .full = &desc_device_mouse,
    .str  = desc_strings,
    .msos = &desc_msos_suspend,
};

static const USBDesc desc_mouse2 = {
    .id = {
        .idVendor          = 0x0627,
        .idProduct         = 0x0001,
        .bcdDevice         = 0,
        .iManufacturer     = STR_MANUFACTURER,
        .iProduct          = STR_PRODUCT_MOUSE,
        .iSerialNumber     = STR_SERIAL_MOUSE,
    },
    .full = &desc_device_mouse,
    .high = &desc_device_mouse2,
    .str  = desc_strings,
    .msos = &desc_msos_suspend,
};

static const USBDesc desc_tablet = {
    .id = {
        .idVendor          = 0x0627,
        .idProduct         = 0x0001,
        .bcdDevice         = 0,
        .iManufacturer     = STR_MANUFACTURER,
        .iProduct          = STR_PRODUCT_TABLET,
        .iSerialNumber     = STR_SERIAL_TABLET,
    },
    .full = &desc_device_tablet,
    .str  = desc_strings,
    .msos = &desc_msos_suspend,
};

static const USBDesc desc_tablet2 = {
    .id = {
        .idVendor          = 0x0627,
        .idProduct         = 0x0001,
        .bcdDevice         = 0,
        .iManufacturer     = STR_MANUFACTURER,
        .iProduct          = STR_PRODUCT_TABLET,
        .iSerialNumber     = STR_SERIAL_TABLET,
    },
    .full = &desc_device_tablet,
    .high = &desc_device_tablet2,
    .str  = desc_strings,
    .msos = &desc_msos_suspend,
};

static const USBDesc desc_touchscreen = {
    .id = {
        .idVendor          = 0x0627,
        .idProduct         = 0x0001,
        .bcdDevice         = 0,
        .iManufacturer     = STR_MANUFACTURER,
        .iProduct          = STR_PRODUCT_TOUCHSCREEN,
        .iSerialNumber     = STR_SERIAL_TOUCHSCREEN,
    },
    .full = &desc_device_touchscreen,
    .str  = desc_strings,
    .msos = &desc_msos_suspend,
};

static const USBDesc desc_touchscreen2 = {
    .id = {
        .idVendor          = 0x0627,
        .idProduct         = 0x0001,
        .bcdDevice         = 0,
        .iManufacturer     = STR_MANUFACTURER,
        .iProduct          = STR_PRODUCT_TOUCHSCREEN,
        .iSerialNumber     = STR_SERIAL_TOUCHSCREEN,
    },
    .full = &desc_device_touchscreen,
    .high = &desc_device_touchscreen2,
    .str  = desc_strings,
    .msos = &desc_msos_suspend,
};

static const USBDesc desc_keyboard = {
    .id = {
        .idVendor          = 0x0627,
        .idProduct         = 0x0001,
        .bcdDevice         = 0,
        .iManufacturer     = STR_MANUFACTURER,
        .iProduct          = STR_PRODUCT_KEYBOARD,
        .iSerialNumber     = STR_SERIAL_KEYBOARD,
    },
    .full = &desc_device_keyboard,
    .str  = desc_strings,
    .msos = &desc_msos_suspend,
};

static const USBDesc desc_keyboard2 = {
    .id = {
        .idVendor          = 0x0627,
        .idProduct         = 0x0001,
        .bcdDevice         = 0,
        .iManufacturer     = STR_MANUFACTURER,
        .iProduct          = STR_PRODUCT_KEYBOARD,
        .iSerialNumber     = STR_SERIAL_KEYBOARD,
    },
    .full = &desc_device_keyboard,
    .high = &desc_device_keyboard2,
    .str  = desc_strings,
    .msos = &desc_msos_suspend,
};

static const uint8_t qemu_mouse_hid_report_descriptor[] = {
    0x05, 0x01,		/* Usage Page (Generic Desktop) */
    0x09, 0x02,		/* Usage (Mouse) */
    0xa1, 0x01,		/* Collection (Application) */
    0x09, 0x01,		/*   Usage (Pointer) */
    0xa1, 0x00,		/*   Collection (Physical) */
    0x05, 0x09,		/*     Usage Page (Button) */
    0x19, 0x01,		/*     Usage Minimum (1) */
    0x29, 0x05,		/*     Usage Maximum (5) */
    0x15, 0x00,		/*     Logical Minimum (0) */
    0x25, 0x01,		/*     Logical Maximum (1) */
    0x95, 0x05,		/*     Report Count (5) */
    0x75, 0x01,		/*     Report Size (1) */
    0x81, 0x02,		/*     Input (Data, Variable, Absolute) */
    0x95, 0x01,		/*     Report Count (1) */
    0x75, 0x03,		/*     Report Size (3) */
    0x81, 0x01,		/*     Input (Constant) */
    0x05, 0x01,		/*     Usage Page (Generic Desktop) */
    0x09, 0x30,		/*     Usage (X) */
    0x09, 0x31,		/*     Usage (Y) */
    0x09, 0x38,		/*     Usage (Wheel) */
    0x15, 0x81,		/*     Logical Minimum (-0x7f) */
    0x25, 0x7f,		/*     Logical Maximum (0x7f) */
    0x75, 0x08,		/*     Report Size (8) */
    0x95, 0x03,		/*     Report Count (3) */
    0x81, 0x06,		/*     Input (Data, Variable, Relative) */
    0xc0,		/*   End Collection */
    0xc0,		/* End Collection */
};

static const uint8_t qemu_tablet_hid_report_descriptor[] = {
    0x05, 0x01,		/* Usage Page (Generic Desktop) */
    0x09, 0x02,		/* Usage (Mouse) */
    0xa1, 0x01,		/* Collection (Application) */
    0x09, 0x01,		/*   Usage (Pointer) */
    0xa1, 0x00,		/*   Collection (Physical) */
    0x05, 0x09,		/*     Usage Page (Button) */
    0x19, 0x01,		/*     Usage Minimum (1) */
    0x29, 0x05,		/*     Usage Maximum (5) */
    0x15, 0x00,		/*     Logical Minimum (0) */
    0x25, 0x01,		/*     Logical Maximum (1) */
    0x95, 0x05,		/*     Report Count (5) */
    0x75, 0x01,		/*     Report Size (1) */
    0x81, 0x02,		/*     Input (Data, Variable, Absolute) */
    0x95, 0x01,		/*     Report Count (1) */
    0x75, 0x03,		/*     Report Size (3) */
    0x81, 0x01,		/*     Input (Constant) */
    0x05, 0x01,		/*     Usage Page (Generic Desktop) */
    0x09, 0x30,		/*     Usage (X) */
    0x09, 0x31,		/*     Usage (Y) */
    0x15, 0x00,		/*     Logical Minimum (0) */
    0x26, 0xff, 0x7f,	/*     Logical Maximum (0x7fff) */
    0x35, 0x00,		/*     Physical Minimum (0) */
    0x46, 0xff, 0x7f,	/*     Physical Maximum (0x7fff) */
    0x75, 0x10,		/*     Report Size (16) */
    0x95, 0x02,		/*     Report Count (2) */
    0x81, 0x02,		/*     Input (Data, Variable, Absolute) */
    0x05, 0x01,		/*     Usage Page (Generic Desktop) */
    0x09, 0x38,		/*     Usage (Wheel) */
    0x15, 0x81,		/*     Logical Minimum (-0x7f) */
    0x25, 0x7f,		/*     Logical Maximum (0x7f) */
    0x35, 0x00,		/*     Physical Minimum (same as logical) */
    0x45, 0x00,		/*     Physical Maximum (same as logical) */
    0x75, 0x08,		/*     Report Size (8) */
    0x95, 0x01,		/*     Report Count (1) */
    0x81, 0x06,		/*     Input (Data, Variable, Relative) */
    0xc0,		/*   End Collection */
    0xc0,		/* End Collection */
};

/*
 * Touchscreen (single-finger digitizer), described as a Digitizer Touch Screen
 * so iOS 26's AppleUserHIDDrivers dext routes it through the standard
 * digitizer parser. Application usage TouchScreen (0x0D:0x04) is what makes
 * IOHIDEventService set DisplayIntegrated; Touch Valid (0x47) must stay 1,
 * on release too, or IOHIDEventDriver turns the transducer from Finger into
 * Hand. The base report is the tablet's 6 bytes with byte 0 rewritten:
 *   [Tip|InRange|TouchValid][Xlo][Xhi][Ylo][Yhi][pad]
 * TS_VARIANT_UNTOUCH adds an Untouch bit (always 0) after Touch Valid; its
 * presence makes IOHIDEventDriver dispatch a collection event with the finger
 * as its child. TS_VARIANT_CONTACT adds a Contact Identifier byte (0) after
 * the flags and replaces the pad byte with a Contact Count (1).
 */
static int usb_touchscreen_build_desc(uint8_t *d, uint32_t variant)
{
    bool untouch = variant & TS_VARIANT_UNTOUCH;
    bool contact = variant & TS_VARIANT_CONTACT;
    int n = 0;

#define B(...) do { \
        const uint8_t b_[] = { __VA_ARGS__ }; \
        memcpy(d + n, b_, sizeof(b_)); \
        n += sizeof(b_); \
    } while (0)
    B(0x05, 0x0d);              /* Usage Page (Digitizer) */
    B(0x09, 0x04);              /* Usage (Touch Screen) */
    B(0xa1, 0x01);              /* Collection (Application) */
    B(0x09, 0x22);              /*   Usage (Finger) */
    B(0xa1, 0x02);              /*   Collection (Logical) */
    B(0x09, 0x42);              /*     Usage (Tip Switch) */
    B(0x09, 0x32);              /*     Usage (In Range) */
    B(0x09, 0x47);              /*     Usage (Touch Valid) */
    if (untouch) {
        B(0x09, 0x34);          /*     Usage (Untouch) */
    }
    B(0x15, 0x00);              /*     Logical Minimum (0) */
    B(0x25, 0x01);              /*     Logical Maximum (1) */
    B(0x75, 0x01);              /*     Report Size (1) */
    B(0x95, untouch ? 4 : 3);   /*     Report Count */
    B(0x81, 0x02);              /*     Input (Data, Variable, Absolute) */
    B(0x95, untouch ? 4 : 5);   /*     Report Count */
    B(0x81, 0x03);              /*     Input (Constant) -- padding to 8 bits */
    if (contact) {
        B(0x09, 0x51);          /*     Usage (Contact Identifier) */
        B(0x25, 0x7f);          /*     Logical Maximum (127) */
        B(0x75, 0x08);          /*     Report Size (8) */
        B(0x95, 0x01);          /*     Report Count (1) */
        B(0x81, 0x02);          /*     Input (Data, Variable, Absolute) */
    }
    B(0x05, 0x01);              /*     Usage Page (Generic Desktop) */
    B(0x09, 0x30);              /*     Usage (X) */
    B(0x09, 0x31);              /*     Usage (Y) */
    B(0x15, 0x00);              /*     Logical Minimum (0) */
    B(0x26, 0xff, 0x7f);        /*     Logical Maximum (0x7fff) */
    B(0x75, 0x10);              /*     Report Size (16) */
    B(0x95, 0x02);              /*     Report Count (2) */
    B(0x81, 0x02);              /*     Input (Data, Variable, Absolute) */
    if (!contact) {
        B(0x75, 0x08);          /*     Report Size (8) */
        B(0x95, 0x01);          /*     Report Count (1) */
        B(0x81, 0x03);          /*     Input (Constant) -- pad (wheel byte) */
    }
    B(0xc0);                    /*   End Collection */
    if (contact) {
        B(0x05, 0x0d);          /*   Usage Page (Digitizer) */
        B(0x09, 0x54);          /*   Usage (Contact Count) */
        B(0x25, 0x7f);          /*   Logical Maximum (127) */
        B(0x75, 0x08);          /*   Report Size (8) */
        B(0x95, 0x01);          /*   Report Count (1) */
        B(0x81, 0x02);          /*   Input (Data, Variable, Absolute) */
    }
    B(0xc0);                    /* End Collection */
#undef B
    return n;
}

/* Rewrite a 6-byte tablet report in place into the touchscreen's layout. */
static int usb_touchscreen_report(USBHIDState *us, uint8_t *buf, int len,
                                  int size)
{
    uint8_t flags;

    if (len < 6) {
        return len;
    }
    if (getenv("ASOFT_LAT")) {
        fprintf(stderr, "LAT report %" PRId64 " tip=%d x=%d y=%d\n", get_clock(),
                buf[0] & 1, buf[1] | buf[2] << 8, buf[3] | buf[4] << 8);
    }
    flags = (buf[0] & 0x01) ? 0x07 : 0x04;  /* Tip|InRange|Valid, or Valid */
    if (!(us->ts_variant & TS_VARIANT_CONTACT)) {
        buf[0] = flags;
        buf[5] = 0;
        return 6;
    }
    if (size < 7) {
        return len;
    }
    memmove(buf + 2, buf + 1, 4);
    buf[0] = flags;
    buf[1] = 0;                             /* Contact Identifier */
    buf[6] = 1;                             /* Contact Count */
    return 7;
}

static const uint8_t qemu_keyboard_hid_report_descriptor[] = {
    0x05, 0x01,		/* Usage Page (Generic Desktop) */
    0x09, 0x06,		/* Usage (Keyboard) */
    0xa1, 0x01,		/* Collection (Application) */
    0x75, 0x01,		/*   Report Size (1) */
    0x95, 0x08,		/*   Report Count (8) */
    0x05, 0x07,		/*   Usage Page (Key Codes) */
    0x19, 0xe0,		/*   Usage Minimum (224) */
    0x29, 0xe7,		/*   Usage Maximum (231) */
    0x15, 0x00,		/*   Logical Minimum (0) */
    0x25, 0x01,		/*   Logical Maximum (1) */
    0x81, 0x02,		/*   Input (Data, Variable, Absolute) */
    0x95, 0x01,		/*   Report Count (1) */
    0x75, 0x08,		/*   Report Size (8) */
    0x81, 0x01,		/*   Input (Constant) */
    0x95, 0x05,		/*   Report Count (5) */
    0x75, 0x01,		/*   Report Size (1) */
    0x05, 0x08,		/*   Usage Page (LEDs) */
    0x19, 0x01,		/*   Usage Minimum (1) */
    0x29, 0x05,		/*   Usage Maximum (5) */
    0x91, 0x02,		/*   Output (Data, Variable, Absolute) */
    0x95, 0x01,		/*   Report Count (1) */
    0x75, 0x03,		/*   Report Size (3) */
    0x91, 0x01,		/*   Output (Constant) */
    0x95, 0x06,		/*   Report Count (6) */
    0x75, 0x08,		/*   Report Size (8) */
    0x15, 0x00,		/*   Logical Minimum (0) */
    0x25, 0xff,		/*   Logical Maximum (255) */
    0x05, 0x07,		/*   Usage Page (Key Codes) */
    0x19, 0x00,		/*   Usage Minimum (0) */
    0x29, 0xff,		/*   Usage Maximum (255) */
    0x81, 0x00,		/*   Input (Data, Array) */
    0xc0,		/* End Collection */
};

static void usb_hid_changed(HIDState *hs)
{
    USBHIDState *us = container_of(hs, USBHIDState, hid);

    usb_wakeup(us->intr, 0);
}

static void usb_hid_handle_reset(USBDevice *dev)
{
    USBHIDState *us = USB_HID(dev);

    hid_reset(&us->hid);
}

static void usb_hid_handle_control(USBDevice *dev, USBPacket *p,
               int request, int value, int index, int length, uint8_t *data)
{
    USBHIDState *us = USB_HID(dev);
    HIDState *hs = &us->hid;
    int ret;

    ret = usb_desc_handle_control(dev, p, request, value, index, length, data);
    if (ret >= 0) {
        return;
    }

    switch (request) {
        /* hid specific requests */
    case InterfaceRequest | USB_REQ_GET_DESCRIPTOR:
        switch (value >> 8) {
        case 0x22:
            if (hs->kind == HID_MOUSE) {
                memcpy(data, qemu_mouse_hid_report_descriptor,
                       sizeof(qemu_mouse_hid_report_descriptor));
                p->actual_length = sizeof(qemu_mouse_hid_report_descriptor);
            } else if (hs->kind == HID_TABLET) {
                if (us->is_touchscreen) {
                    memcpy(data, us->ts_desc, us->ts_desc_len);
                    p->actual_length = us->ts_desc_len;
                } else {
                    memcpy(data, qemu_tablet_hid_report_descriptor,
                           sizeof(qemu_tablet_hid_report_descriptor));
                    p->actual_length = sizeof(qemu_tablet_hid_report_descriptor);
                }
            } else if (hs->kind == HID_KEYBOARD) {
                memcpy(data, qemu_keyboard_hid_report_descriptor,
                       sizeof(qemu_keyboard_hid_report_descriptor));
                p->actual_length = sizeof(qemu_keyboard_hid_report_descriptor);
            }
            break;
        default:
            goto fail;
        }
        break;
    case HID_GET_REPORT:
        if (hs->kind == HID_MOUSE || hs->kind == HID_TABLET) {
            p->actual_length = hid_pointer_poll(hs, data, length);
            if (us->is_touchscreen) {
                p->actual_length = usb_touchscreen_report(us, data,
                                                          p->actual_length,
                                                          length);
            }
        } else if (hs->kind == HID_KEYBOARD) {
            p->actual_length = hid_keyboard_poll(hs, data, length);
        }
        break;
    case HID_SET_REPORT:
        if (hs->kind == HID_KEYBOARD) {
            p->actual_length = hid_keyboard_write(hs, data, length);
        } else {
            goto fail;
        }
        break;
    case HID_GET_PROTOCOL:
        if (hs->kind != HID_KEYBOARD && hs->kind != HID_MOUSE) {
            goto fail;
        }
        data[0] = hs->protocol;
        p->actual_length = 1;
        break;
    case HID_SET_PROTOCOL:
        if (hs->kind != HID_KEYBOARD && hs->kind != HID_MOUSE) {
            goto fail;
        }
        hs->protocol = value;
        break;
    case HID_GET_IDLE:
        data[0] = hs->idle;
        p->actual_length = 1;
        break;
    case HID_SET_IDLE:
        hs->idle = (uint8_t) (value >> 8);
        hid_set_next_idle(hs);
        if (hs->kind == HID_MOUSE || hs->kind == HID_TABLET) {
            hid_pointer_activate(hs);
        }
        break;
    default:
    fail:
        p->status = USB_RET_STALL;
        break;
    }
}

static void usb_hid_handle_data(USBDevice *dev, USBPacket *p)
{
    USBHIDState *us = USB_HID(dev);
    HIDState *hs = &us->hid;
    g_autofree uint8_t *buf = g_malloc(MAX(p->iov.size, 8));
    int len = 0;

    switch (p->pid) {
    case USB_TOKEN_IN:
        if (p->ep->nr == 1) {
            if (hs->kind == HID_MOUSE || hs->kind == HID_TABLET) {
                hid_pointer_activate(hs);
            }
            if (!hid_has_events(hs)) {
                p->status = USB_RET_NAK;
                return;
            }
            hid_set_next_idle(hs);
            if (hs->kind == HID_MOUSE || hs->kind == HID_TABLET) {
                len = hid_pointer_poll(hs, buf, p->iov.size);
            } else if (hs->kind == HID_KEYBOARD) {
                len = hid_keyboard_poll(hs, buf, p->iov.size);
            }
            if (us->is_touchscreen) {
                len = usb_touchscreen_report(us, buf, len, p->iov.size);
            }
            usb_packet_copy(p, buf, len);
        } else {
            goto fail;
        }
        break;
    case USB_TOKEN_OUT:
    default:
    fail:
        p->status = USB_RET_STALL;
        break;
    }
}

static void usb_hid_unrealize(USBDevice *dev)
{
    USBHIDState *us = USB_HID(dev);

    hid_free(&us->hid);
}

static void usb_hid_initfn(USBDevice *dev, int kind,
                           const USBDesc *usb1, const USBDesc *usb2,
                           Error **errp)
{
    USBHIDState *us = USB_HID(dev);
    switch (us->usb_version) {
    case 1:
        dev->usb_desc = usb1;
        break;
    case 2:
        dev->usb_desc = usb2;
        break;
    default:
        dev->usb_desc = NULL;
    }
    if (!dev->usb_desc) {
        error_setg(errp, "Invalid usb version %d for usb hid device",
                   us->usb_version);
        return;
    }

    usb_desc_create_serial(dev);
    usb_desc_init(dev);
    us->intr = usb_ep_get(dev, USB_TOKEN_IN, 1);
    hid_init(&us->hid, kind, usb_hid_changed);
    if (us->display && us->hid.s) {
        qemu_input_handler_bind(us->hid.s, us->display, us->head, NULL);
    }
}

static void usb_tablet_realize(USBDevice *dev, Error **errp)
{

    usb_hid_initfn(dev, HID_TABLET, &desc_tablet, &desc_tablet2, errp);
}

static void usb_touchscreen_realize(USBDevice *dev, Error **errp)
{
    USBHIDState *us = USB_HID(dev);

    us->is_touchscreen = true;
    us->ts_desc_len = usb_touchscreen_build_desc(us->ts_desc, us->ts_variant);
    stw_le_p(&touchscreen_hid_class_desc[7], us->ts_desc_len);
    usb_hid_initfn(dev, HID_TABLET, &desc_touchscreen, &desc_touchscreen2, errp);
}

static void usb_mouse_realize(USBDevice *dev, Error **errp)
{
    usb_hid_initfn(dev, HID_MOUSE, &desc_mouse, &desc_mouse2, errp);
}

static void usb_keyboard_realize(USBDevice *dev, Error **errp)
{
    usb_hid_initfn(dev, HID_KEYBOARD, &desc_keyboard, &desc_keyboard2, errp);
}

static int usb_ptr_post_load(void *opaque, int version_id)
{
    USBHIDState *s = opaque;

    if (s->dev.remote_wakeup) {
        hid_pointer_activate(&s->hid);
    }
    return 0;
}

static const VMStateDescription vmstate_usb_ptr = {
    .name = "usb-ptr",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = usb_ptr_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_USB_DEVICE(dev, USBHIDState),
        VMSTATE_HID_POINTER_DEVICE(hid, USBHIDState),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_usb_kbd = {
    .name = "usb-kbd",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_USB_DEVICE(dev, USBHIDState),
        VMSTATE_HID_KEYBOARD_DEVICE(hid, USBHIDState),
        VMSTATE_END_OF_LIST()
    }
};

static void usb_hid_class_initfn(ObjectClass *klass, const void *data)
{
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->handle_reset   = usb_hid_handle_reset;
    uc->handle_control = usb_hid_handle_control;
    uc->handle_data    = usb_hid_handle_data;
    uc->unrealize      = usb_hid_unrealize;
    uc->handle_attach  = usb_desc_attach;
}

static const TypeInfo usb_hid_type_info = {
    .name = TYPE_USB_HID,
    .parent = TYPE_USB_DEVICE,
    .instance_size = sizeof(USBHIDState),
    .abstract = true,
    .class_init = usb_hid_class_initfn,
};

static const Property usb_tablet_properties[] = {
        DEFINE_PROP_UINT32("usb_version", USBHIDState, usb_version, 2),
        DEFINE_PROP_STRING("display", USBHIDState, display),
        DEFINE_PROP_UINT32("head", USBHIDState, head, 0),
};

static void usb_tablet_class_initfn(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->realize        = usb_tablet_realize;
    uc->product_desc   = "QEMU USB Tablet";
    dc->vmsd = &vmstate_usb_ptr;
    device_class_set_props(dc, usb_tablet_properties);
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static const TypeInfo usb_tablet_info = {
    .name          = "usb-tablet",
    .parent        = TYPE_USB_HID,
    .class_init    = usb_tablet_class_initfn,
};

static const Property usb_touchscreen_properties[] = {
        DEFINE_PROP_UINT32("usb_version", USBHIDState, usb_version, 2),
        DEFINE_PROP_STRING("display", USBHIDState, display),
        DEFINE_PROP_UINT32("head", USBHIDState, head, 0),
        DEFINE_PROP_UINT32("variant", USBHIDState, ts_variant, 0),
};

static void usb_touchscreen_class_initfn(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->realize        = usb_touchscreen_realize;
    uc->product_desc   = "QEMU USB Touchscreen";
    dc->vmsd = &vmstate_usb_ptr;
    device_class_set_props(dc, usb_touchscreen_properties);
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static const TypeInfo usb_touchscreen_info = {
    .name          = "usb-touchscreen",
    .parent        = TYPE_USB_HID,
    .class_init    = usb_touchscreen_class_initfn,
};

static const Property usb_mouse_properties[] = {
        DEFINE_PROP_UINT32("usb_version", USBHIDState, usb_version, 2),
};

static void usb_mouse_class_initfn(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->realize        = usb_mouse_realize;
    uc->product_desc   = "QEMU USB Mouse";
    dc->vmsd = &vmstate_usb_ptr;
    device_class_set_props(dc, usb_mouse_properties);
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static const TypeInfo usb_mouse_info = {
    .name          = "usb-mouse",
    .parent        = TYPE_USB_HID,
    .class_init    = usb_mouse_class_initfn,
};

static const Property usb_keyboard_properties[] = {
        DEFINE_PROP_UINT32("usb_version", USBHIDState, usb_version, 2),
        DEFINE_PROP_STRING("display", USBHIDState, display),
};

static void usb_keyboard_class_initfn(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->realize        = usb_keyboard_realize;
    uc->product_desc   = "QEMU USB Keyboard";
    dc->vmsd = &vmstate_usb_kbd;
    device_class_set_props(dc, usb_keyboard_properties);
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static const TypeInfo usb_keyboard_info = {
    .name          = "usb-kbd",
    .parent        = TYPE_USB_HID,
    .class_init    = usb_keyboard_class_initfn,
};

/* ======================================================================= *
 * usb-consumer: a Consumer Control HID device (Home / Power / Volume).
 * The vphone600 lock screen ("Press home to open") unlocks on a Consumer
 * Home (0x0C:0x40) press; no touch gesture dismisses it. Trigger a press from
 * the monitor:  qom-set /machine/peripheral/<id> press home   (also power,
 * volup, voldown). The button auto-releases hold-ms (default 100) after the
 * guest read it down. "down" and "up" instead hold a button until released.
 * Each change of the buttons is queued until the guest reads it: a press and
 * its release both made before the guest's next poll reach it as two reports
 * (sent as the current state alone, the press would be lost).
 * ======================================================================= */

#define USB_CONSUMER_QUEUE 16

struct USBConsumerState {
    USBDevice dev;
    USBEndpoint *intr;
    uint32_t usb_version;
    uint32_t hold_ms;
    uint8_t report;        /* the buttons down, as the newest queued report */
    uint8_t queue[USB_CONSUMER_QUEUE];  /* reports the guest has not read */
    unsigned head, count;
    uint8_t auto_release;  /* buttons "press" holds for hold-ms */
    uint8_t armed;         /* of those, the ones the guest has read down */
    QEMUTimer *timer;
};

#define TYPE_USB_CONSUMER "usb-consumer"
OBJECT_DECLARE_SIMPLE_TYPE(USBConsumerState, USB_CONSUMER)

static const uint8_t qemu_consumer_hid_report_descriptor[] = {
    0x05, 0x0c,         /* Usage Page (Consumer) */
    0x09, 0x01,         /* Usage (Consumer Control) */
    0xa1, 0x01,         /* Collection (Application) */
    0x15, 0x00,         /*   Logical Minimum (0) */
    0x25, 0x01,         /*   Logical Maximum (1) */
    0x75, 0x01,         /*   Report Size (1) */
    0x95, 0x04,         /*   Report Count (4) */
    0x09, 0x40,         /*   Usage (Menu / Home) */
    0x09, 0x30,         /*   Usage (Power) */
    0x09, 0xe9,         /*   Usage (Volume Increment) */
    0x09, 0xea,         /*   Usage (Volume Decrement) */
    0x81, 0x02,         /*   Input (Data, Variable, Absolute) */
    0x95, 0x04,         /*   Report Count (4) */
    0x81, 0x03,         /*   Input (Constant) -- 4-bit padding */
    0xc0,               /* End Collection */
};

static const USBDescIface desc_iface_consumer = {
    .bInterfaceNumber              = 0,
    .bNumEndpoints                 = 1,
    .bInterfaceClass               = USB_CLASS_HID,
    .bInterfaceProtocol            = 0x00,
    .ndesc                         = 1,
    .descs = (USBDescOther[]) {
        {
            .data = (uint8_t[]) {
                0x09,          /*  u8  bLength */
                USB_DT_HID,    /*  u8  bDescriptorType */
                0x01, 0x00,    /*  u16 HID_class */
                0x00,          /*  u8  country_code */
                0x01,          /*  u8  num_descriptors */
                USB_DT_REPORT, /*  u8  type: Report */
                29, 0,         /*  u16 len (consumer report descriptor) */
            },
        },
    },
    .eps = (USBDescEndpoint[]) {
        {
            .bEndpointAddress      = USB_DIR_IN | 0x01,
            .bmAttributes          = USB_ENDPOINT_XFER_INT,
            .wMaxPacketSize        = 8,
            .bInterval             = 0x0a,
        },
    },
};

static const USBDescDevice desc_device_consumer = {
    .bcdUSB                        = 0x0100,
    .bMaxPacketSize0               = 8,
    .bNumConfigurations            = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces        = 1,
            .bConfigurationValue   = 1,
            .iConfiguration        = STR_CONFIG_CONSUMER,
            .bmAttributes          = USB_CFG_ATT_ONE | USB_CFG_ATT_WAKEUP,
            .bMaxPower             = 50,
            .nif = 1,
            .ifs = &desc_iface_consumer,
        },
    },
};

static const USBDescDevice desc_device_consumer2 = {
    .bcdUSB                        = 0x0200,
    .bMaxPacketSize0               = 64,
    .bNumConfigurations            = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces        = 1,
            .bConfigurationValue   = 1,
            .iConfiguration        = STR_CONFIG_CONSUMER,
            .bmAttributes          = USB_CFG_ATT_ONE | USB_CFG_ATT_WAKEUP,
            .bMaxPower             = 50,
            .nif = 1,
            .ifs = &desc_iface_consumer,
        },
    },
};

static const USBDesc desc_consumer = {
    .id = {
        .idVendor          = 0x0627,
        .idProduct         = 0x0001,
        .bcdDevice         = 0,
        .iManufacturer     = STR_MANUFACTURER,
        .iProduct          = STR_PRODUCT_CONSUMER,
        .iSerialNumber     = STR_SERIAL_CONSUMER,
    },
    .full = &desc_device_consumer,
    .high = &desc_device_consumer2,
    .str  = desc_strings,
    .msos = &desc_msos_suspend,
};

/* The buttons become @report: queued for the guest, which is woken for it. */
static void usb_consumer_set_report(USBConsumerState *us, uint8_t report)
{
    if (report == us->report) {
        return;
    }
    us->report = report;
    if (us->count == USB_CONSUMER_QUEUE) {
        /* the guest reads nothing: the newest report becomes this one */
        us->queue[(us->head + us->count - 1) % USB_CONSUMER_QUEUE] = report;
    } else {
        us->queue[(us->head + us->count++) % USB_CONSUMER_QUEUE] = report;
    }
    trace_usb_consumer_queue(report, us->count);
    usb_wakeup(us->intr, 0);
}

static void usb_consumer_release(void *opaque)
{
    USBConsumerState *us = opaque;
    uint8_t release = us->armed & us->auto_release;

    us->auto_release &= ~release;
    us->armed = 0;
    usb_consumer_set_report(us, us->report & ~release);
}

static void usb_consumer_handle_reset(USBDevice *dev)
{
    USBConsumerState *us = USB_CONSUMER(dev);

    timer_del(us->timer);
    us->report = 0;
    us->head = us->count = 0;
    us->auto_release = us->armed = 0;
}

static void usb_consumer_handle_control(USBDevice *dev, USBPacket *p,
               int request, int value, int index, int length, uint8_t *data)
{
    USBConsumerState *us = USB_CONSUMER(dev);
    int ret;

    ret = usb_desc_handle_control(dev, p, request, value, index, length, data);
    if (ret >= 0) {
        return;
    }

    switch (request) {
    case InterfaceRequest | USB_REQ_GET_DESCRIPTOR:
        if ((value >> 8) == 0x22) {
            memcpy(data, qemu_consumer_hid_report_descriptor,
                   sizeof(qemu_consumer_hid_report_descriptor));
            p->actual_length = sizeof(qemu_consumer_hid_report_descriptor);
        } else {
            goto fail;
        }
        break;
    case HID_GET_REPORT:
        data[0] = us->report;
        p->actual_length = 1;
        break;
    case HID_GET_IDLE:
        data[0] = 0;
        p->actual_length = 1;
        break;
    case HID_SET_IDLE:
        break;
    default:
    fail:
        p->status = USB_RET_STALL;
        break;
    }
}

static void usb_consumer_handle_data(USBDevice *dev, USBPacket *p)
{
    USBConsumerState *us = USB_CONSUMER(dev);
    uint8_t buf[1];

    if (p->pid == USB_TOKEN_IN && p->ep->nr == 1) {
        if (!us->count) {
            p->status = USB_RET_NAK;
            return;
        }
        buf[0] = us->queue[us->head];
        us->head = (us->head + 1) % USB_CONSUMER_QUEUE;
        us->count--;
        usb_packet_copy(p, buf, 1);
        trace_usb_consumer_read(buf[0], us->count);
        /* a press's hold counts from the guest's read of it */
        if (buf[0] & us->auto_release & ~us->armed) {
            us->armed |= buf[0] & us->auto_release;
            timer_mod(us->timer,
                      qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + us->hold_ms);
        }
    } else {
        p->status = USB_RET_STALL;
    }
}

static uint8_t usb_consumer_bit(const char *value, Error **errp)
{
    if (!strcmp(value, "home") || !strcmp(value, "menu")) {
        return 0x01;
    } else if (!strcmp(value, "power")) {
        return 0x02;
    } else if (!strcmp(value, "volup")) {
        return 0x04;
    } else if (!strcmp(value, "voldown")) {
        return 0x08;
    }
    error_setg(errp, "usb-consumer: unknown button '%s' "
               "(home|power|volup|voldown)", value);
    return 0;
}

/* press: down, then up hold-ms after the guest read it down */
static void usb_consumer_set_press(Object *obj, const char *value, Error **errp)
{
    USBConsumerState *us = USB_CONSUMER(obj);
    uint8_t bit = usb_consumer_bit(value, errp);

    if (bit && !(us->report & bit)) {
        us->auto_release |= bit;
        us->armed &= ~bit;
        usb_consumer_set_report(us, us->report | bit);
    }
}

/* down / up: a button held until it is released */
static void usb_consumer_set_down(Object *obj, const char *value, Error **errp)
{
    USBConsumerState *us = USB_CONSUMER(obj);
    uint8_t bit = usb_consumer_bit(value, errp);

    us->auto_release &= ~bit;
    us->armed &= ~bit;
    usb_consumer_set_report(us, us->report | bit);
}

static void usb_consumer_set_up(Object *obj, const char *value, Error **errp)
{
    USBConsumerState *us = USB_CONSUMER(obj);
    uint8_t bit = usb_consumer_bit(value, errp);

    us->auto_release &= ~bit;
    us->armed &= ~bit;
    usb_consumer_set_report(us, us->report & ~bit);
}

static char *usb_consumer_get_press(Object *obj, Error **errp)
{
    return g_strdup("");
}

static void usb_consumer_realize(USBDevice *dev, Error **errp)
{
    USBConsumerState *us = USB_CONSUMER(dev);

    dev->usb_desc = &desc_consumer;
    usb_desc_create_serial(dev);
    usb_desc_init(dev);
    us->intr = usb_ep_get(dev, USB_TOKEN_IN, 1);
    us->report = 0;
    us->head = us->count = 0;
    us->auto_release = us->armed = 0;
    us->timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, usb_consumer_release, us);

    object_property_add_str(OBJECT(dev), "press",
                            usb_consumer_get_press, usb_consumer_set_press);
    object_property_add_str(OBJECT(dev), "down",
                            usb_consumer_get_press, usb_consumer_set_down);
    object_property_add_str(OBJECT(dev), "up",
                            usb_consumer_get_press, usb_consumer_set_up);
}

static void usb_consumer_unrealize(USBDevice *dev)
{
    USBConsumerState *us = USB_CONSUMER(dev);

    timer_free(us->timer);
}

static const Property usb_consumer_properties[] = {
    DEFINE_PROP_UINT32("usb_version", USBConsumerState, usb_version, 2),
    DEFINE_PROP_UINT32("hold-ms", USBConsumerState, hold_ms, 100),
};

static void usb_consumer_class_initfn(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->realize        = usb_consumer_realize;
    uc->unrealize      = usb_consumer_unrealize;
    uc->product_desc   = "QEMU USB Consumer Control";
    uc->handle_reset   = usb_consumer_handle_reset;
    uc->handle_control = usb_consumer_handle_control;
    uc->handle_data    = usb_consumer_handle_data;
    uc->handle_attach  = usb_desc_attach;
    device_class_set_props(dc, usb_consumer_properties);
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static const TypeInfo usb_consumer_info = {
    .name          = TYPE_USB_CONSUMER,
    .parent        = TYPE_USB_DEVICE,
    .instance_size = sizeof(USBConsumerState),
    .class_init    = usb_consumer_class_initfn,
};

static void usb_hid_register_types(void)
{
    type_register_static(&usb_hid_type_info);
    type_register_static(&usb_tablet_info);
    usb_legacy_register("usb-tablet", "tablet", NULL);
    type_register_static(&usb_touchscreen_info);
    type_register_static(&usb_consumer_info);
    type_register_static(&usb_mouse_info);
    usb_legacy_register("usb-mouse", "mouse", NULL);
    type_register_static(&usb_keyboard_info);
    usb_legacy_register("usb-kbd", "keyboard", NULL);
}

type_init(usb_hid_register_types)
