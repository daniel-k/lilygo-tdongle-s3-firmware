// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Daniel Roussel
/*
 * ESP32-S3 USB composite device.
 *
 * Two build targets, selected by exactly one of BOARD_T_DONGLE_S3 /
 * BOARD_SUPER_MINI defined at compile time (see platformio.ini):
 *
 *   BOARD_T_DONGLE_S3 — full firmware for the LilyGo T-Dongle S3:
 *                       BT HCI + dummy SCO + Mass Storage + GUD display
 *                       (4 interfaces).
 *   BOARD_SUPER_MINI  — Bluetooth-only firmware for plain ESP32-S3 boards
 *                       (no LCD, no µSD): BT HCI + dummy SCO (2 interfaces).
 *
 * USB BT HCI spec (Bluetooth Core v5.x, Vol 4, Part B):
 *   EP0  Control OUT  -> HCI Commands  (host -> controller)
 *   EP1  Interrupt IN -> HCI Events    (controller -> host)
 *   EP2  Bulk IN      -> ACL Data      (controller -> host)
 *   EP2  Bulk OUT     -> ACL Data      (host -> controller)
 */

#if !defined(BOARD_T_DONGLE_S3) && !defined(BOARD_SUPER_MINI)
#error "Define one of BOARD_T_DONGLE_S3 or BOARD_SUPER_MINI in build_flags."
#endif
#if defined(BOARD_T_DONGLE_S3) && defined(BOARD_SUPER_MINI)
#error "Define exactly one of BOARD_T_DONGLE_S3 / BOARD_SUPER_MINI."
#endif

#include <string.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_rom_sys.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/system_reg.h"

#include "esp_heap_caps.h"
#include "esp_private/usb_phy.h"
#include "tusb.h"
#include "device/usbd.h"
#include "device/usbd_pvt.h"

#ifdef BOARD_T_DONGLE_S3
#include "gud/gud_usb.h"
#include "gud/gud_display.h"
#include "msc_sd.h"
#endif

static const char *TAG = "USB_BT_HCI";

/* ------------------------------------------------------------------ */
/*  USB Descriptor constants                                          */
/* ------------------------------------------------------------------ */

/* OpenMoko / Openhardware.io shared vendor ID, with the GUD display PID
 * allocated to "GUD USB Display". The Linux gud kernel driver carries this
 * pair in its static id_table, so it auto-binds without any udev new_id
 * dance. btusb matches by USB interface class (not VID:PID), so /dev/hciN
 * still comes up cleanly. iManufacturer / iProduct strings keep saying
 * "Espressif / ESP32-S3 …" so the device is still identifiable as ours. */
#define USB_VID   0x1D50
#define USB_PID   0x614D
#define USB_BCD   0x0200  /* USB 2.0 */

/* Endpoint addresses
 *
 * T-Dongle S3 build:
 *   IF0       BT HCI:     EP1 IN interrupt, EP2 IN/OUT bulk
 *   IF1       BT SCO:     no endpoints (dummy, satisfies btusb's N+1 quirk)
 *   IF2       MSC:        EP3 IN/OUT bulk
 *   IF3       GUD vendor: EP5 OUT bulk
 *
 * Super Mini build (BT-only):
 *   IF0       BT HCI:     EP1 IN interrupt, EP2 IN/OUT bulk
 *   IF1       BT SCO:     no endpoints (dummy)
 *
 * BT HCI MUST be at IF0: btusb's alloc_ctrl_urb() hardcodes wIndex=0 for
 * HCI command control transfers (drivers/bluetooth/btusb.c). With HCI at
 * any other interface, TinyUSB routes those control transfers to whatever
 * driver owns IF0 — which then stalls EP0 and the host gives up with
 * EPIPE on opcode 0x0c03 (HCI Reset). MSC at IF2 is fine on any modern
 * UEFI (the boot stack walks all interfaces looking for class 0x08).
 */
#define EP_EVT_IN       0x81  /* Interrupt IN  - HCI events */
#define EP_ACL_IN       0x82  /* Bulk IN       - ACL data to host */
#define EP_ACL_OUT      0x02  /* Bulk OUT      - ACL data from host */
#ifdef BOARD_T_DONGLE_S3
#define EP_MSC_IN       0x83  /* Bulk IN       - SCSI READ  responses */
#define EP_MSC_OUT      0x03  /* Bulk OUT      - SCSI WRITE payloads  */
#define EP_GUD_OUT      0x05  /* Bulk OUT      - framebuffer transfers */
#define EP_MSC_SIZE     64
#endif

