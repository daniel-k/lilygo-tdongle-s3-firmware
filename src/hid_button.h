// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Daniel Roussel
/*
 * USB HID single-button gamepad bridge for the LilyGo T-Dongle S3 BOOT0
 * button (GPIO0). Linux surfaces it as /dev/input/eventN reporting
 * BTN_TRIGGER on press / release; nothing is injected as keystrokes.
 *
 * Compiled in only on T-Dongle S3 builds (CFG_TUD_HID gated identically
 * in tusb_config.h). The Super Mini also has a BOOT button on GPIO0,
 * but we don't expose it there to keep the BT-only image minimal.
 */

#ifndef HID_BUTTON_H_
#define HID_BUTTON_H_

/* Length of the HID report descriptor returned by tud_hid_descriptor_report_cb.
 * Exposed as a compile-time constant so the HID class descriptor in
 * main.c's desc_configuration[] can embed wDescriptorLength without taking
 * sizeof of an array defined in another translation unit. The actual
 * report descriptor lives in hid_button.c; a _Static_assert guards the
 * length against drift. */
#define HID_REPORT_DESC_LEN 29

/* Configure GPIO0 as input with internal pull-up and spawn the polling
 * task that emits HID reports on press / release. Must be called after
 * tud_rhport_init() — the task tries to send via tud_hid_report() and
 * silently drops when not mounted, but starting it after USB is up is
 * cleaner. */
void hid_button_init(void);

#endif
