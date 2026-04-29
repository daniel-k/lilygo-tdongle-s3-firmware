// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2021-2024 Noralf Trønnes
/*
 * GUD <-> TinyUSB class driver glue.
 *
 * Adapted from notro/gud-pico (libraries/gud_pico/driver.c, commit 984a171).
 * See licenses/gud-pico-LICENSE.md for the upstream MIT terms.
 * Changes vs. the upstream RP2040 port:
 *   - timing: time_us_64() → esp_timer_get_time()
 *   - logging: ESP_LOGx instead of printf, panic() removed
 *   - LZ4 decompression path elided (no compression in this v1 port)
 *   - usbd_app_driver_get_cb removed; main.c composes BTH + GUD class drivers
 *
 * GUD uses USB vendor-type control requests on the IF3 endpoint. TinyUSB
 * routes vendor-type control transfers to tud_vendor_control_xfer_cb()
 * unconditionally — see usbd.c process_control_request() — so we override
 * that weak hook regardless of CFG_TUD_VENDOR. The class driver's own
 * control_xfer_cb is therefore only relevant for class-type requests, which
 * GUD doesn't issue, so it returns false.
 *
 * Compiled in only on T-Dongle S3 builds — plain ESP32-S3 boards have no
 * GUD function and main.c skips registering this class driver.
 */

#ifdef BOARD_T_DONGLE_S3

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "tusb.h"
#include "device/usbd.h"
#include "device/usbd_pvt.h"

#include "gud.h"
#include "gud_edid_ext.h"
#include "gud_usb.h"

static const char *TAG = "GUD";

/* 256 bytes = base EDID block + one CEA-861 extension. The extension carries
 * the Microsoft "specialized monitor" VSDB which sets the kernel's `non-
 * desktop` connector property — see src/gud/gud_edid_ext.c. Other GUD GET
 * requests fit in much less, but this is the worst case we're willing to
 * service in one control transfer. */
#define GUD_CTRL_REQ_BUF_SIZE   256

/* Chunk size for the bulk OUT receive. The TinyUSB ESP32-S3 (DWC2) slave-mode
 * RX FIFO seems unable to sustain back-to-back EP5 OUT packets across the full
 * frame in one usbd_edpt_xfer() call: larger requests stall after some packets
 * and the host URB times out. Capping per-call size to 1024 (matching the
 * BT ACL OUT buffer that's been working) and re-arming after each chunk works
 * around it. The xfer_cb already has chunk-continuation logic; we just lower
 * the chunk size here. */
#define GUD_EDPT_XFER_MAX_SIZE  1024

#define min(a,b)    (((a) < (b)) ? (a) : (b))

typedef struct {
    uint8_t itf_num;
    uint8_t ep_out;

    uint8_t *buf;
    uint32_t xfer_len;
    uint32_t len;
    uint32_t offset;
} gud_interface_t;

CFG_TUSB_MEM_SECTION static gud_interface_t _gud_itf;

static const struct gud_display *_display;
static uint8_t *_framebuffer;
static uint8_t *_compress_buf;

CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN static uint8_t _ctrl_req_buf[GUD_CTRL_REQ_BUF_SIZE];
CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN static uint8_t _status;

static bool _controller_enabled;
static int64_t _req_timestamp;

static void gud_driver_init(void)
{
    memset(&_gud_itf, 0, sizeof(_gud_itf));
}

static bool gud_driver_deinit(void)
{
    return true;
}

static void gud_driver_reset(uint8_t rhport)
{
    (void) rhport;
}

static uint16_t gud_driver_open(uint8_t rhport, tusb_desc_interface_t const *itf_desc, uint16_t max_len)
{
    if (itf_desc->bInterfaceClass != TUSB_CLASS_VENDOR_SPECIFIC) {
        return 0;
    }

    uint16_t const drv_len = (uint16_t)(sizeof(tusb_desc_interface_t) +
                                        itf_desc->bNumEndpoints * sizeof(tusb_desc_endpoint_t));
    if (max_len < drv_len) {
        return 0;
    }

    tusb_desc_endpoint_t const *desc_ep =
        (tusb_desc_endpoint_t const *) tu_desc_next(itf_desc);
    if (!usbd_edpt_open(rhport, desc_ep)) {
        return 0;
    }

    _gud_itf.ep_out  = desc_ep->bEndpointAddress;
    _gud_itf.itf_num = itf_desc->bInterfaceNumber;

    ESP_LOGI(TAG, "opened: itf=%u ep_out=0x%02x", _gud_itf.itf_num, _gud_itf.ep_out);

    return drv_len;
}