#define EP_EVT_SIZE   16
#define EP_ACL_SIZE   64

/* String descriptor indices */
enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
#ifdef BOARD_T_DONGLE_S3
    STRID_GUD,
#endif
};

/* ------------------------------------------------------------------ */
/*  USB Device Descriptor                                             */
/* ------------------------------------------------------------------ */

/* Composite device. Class = Miscellaneous / Common / IAD so the host
 * parses the IAD(s) to find the separate functions.
 *   T-Dongle S3 build: BT (IF0+IF1) + MSC (IF2) + GUD (IF3).
 *   Super Mini build:  BT (IF0+IF1) only. */
static tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,
    .bDeviceClass       = TUSB_CLASS_MISC,       /* 0xEF */
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,  /* 0x02 */
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,     /* 0x01 */
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = STRID_MANUFACTURER,
    .iProduct           = STRID_PRODUCT,
    .iSerialNumber      = STRID_SERIAL,
    .bNumConfigurations = 1,
};

/* ------------------------------------------------------------------ */
/*  USB Configuration Descriptor                                      */
/* ------------------------------------------------------------------ */

/* Config total length:
 *   config hdr (9)
 * + BT IAD (8) + HCI iface (9) + 3 EPs (21) + dummy SCO iface (9)
 * [T-Dongle only:]
 * + MSC iface (9) + 2 EPs (14)
 * + GUD IAD (8) + iface (9) + 1 EP (7)
 */
#ifdef BOARD_T_DONGLE_S3
#define NUM_INTERFACES    4
#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN \
                           + 8 + 9 + 21 + 9 \
                           + 9 + 14 \
                           + 8 + 9 + 7)
#else
#define NUM_INTERFACES    2
#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN \
                           + 8 + 9 + 21 + 9)
#endif

