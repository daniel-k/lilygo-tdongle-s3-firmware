#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

#define CFG_TUSB_OS              OPT_OS_FREERTOS
#define CFG_TUSB_RHPORT0_MODE    OPT_MODE_DEVICE
#define CFG_TUD_ENDPOINT0_SIZE   64

#define CFG_TUD_BTH              0
#define CFG_TUD_CDC              0
#ifdef BOARD_T_DONGLE_S3
/* USB Mass Storage backed by the T-Dongle's onboard µSD slot. Exposes the
 * card as a generic removable drive — host treats it like any USB stick.
 * Endpoint buffer sized to one SD sector (512 B is the universal SD sector
 * size); SCSI READ/WRITE 10 commands service one sector at a time via the
 * TinyUSB MSC callbacks. */
#define CFG_TUD_MSC              1
#define CFG_TUD_MSC_EP_BUFSIZE   512
/* USB HID single-button gamepad bridging the T-Dongle's BOOT button
 * (GPIO0) to the host. Endpoint buffer is plenty for our 1-byte input
 * report; CFG_TUD_HID=1 enables the built-in TinyUSB HID class driver
 * whose callbacks live in src/hid_button.c. */
#define CFG_TUD_HID              1
#define CFG_TUD_HID_EP_BUFSIZE   8
#else
#define CFG_TUD_MSC              0
#define CFG_TUD_HID              0
#endif
#define CFG_TUD_MIDI             0
#define CFG_TUD_VENDOR           0
#define CFG_TUD_ECM_RNDIS        0
#define CFG_TUD_NCM              0
#define CFG_TUD_DFU              0
#define CFG_TUD_DFU_RUNTIME      0
#define CFG_TUD_USBTMC           0
#define CFG_TUD_AUDIO            0
#define CFG_TUD_VIDEO            0

#endif