static bool gud_driver_control_request(uint8_t rhport, tusb_control_request_t const *req)
{
    uint16_t wLength;
    int ret;

    wLength = min(req->wLength, sizeof(_ctrl_req_buf));

    _req_timestamp = esp_timer_get_time();

    if (req->bmRequestType_bit.recipient != TUSB_REQ_RCPT_INTERFACE ||
        req->bmRequestType_bit.type != TUSB_REQ_TYPE_VENDOR) {
        return false;
    }

    /* TinyUSB dispatches every vendor control transfer to this hook
     * regardless of target interface. Reject requests aimed at any other
     * interface — otherwise the broad-binding gud kernel driver (which
     * may have auto-bound to IF0/IF2 too) sends a SET_BUFFER for IF0,
     * we incorrectly prime EP5 OUT here, the bulk data goes to the wrong
     * endpoint, and EP5 OUT stays stuck primed for the real IF3 flush. */
    if (req->wIndex != _gud_itf.itf_num) {
        return false;
    }

    if (req->bmRequestType_bit.direction) {
        if (req->bRequest == GUD_REQ_GET_STATUS) {
            return tud_control_xfer(rhport, req, &_status, sizeof(_status));
        }

        _status = 0;
        if (req->bRequest == GUD_REQ_GET_CONNECTOR_EDID) {
            /* Hand off to our extension generator so the EDID carries the
             * Microsoft non-desktop VSDB; everything else flows through the
             * upstream gud.c path unchanged. */
            ret = gud_get_edid_extended(_display, _ctrl_req_buf, wLength);
        } else {
            ret = gud_req_get(_display, req->bRequest, req->wValue, _ctrl_req_buf, wLength);
        }
        if (ret < 0) {
            _status = -ret;
            return false;
        }

        return tud_control_xfer(rhport, req, _ctrl_req_buf, ret);
    } else {
        _status = 0;

        if (!wLength) {
            ret = gud_req_set(_display, req->bRequest, req->wValue, _ctrl_req_buf, 0);
            if (ret < 0) {
                _status = -ret;
                return false;
            }
        }

        return tud_control_xfer(rhport, req, _ctrl_req_buf, wLength);
    }
}

static bool gud_driver_bulk_xfer(uint8_t rhport, uint8_t *buf, uint32_t xfer_len, uint32_t len)
{
    if (usbd_edpt_busy(rhport, _gud_itf.ep_out)) {
        return false;
    }

    if (buf) {
        if (!xfer_len || !len) return false;
        _gud_itf.offset   = 0;
        _gud_itf.buf      = buf;
        _gud_itf.len      = len;
        _gud_itf.xfer_len = xfer_len;
    } else {
        _gud_itf.offset += GUD_EDPT_XFER_MAX_SIZE;
        buf       = _gud_itf.buf + _gud_itf.offset;
        xfer_len  = _gud_itf.xfer_len - _gud_itf.offset;
    }

    if (xfer_len > GUD_EDPT_XFER_MAX_SIZE) {
        xfer_len = GUD_EDPT_XFER_MAX_SIZE;
    }

    return usbd_edpt_xfer(rhport, _gud_itf.ep_out, buf, (uint16_t)xfer_len);
}