static uint8_t const desc_configuration[] = {
    /* Configuration descriptor */
    9, TUSB_DESC_CONFIGURATION, U16_TO_U8S_LE(CONFIG_TOTAL_LEN),
    NUM_INTERFACES, 1 /* config# */, 0 /* iConfig */,
    TU_BIT(7) /* bus powered */, 250 /* 500mA */,

    /* IAD for BT: covers IF0 (HCI) + IF1 (dummy SCO). HCI must live at IF0
     * because btusb's alloc_ctrl_urb() hardcodes wIndex=0 for HCI command
     * control transfers — placing HCI elsewhere routes them to whichever
     * driver owns IF0, which stalls EP0 and the host fails opcode 0x0c03
     * (HCI Reset) with -EPIPE. The dummy IF1 keeps btusb_probe() happy: it
     * unconditionally claims interface N+1 of an HCI device looking for a
     * SCO ISOC alt-setting and refuses to bind to N if that fails. With a
     * real-looking SCO at IF1 (alt 0, zero ISOC endpoints, idle SCO) btusb
     * claims it without ever activating ISOC — we're BLE-only — and
     * /dev/hciN comes up cleanly. */
    8, TUSB_DESC_INTERFACE_ASSOCIATION,
    0 /* first interface */, 2 /* interface count */,
    0xE0, 0x01, 0x01 /* class / subclass / protocol */,
    0 /* iFunction */,

    /* Interface 0: Bluetooth HCI */
    9, TUSB_DESC_INTERFACE, 0 /* itf# */, 0 /* alt */, 3 /* nEPs */,
    0xE0, 0x01, 0x01, 0,

    /* EP1 IN - Interrupt (HCI Events) */
    7, TUSB_DESC_ENDPOINT, EP_EVT_IN,
    TUSB_XFER_INTERRUPT, U16_TO_U8S_LE(EP_EVT_SIZE), 1 /* 1ms interval */,

    /* EP2 IN - Bulk (ACL Data IN) */
    7, TUSB_DESC_ENDPOINT, EP_ACL_IN,
    TUSB_XFER_BULK, U16_TO_U8S_LE(EP_ACL_SIZE), 0,

    /* EP2 OUT - Bulk (ACL Data OUT) */
    7, TUSB_DESC_ENDPOINT, EP_ACL_OUT,
    TUSB_XFER_BULK, U16_TO_U8S_LE(EP_ACL_SIZE), 0,

    /* Interface 1: dummy SCO (alt 0, zero ISOC endpoints). See BT IAD. */
    9, TUSB_DESC_INTERFACE, 1 /* itf */, 0 /* alt */, 0 /* nEPs */,
    0xE0, 0x01, 0x01, 0,

#ifdef BOARD_T_DONGLE_S3
    /* Interface 2: Mass Storage (SCSI transparent / Bulk-Only Transport).
     * Bare interface — single function, no IAD needed. Modern UEFI walks
     * all interfaces looking for class 0x08, so MSC at IF2 boots fine. */
    9, TUSB_DESC_INTERFACE, 2 /* itf# */, 0 /* alt */, 2 /* nEPs */,
    TUSB_CLASS_MSC, MSC_SUBCLASS_SCSI, MSC_PROTOCOL_BOT, 0 /* iInterface */,

    /* EP3 IN - Bulk (SCSI responses) */
    7, TUSB_DESC_ENDPOINT, EP_MSC_IN,
    TUSB_XFER_BULK, U16_TO_U8S_LE(EP_MSC_SIZE), 0,

    /* EP3 OUT - Bulk (SCSI command + data) */
    7, TUSB_DESC_ENDPOINT, EP_MSC_OUT,
    TUSB_XFER_BULK, U16_TO_U8S_LE(EP_MSC_SIZE), 0,

    /* IAD for GUD (single vendor-class interface). */
    8, TUSB_DESC_INTERFACE_ASSOCIATION,
    3 /* first interface */, 1 /* interface count */,
    TUSB_CLASS_VENDOR_SPECIFIC, 0x00, 0x00, 0 /* iFunction */,

    /* Interface 3: GUD (Generic USB Display) */
    9, TUSB_DESC_INTERFACE, 3 /* itf# */, 0 /* alt */, 1 /* nEPs */,
    TUSB_CLASS_VENDOR_SPECIFIC, 0x00, 0x00, STRID_GUD,

    /* EP5 OUT - Bulk (framebuffer pixels from host) */
    7, TUSB_DESC_ENDPOINT, EP_GUD_OUT,
    TUSB_XFER_BULK, U16_TO_U8S_LE(GUD_BULK_OUT_SIZE), 0,
#endif /* BOARD_T_DONGLE_S3 */
};

static char const *string_desc_arr[] = {
    "",
    "Espressif",
#ifdef BOARD_T_DONGLE_S3
    "ESP32-S3 BT HCI + MSC + GUD",
#else
    "ESP32-S3 BT HCI",
#endif
    "000001",
#ifdef BOARD_T_DONGLE_S3
    "ESP32-S3 GUD Display",
#endif
};
static uint16_t _desc_str[33];

uint8_t const *tud_descriptor_device_cb(void) { return (uint8_t const *)&desc_device; }
uint8_t const *tud_descriptor_configuration_cb(uint8_t index) { (void)index; return desc_configuration; }

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    uint8_t chr_count;
    if (index == 0) { _desc_str[1] = 0x0409; chr_count = 1; }
    else {
        if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) return NULL;
        const char *str = string_desc_arr[index];
        chr_count = strlen(str);
        if (chr_count > 31) chr_count = 31;
        for (uint8_t i = 0; i < chr_count; i++) _desc_str[1 + i] = str[i];
    }
    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}

void usb_device_task(void *arg) { (void)arg; while (1) tud_task(); }

/* ------------------------------------------------------------------ */
/*  BTH Class Driver state                                            */
/* ------------------------------------------------------------------ */

/* H4 packet type indicators */
#define H4_CMD   0x01
#define H4_ACL   0x02
#define H4_EVT   0x04

/* Max HCI packet sizes */
#define HCI_CMD_MAX   258   /* 3-byte header + 255 params */
#define HCI_EVT_MAX   260   /* 2-byte header + 255 params */
#define HCI_ACL_MAX   1024

/* Vendor HCI opcode (OGF=0x3F, OCF=0xFE) — reboot into ROM download mode.
 * Intercepted in btd_control_xfer_cb() before reaching the BT controller, so
 * we never have to worry about colliding with a real controller opcode. */
