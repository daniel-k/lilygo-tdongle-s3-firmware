// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Daniel Roussel
/*
 * USB HID single-button gamepad bridge for GPIO0 (BOOT button).
 *
 * GPIO0 is also the chip's strapping pin — held low at reset triggers
 * ROM download mode. After boot it's a plain input; we configure it as
 * input + internal pull-up (the dongle has an external pull-up already,
 * pressing pulls it to GND through the BOOT button), so reading is
 * non-disruptive and the strapping behaviour at the next reset is
 * unchanged.
 *
 * Debouncing: simple "wait 20 ms after a transition, re-sample, commit
 * if still different". Adequate for human button presses (≤ ~10 Hz).
 * The poll loop runs every 10 ms which gives a worst-case 30 ms latency
 * from press to USB report — well below the threshold of perception.
 *
 * Compiled in only on T-Dongle S3 builds. Super Mini omits this file.
 */

#ifdef BOARD_T_DONGLE_S3

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "tusb.h"
#include "class/hid/hid.h"
#include "class/hid/hid_device.h"

#include "hid_button.h"

#define BUTTON_GPIO    GPIO_NUM_0
#define DEBOUNCE_MS    20
#define POLL_MS        10

static const char *TAG = "HID_BTN";

/* Single-button gamepad. Linux's hid-generic / evdev pair surfaces the
 * Button-1 usage as BTN_TRIGGER on the resulting /dev/input/eventN.
 * Keep the report to one byte: bit 0 = button state, bits 1..7 padded
 * to byte boundary with a constant input. */
uint8_t const hid_report_desc[] = {
    0x05, 0x01,        /* Usage Page (Generic Desktop)         */
    0x09, 0x05,        /* Usage (Game Pad)                     */
    0xA1, 0x01,        /* Collection (Application)             */
    0x05, 0x09,        /*   Usage Page (Button)                */
    0x19, 0x01,        /*   Usage Minimum (Button 1)           */
    0x29, 0x01,        /*   Usage Maximum (Button 1)           */
    0x15, 0x00,        /*   Logical Minimum (0)                */
    0x25, 0x01,        /*   Logical Maximum (1)                */
    0x95, 0x01,        /*   Report Count (1)                   */
    0x75, 0x01,        /*   Report Size (1)                    */
    0x81, 0x02,        /*   Input (Data, Var, Abs)             */
    0x95, 0x01,        /*   Report Count (1)                   */
    0x75, 0x07,        /*   Report Size (7)                    */
    0x81, 0x03,        /*   Input (Const, Var, Abs)  - padding */
    0xC0,              /* End Collection                       */
};

_Static_assert(sizeof(hid_report_desc) == HID_REPORT_DESC_LEN,
               "HID_REPORT_DESC_LEN must match sizeof(hid_report_desc)");

static void hid_button_task(void *arg)
{
    (void)arg;

    int last_stable = 1; /* released (pull-up high) */

    while (1) {
        int level = gpio_get_level(BUTTON_GPIO);
        if (level != last_stable) {
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
            int level2 = gpio_get_level(BUTTON_GPIO);
            if (level2 != last_stable) {
                last_stable = level2;
                /* level2 == 0 means BOOT button pressed (pulled to GND).
                 * Send a fresh report whether host has subscribed or
                 * not: tud_hid_ready() guards against pre-mount or
                 * unconfigured states. */
                uint8_t report = (level2 == 0) ? 0x01 : 0x00;
                if (tud_hid_ready()) {
                    tud_hid_report(0 /* no report id */, &report, sizeof(report));
                }
                ESP_LOGI(TAG, "BOOT %s", level2 == 0 ? "down" : "up");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void hid_button_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    xTaskCreate(hid_button_task, "hid_btn", 2048, NULL, 4, NULL);
    ESP_LOGI(TAG, "BOOT button HID polling task started on GPIO%d",
             BUTTON_GPIO);
}

/* ------------------------------------------------------------------ */
/*  TinyUSB HID class driver callbacks                                */
/*                                                                    */
/*  CFG_TUD_HID=1 in tusb_config.h enables the built-in HID class     */
/*  driver; it dispatches to these weak hooks for our descriptor and  */
/*  request handlers. We only emit IN reports (button state) so       */
/*  GET_REPORT is unnecessary (host falls back to interrupt IN), and  */
/*  SET_REPORT is a no-op (no LEDs / output reports).                 */
/* ------------------------------------------------------------------ */

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return hid_report_desc;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)instance; (void)report_id; (void)report_type;
    (void)buffer; (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance; (void)report_id; (void)report_type;
    (void)buffer; (void)bufsize;
}

#endif /* BOARD_T_DONGLE_S3 */