static bool gud_driver_control_complete(uint8_t rhport, tusb_control_request_t const *req)
{
    uint16_t wLength;

    wLength = min(req->wLength, sizeof(_ctrl_req_buf));

    if (!req->bmRequestType_bit.direction) {
        int ret = gud_req_set(_display, req->bRequest, req->wValue, _ctrl_req_buf, wLength);
        if (ret < 0) {
            _status = -ret;
            return false;
        }

        if (req->bRequest == GUD_REQ_SET_CONTROLLER_ENABLE) {
            _controller_enabled = *(uint8_t *)_ctrl_req_buf;
        }

        if (req->bRequest == GUD_REQ_SET_BUFFER) {
            const struct gud_set_buffer_req *buf_req =
                (const struct gud_set_buffer_req *)_ctrl_req_buf;
            uint32_t len;
            void *buf;

            if (buf_req->compression) {
                buf = _compress_buf;
                len = buf_req->compressed_length;
            } else {
                buf = _framebuffer;
                len = buf_req->length;
            }

            return gud_driver_bulk_xfer(rhport, buf, len, buf_req->length);
        }

        if (req->bRequest == GUD_REQ_SET_STATE_CHECK &&
            (_display->flags & GUD_DISPLAY_FLAG_FULL_UPDATE)) {
            if (!usbd_edpt_busy(rhport, _gud_itf.ep_out)) {
                const struct gud_state_req *sreq = (const struct gud_state_req *)_ctrl_req_buf;
                uint32_t len = gud_get_buffer_length(sreq->format, _display->width, _display->height);
                if (!len) {
                    _status = GUD_STATUS_INVALID_PARAMETER;
                    return false;
                }
                return gud_driver_bulk_xfer(rhport, _framebuffer, len, len);
            }
        }
    }

    return true;
}

/* TinyUSB calls this for every vendor-type control transfer (see usbd.c
 * process_control_request). It overrides the weak default in TinyUSB. */
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req)
{
    if (stage == CONTROL_STAGE_SETUP) {
        return gud_driver_control_request(rhport, req);
    } else if (stage == CONTROL_STAGE_DATA) {
        return gud_driver_control_complete(rhport, req);
    }
    return true;
}

static bool gud_driver_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req)
{
    (void)rhport; (void)stage; (void)req;
    return false;
}

static bool gud_driver_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes)
{
    if (ep_addr != _gud_itf.ep_out) {
        return false;
    }
    if (result != XFER_RESULT_SUCCESS) {
        ESP_LOGW(TAG, "bulk xfer failed: result=%d", (int)result);
        return false;
    }

    if (xferred_bytes != (_gud_itf.xfer_len - _gud_itf.offset)) {
        if (xferred_bytes != GUD_EDPT_XFER_MAX_SIZE) {
            ESP_LOGW(TAG, "short xfer: got %u expected %u",
                     (unsigned)xferred_bytes,
                     (unsigned)(_gud_itf.xfer_len - _gud_itf.offset));
            return false;
        }
        return gud_driver_bulk_xfer(rhport, NULL, 0, 0);
    }

    if (_gud_itf.xfer_len != _gud_itf.len) {
        /* Compressed transfer — would call LZ4_decompress_safe() here. We
         * advertise compression=0, so the host should never send this. */
        ESP_LOGW(TAG, "compressed xfer received but LZ4 not built in");
        _status = GUD_STATUS_ERROR;
        return false;
    }

    gud_write_buffer(_display, _framebuffer);

    if (_display->flags & GUD_DISPLAY_FLAG_FULL_UPDATE) {
        return gud_driver_bulk_xfer(rhport, _framebuffer, _gud_itf.xfer_len, _gud_itf.xfer_len);
    }

    return true;
}

bool gud_driver_req_timeout(unsigned int timeout_secs)
{
    if (!_controller_enabled) return false;
    int64_t timeout = _req_timestamp + ((int64_t)timeout_secs * 1000 * 1000);
    return esp_timer_get_time() > timeout;
}

void gud_driver_setup(const struct gud_display *disp, void *framebuffer, void *compress_buf)
{
    _display       = disp;
    _framebuffer   = framebuffer;
    _compress_buf  = compress_buf;
}

usbd_class_driver_t const gud_class_driver = {
#if CFG_TUSB_DEBUG >= 2
    .name             = "GUD",
#endif
    .init             = gud_driver_init,
    .deinit           = gud_driver_deinit,
    .reset            = gud_driver_reset,
    .open             = gud_driver_open,
    .control_xfer_cb  = gud_driver_control_xfer_cb,
    .xfer_cb          = gud_driver_xfer_cb,
    .sof              = NULL,
};

#endif /* BOARD_T_DONGLE_S3 */