#define HCI_OPCODE_REBOOT_DOWNLOAD 0xFCFE

static struct {
    uint8_t ep_evt_in;
    uint8_t ep_acl_in;
    uint8_t ep_acl_out;

    /* RX from USB host (commands + ACL) → feed to BT controller */
    QueueHandle_t rx_queue;          /* queue of hci_rx_pkt_t* */

    /* Buffer for receiving ACL from host */
    uint8_t acl_out_buf[HCI_ACL_MAX];

    /* Buffer for HCI command from control transfer */
    uint8_t cmd_buf[HCI_CMD_MAX];

    /* TX to USB host (events + ACL) ← from BT controller */
    SemaphoreHandle_t evt_sem;       /* signals event EP available */
    SemaphoreHandle_t acl_in_sem;    /* signals ACL IN EP available */

    /* HCI TL recv state */
    SemaphoreHandle_t rx_ready_sem;  /* controller has called _recv */
    esp_bt_hci_tl_callback_t rx_cb;
    void *rx_cb_arg;
    uint8_t *rx_buf;
    uint32_t rx_size;

    /* HCI TL send state */
    esp_bt_hci_tl_callback_t tx_cb;
    void *tx_cb_arg;
} s_bt;

/* Packet queued from USB host to BT controller */
typedef struct {
    uint8_t type;          /* H4 type byte */
    uint16_t len;          /* payload length */
    uint8_t data[];        /* flexible array */
} hci_rx_pkt_t;


/* ------------------------------------------------------------------ */
/*  Reboot into ROM download mode                                     */
/*                                                                    */
/*  Triggered by HCI vendor opcode 0xFCFE. Called inline from the     */
/*  control-transfer callback — the host sees libusb's transfer error */
/*  (ENODEV/EIO), which is fine: we deliberately disconnect.          */
/*                                                                    */
/*  esp_restart() can't be used: its IDF shutdown handlers deadlock   */
/*  against our HCI TL semaphores. esp_restart_noos() avoids that but */
/*  only does a CPU reset, which on the S3 doesn't run the ROM        */
/*  bootloader's force_download_boot check — the chip just resumes    */
/*  the app. esp_rom_software_reset_system() is a true system reset   */
/*  via the ROM, so the ROM bootloader runs and honors the bit.       */
/* ------------------------------------------------------------------ */

static void reboot_to_download_mode(void)
{
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);

    /* Reset BOTH USB peripheral blocks (OTG + USJ) — they share the same
     * analog phy, and residual state in either confuses the ROM bootloader's
     * USB init after a software reset (dmesg shows -71/-110 enum errors). */
    SET_PERI_REG_MASK(SYSTEM_PERIP_RST_EN1_REG,
                      SYSTEM_USB_RST | SYSTEM_USB_DEVICE_RST);
    esp_rom_delay_us(10 * 1000);
    CLEAR_PERI_REG_MASK(SYSTEM_PERIP_RST_EN1_REG,
                        SYSTEM_USB_RST | SYSTEM_USB_DEVICE_RST);

    esp_rom_software_reset_system();
}

/* ------------------------------------------------------------------ */
/*  BTH Class Driver implementation                                   */
/* ------------------------------------------------------------------ */

static void btd_init(void)
{
    ESP_LOGI(TAG, "BTH class init");
}

static bool btd_deinit(void)
{
    return true;
}

static void btd_reset(uint8_t rhport)
{
    (void)rhport;
}

