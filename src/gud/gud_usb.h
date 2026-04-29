// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2021-2024 Noralf Trønnes
/*
 * GUD <-> TinyUSB class driver glue, ported from notro/gud-pico.
 * Exposes a usbd_class_driver_t to be composed with the existing BT HCI
 * class driver in main.c's usbd_app_driver_get_cb(). See
 * licenses/gud-pico-LICENSE.md for the upstream MIT terms.
 */

#ifndef GUD_USB_H_
#define GUD_USB_H_

#include "device/usbd.h"
#include "device/usbd_pvt.h"

#include "gud.h"

/* GUD bulk OUT max-packet size (USB Full-Speed). */
#define GUD_BULK_OUT_SIZE 64

/* The class driver descriptor — registered alongside the BT HCI driver. */
extern usbd_class_driver_t const gud_class_driver;

/* Binds a gud_display configuration and the host-pixel framebuffer to the
 * USB layer. Must be called before USB enumerates. compress_buf may be NULL
 * (no compression supported in this port). */
void gud_driver_setup(const struct gud_display *disp, void *framebuffer, void *compress_buf);

/* Returns true if no GUD request has been received in the last `timeout_secs`
 * seconds while the controller is enabled. The pico-pico port uses this as a
 * kind of host-side watchdog: when GUD_CONNECTOR_FLAGS_POLL_STATUS is set the
 * host pings every 10s, so a 15s drought means we lost the host. Currently
 * unused on ESP32-S3 but kept for parity. */
bool gud_driver_req_timeout(unsigned int timeout_secs);

#endif