static uint16_t btd_open(uint8_t rhport, tusb_desc_interface_t const *itf_desc, uint16_t max_len)
{
    /* Only accept Wireless Controller / RF / Bluetooth */
    if (itf_desc->bInterfaceClass != 0xE0 ||
        itf_desc->bInterfaceSubClass != 0x01 ||
        itf_desc->bInterfaceProtocol != 0x01) {
        return 0;
    }

    /* Our IAD has bInterfaceCount=2 (HCI IF0 + dummy SCO IF1). TinyUSB binds
     * both interfaces to this driver via the IAD count, but advances p_desc
     * by drv_len — so we must claim the IF1 descriptor here too, otherwise
     * the next iteration tries to open IF1 separately and trips its
     * "interface already bound" assertion. drv_len = IF0 (9) + 3 EPs (21) +
     * dummy IF1 (9) = 39. The dummy IF1 has zero endpoints; we don't have
     * to do anything with it on the device side beyond claiming it. */
    uint16_t const drv_len = (uint16_t)(9 + 3 * 7 + 9);
    if (max_len < drv_len) return 0;

    uint8_t const *p_desc = (uint8_t const *)itf_desc;
    p_desc += 9; /* skip interface descriptor */

    /* Open 3 endpoints */
    for (int i = 0; i < 3; i++) {
        tusb_desc_endpoint_t const *ep = (tusb_desc_endpoint_t const *)p_desc;
        if (ep->bDescriptorType != TUSB_DESC_ENDPOINT) return 0;

        usbd_edpt_open(rhport, ep);

        uint8_t addr = ep->bEndpointAddress;
        uint8_t xfer = ep->bmAttributes.xfer;

        if (xfer == TUSB_XFER_INTERRUPT && (addr & 0x80)) {
            s_bt.ep_evt_in = addr;
        } else if (xfer == TUSB_XFER_BULK && (addr & 0x80)) {
            s_bt.ep_acl_in = addr;
        } else if (xfer == TUSB_XFER_BULK && !(addr & 0x80)) {
            s_bt.ep_acl_out = addr;
        }

        p_desc += 7;
    }

    /* Prime ACL OUT endpoint for receiving data from host */
    usbd_edpt_xfer(rhport, s_bt.ep_acl_out, s_bt.acl_out_buf, sizeof(s_bt.acl_out_buf));

    ESP_LOGI(TAG, "BTH endpoints opened: evt_in=0x%02x acl_in=0x%02x acl_out=0x%02x",
             s_bt.ep_evt_in, s_bt.ep_acl_in, s_bt.ep_acl_out);

    return drv_len;
}

static bool btd_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request)
{
    /* HCI command: bmRequestType=0x20, bRequest=0x00 */
    if (request->bmRequestType_bit.type != TUSB_REQ_TYPE_CLASS) return false;
    if (request->bmRequestType_bit.direction != TUSB_DIR_OUT) return false;

    if (stage == CONTROL_STAGE_SETUP) {
        /* Accept the data phase */
        tud_control_xfer(rhport, request, s_bt.cmd_buf,
                         request->wLength < HCI_CMD_MAX ? request->wLength : HCI_CMD_MAX);
        return true;
    }

    if (stage == CONTROL_STAGE_DATA) {
        /* HCI command received - queue it for the controller */
        uint16_t len = request->wLength;
        if (len >= 3) {
            uint16_t opcode = (uint16_t)s_bt.cmd_buf[0] |
                              ((uint16_t)s_bt.cmd_buf[1] << 8);
            if (opcode == HCI_OPCODE_REBOOT_DOWNLOAD) {
                ESP_LOGW(TAG, "Vendor HCI: reboot into download mode");
                reboot_to_download_mode(); /* does not return */
                return true;
            }
            ESP_LOGI(TAG, "cmd op=0x%04x plen=%u", opcode, s_bt.cmd_buf[2]);
            ESP_LOG_BUFFER_HEXDUMP(TAG, s_bt.cmd_buf, len, ESP_LOG_INFO);
        }
        hci_rx_pkt_t *pkt = malloc(sizeof(hci_rx_pkt_t) + len);
        if (pkt) {
            pkt->type = H4_CMD;
            pkt->len = len;
            memcpy(pkt->data, s_bt.cmd_buf, len);
            if (xQueueSend(s_bt.rx_queue, &pkt, 0) != pdTRUE) {
                free(pkt);
                ESP_LOGW(TAG, "RX queue full, dropping HCI cmd");
            }
            xSemaphoreGive(s_bt.rx_ready_sem);
        }
        return true;
    }

    return true;
}

static bool btd_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes)
{
    (void)result;

    if (ep_addr == s_bt.ep_acl_out) {
        /* ACL data received from host */
        if (xferred_bytes > 0) {
            hci_rx_pkt_t *pkt = malloc(sizeof(hci_rx_pkt_t) + xferred_bytes);
            if (pkt) {
                pkt->type = H4_ACL;
                pkt->len = (uint16_t)xferred_bytes;
                memcpy(pkt->data, s_bt.acl_out_buf, xferred_bytes);
                if (xQueueSend(s_bt.rx_queue, &pkt, 0) != pdTRUE) {
                    free(pkt);
                    ESP_LOGW(TAG, "RX queue full, dropping ACL");
                }
                xSemaphoreGive(s_bt.rx_ready_sem);
            }
        }
        /* Re-prime the endpoint */
        usbd_edpt_xfer(rhport, s_bt.ep_acl_out, s_bt.acl_out_buf, sizeof(s_bt.acl_out_buf));
        return true;
    }

    if (ep_addr == s_bt.ep_evt_in) {
        xSemaphoreGive(s_bt.evt_sem);
        return true;
    }

    if (ep_addr == s_bt.ep_acl_in) {
        xSemaphoreGive(s_bt.acl_in_sem);
        return true;
    }

    return false;
}

/* Composite class drivers — BTH (this file), and on T-Dongle S3 also GUD
 * (gud_usb.c). C doesn't let us value-initialize a static array from another
 * extern const, so the array is filled at startup in app_main(). */
#ifdef BOARD_T_DONGLE_S3
static usbd_class_driver_t _app_drivers[2];
#else
static usbd_class_driver_t _app_drivers[1];
#endif

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count)
{
    *driver_count = (uint8_t)(sizeof(_app_drivers) / sizeof(_app_drivers[0]));
    return _app_drivers;
}

/* ------------------------------------------------------------------ */
/*  Controller → Host (send events/ACL over USB)                      */
/* ------------------------------------------------------------------ */

static void usb_send_event(const uint8_t *data, uint16_t len)
{
    /* Wait for interrupt EP to be available */
    xSemaphoreTake(s_bt.evt_sem, portMAX_DELAY);
    usbd_edpt_xfer(0, s_bt.ep_evt_in, (uint8_t *)data, len);
}

static void usb_send_acl(const uint8_t *data, uint16_t len)
{
    /* Wait for bulk IN EP to be available */
    xSemaphoreTake(s_bt.acl_in_sem, portMAX_DELAY);
    usbd_edpt_xfer(0, s_bt.ep_acl_in, (uint8_t *)data, len);
}

/* ------------------------------------------------------------------ */
/*  HCI Transport Layer for ESP BT Controller                         */
/* ------------------------------------------------------------------ */

/* The controller uses H4 framing on the TL interface.
 * _recv requests bytes:  first 1 (type), then header, then payload.
 * _send provides a complete H4 packet (type + payload).
 */

/* Stream buffer: reassembles queued packets into a byte stream
 * for the controller's _recv requests. */
static struct {
    uint8_t *buf;
    uint16_t len;
    uint16_t pos;
} s_stream;

static void stream_free(void)
{
    if (s_stream.buf) {
        free(s_stream.buf);
        s_stream.buf = NULL;
    }
    s_stream.len = 0;
    s_stream.pos = 0;
}

/* Feed pending data to the controller's recv buffer.
 * Returns the number of bytes fed. */
static uint32_t feed_controller(uint8_t *dst, uint32_t wanted)
{
    uint32_t fed = 0;

    while (fed < wanted) {
        /* Try to refill stream from queue */
        if (s_stream.pos >= s_stream.len) {
            stream_free();
            hci_rx_pkt_t *pkt = NULL;
            if (xQueueReceive(s_bt.rx_queue, &pkt, 0) != pdTRUE) {
                break; /* nothing available */
            }
            /* Build H4 framed packet: type + payload */
            s_stream.len = 1 + pkt->len;
            s_stream.buf = malloc(s_stream.len);
            if (!s_stream.buf) {
                free(pkt);
                s_stream.len = 0;
                break;
            }
            s_stream.buf[0] = pkt->type;
            memcpy(s_stream.buf + 1, pkt->data, pkt->len);
            s_stream.pos = 0;
            free(pkt);
        }

        uint32_t avail = s_stream.len - s_stream.pos;
        uint32_t n = (wanted - fed) < avail ? (wanted - fed) : avail;
        memcpy(dst + fed, s_stream.buf + s_stream.pos, n);
        s_stream.pos += n;
        fed += n;
    }

    return fed;
}

/* Task that services the controller's _recv requests */
static void hci_rx_task(void *arg)
{
    (void)arg;

    while (1) {
        /* Wait for controller to issue a _recv request */
        xSemaphoreTake(s_bt.rx_ready_sem, portMAX_DELAY);

        while (s_bt.rx_size > 0) {
            uint32_t fed = feed_controller(s_bt.rx_buf, s_bt.rx_size);
            if (fed == 0) {
                /* No data yet, wait for USB host to send something */
                xSemaphoreTake(s_bt.rx_ready_sem, portMAX_DELAY);
                continue;
            }
            s_bt.rx_buf += fed;
            s_bt.rx_size -= fed;
        }

        /* All requested bytes delivered - notify controller */
        if (s_bt.rx_cb) {
            esp_bt_hci_tl_callback_t cb = s_bt.rx_cb;
            void *arg2 = s_bt.rx_cb_arg;
            s_bt.rx_cb = NULL;
            s_bt.rx_cb_arg = NULL;
            cb(arg2, ESP_BT_HCI_TL_STATUS_OK);
            esp_bt_h4tl_eif_io_event_notify(1);
        }
    }
}

/* HCI TL callbacks registered with BT controller */

static bool hci_tl_init(void)
{
    return true;
}

static void hci_tl_deinit(void)
{
}

static void hci_tl_recv_async(uint8_t *buf, uint32_t size,
                              esp_bt_hci_tl_callback_t callback, void *arg)
{
    s_bt.rx_cb = callback;
    s_bt.rx_cb_arg = arg;
    s_bt.rx_buf = buf;
    s_bt.rx_size = size;
    xSemaphoreGive(s_bt.rx_ready_sem);
}

/* TX buffers - we need persistent buffers because USB transfer is async */
static uint8_t s_evt_tx_buf[HCI_EVT_MAX];
static uint8_t s_acl_tx_buf[HCI_ACL_MAX];

static void hci_tl_send_async(uint8_t *buf, uint32_t size,
                              esp_bt_hci_tl_callback_t callback, void *arg)
{
    /* buf contains an H4 packet: type byte + payload */
    uint8_t type = buf[0];
    uint8_t *payload = buf + 1;
    uint32_t payload_len = size - 1;

    s_bt.tx_cb = callback;
    s_bt.tx_cb_arg = arg;

    switch (type) {
    case H4_EVT:
        if (payload_len >= 2) {
            ESP_LOGI(TAG, "evt code=0x%02x plen=%u",
                     payload[0], payload[1]);
            ESP_LOG_BUFFER_HEXDUMP(TAG, payload, payload_len, ESP_LOG_INFO);
        }
        memcpy(s_evt_tx_buf, payload, payload_len);
        usb_send_event(s_evt_tx_buf, (uint16_t)payload_len);
        break;
    case H4_ACL:
        memcpy(s_acl_tx_buf, payload, payload_len);
        usb_send_acl(s_acl_tx_buf, (uint16_t)payload_len);
        break;
    default:
        ESP_LOGW(TAG, "Unknown H4 type 0x%02x", type);
        break;
    }

    /* Call completion immediately - the data has been copied to our buffer */
    if (s_bt.tx_cb) {
        esp_bt_hci_tl_callback_t cb = s_bt.tx_cb;
        void *a = s_bt.tx_cb_arg;
        s_bt.tx_cb = NULL;
        s_bt.tx_cb_arg = NULL;
        cb(a, ESP_BT_HCI_TL_STATUS_OK);
        esp_bt_h4tl_eif_io_event_notify(1);
    }
}

static void hci_tl_flow_on(void)
{
}

static bool hci_tl_flow_off(void)
{
    return true;
}

static void hci_tl_finish_transfers(void)
{
}

static esp_bt_hci_tl_t s_hci_tl_funcs = {
    ._magic             = ESP_BT_HCI_TL_MAGIC_VALUE,
    ._version           = ESP_BT_HCI_TL_VERSION,
    ._reserved          = 0,
    ._open              = (void *)hci_tl_init,
    ._close             = (void *)hci_tl_deinit,
    ._finish_transfers  = (void *)hci_tl_finish_transfers,
    ._recv              = (void *)hci_tl_recv_async,
    ._send              = (void *)hci_tl_send_async,
    ._flow_on           = (void *)hci_tl_flow_on,
    ._flow_off          = (void *)hci_tl_flow_off,
};

/* TinyUSB device task is handled by esp_tinyusb driver */

/* ------------------------------------------------------------------ */
/*  app_main                                                          */
/* ------------------------------------------------------------------ */

void app_main(void)
{
    esp_err_t ret;

    /* Initialize NVS — used to store PHY calibration data */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Create synchronization primitives */
    s_bt.rx_queue = xQueueCreate(20, sizeof(hci_rx_pkt_t *));
    s_bt.evt_sem = xSemaphoreCreateBinary();
    s_bt.acl_in_sem = xSemaphoreCreateBinary();
    s_bt.rx_ready_sem = xSemaphoreCreateBinary();

    /* Pre-give the TX semaphores so first send doesn't block */
    xSemaphoreGive(s_bt.evt_sem);
    xSemaphoreGive(s_bt.acl_in_sem);

    /* Compose the class driver table: BTH (defined in this file) + GUD
     * (defined in src/gud/gud_usb.c). Done at runtime because C can't
     * value-initialize a static array from another extern const. */
    _app_drivers[0] = (usbd_class_driver_t){
        .name             = "BTH",
        .init             = btd_init,
        .deinit           = btd_deinit,
        .reset            = btd_reset,
        .open             = btd_open,
        .control_xfer_cb  = btd_control_xfer_cb,
        .xfer_cb          = btd_xfer_cb,
    };
#ifdef BOARD_T_DONGLE_S3
    _app_drivers[1] = gud_class_driver;

    /* Bring up the ST7735 panel (T-Dongle S3 pin map). */
    gud_display_panel_init();

    /* Bring up the µSD card slot. Non-fatal on no-card / read errors:
     * the firmware still runs with HCI + GUD even if MSC reports
     * "no medium". */
    msc_sd_init();

    /* Allocate the GUD framebuffer. Internal SRAM is DMA-capable and the
     * 25.6 KB at 80x160 RGB565 fits trivially. esp_lcd_panel_draw_bitmap
     * DMAs straight from this buffer, so it must remain valid for the
     * duration of an SPI flush — handled by the semaphore in
     * write_buffer_cb. */
    void *gud_framebuffer = heap_caps_malloc(GUD_FRAMEBUFFER_BYTES,
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    ESP_ERROR_CHECK(gud_framebuffer ? ESP_OK : ESP_ERR_NO_MEM);
    gud_driver_setup(&gud_display_cfg, gud_framebuffer, NULL);
#endif

    /* Diagnostic: log boot context so dmesg/CDC capture shows whether ROM
     * is honoring force_download_boot. Then clear the bit so a subsequent
     * cold reset (e.g. esptool RTS) doesn't bounce back into download mode. */
    ESP_LOGW(TAG, "boot reset_reason=%d  RTC_CNTL_OPTION1=0x%08lx",
             (int)esp_reset_reason(), (unsigned long)REG_READ(RTC_CNTL_OPTION1_REG));
    REG_WRITE(RTC_CNTL_OPTION1_REG, 0);

    /* Switch USB PHY from Serial/JTAG to OTG */
    usb_phy_config_t phy_conf = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_DEVICE,
        .otg_speed = USB_PHY_SPEED_FULL,
    };
    usb_phy_handle_t phy_hdl = NULL;
    ESP_ERROR_CHECK(usb_new_phy(&phy_conf, &phy_hdl));

    /* Initialize TinyUSB */
    const tusb_rhport_init_t rh_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_FULL,
    };
    tud_rhport_init(0, &rh_init);

    /* Start USB device task */
    extern void usb_device_task(void *arg);
    xTaskCreate(usb_device_task, "usb_dev", 4096, NULL, 5, NULL);

    /* Start HCI RX feeder task */
    xTaskCreate(hci_rx_task, "hci_rx", 4096, NULL, 5, NULL);

    /* Wait for USB to enumerate before starting BT controller */
    ESP_LOGI(TAG, "Waiting for USB enumeration...");
    while (!tud_mounted()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "USB mounted");

    /* Initialize Bluetooth controller */
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    bt_cfg.hci_tl_funcs = &s_hci_tl_funcs;

    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BT controller init failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BT controller enable failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "USB BT HCI controller ready");
}
