# Notes: ESP32-S3 OTG ROM bootloader doesn't honor `force_download_boot` after software reset

## Symptom

From running firmware on USB-OTG, setting `RTC_CNTL_OPTION1.force_download_boot=1` and triggering a software reset *should* drop the chip into the ROM bootloader's CDC-ACM download mode (USB ID `303a:0009`). On the ESP32-S3 it doesn't, reliably:

- The chip resets (devnum changes, USB disconnects + reconnects in `dmesg`).
- Sometimes the ROM honors `force_download_boot` and `303a:0009` appears.
- Often the ROM silently falls through to the app — chip comes back as `303a:4002` (running firmware) instead.
- Sometimes the chip wedges entirely: `dmesg` shows repeated `device descriptor read/64, error -71` or `error -110` and the device disappears from `lsusb` until physically replugged.

The first reset after a true cold power-on usually works; subsequent software resets within the same power session don't.

## Public references confirming the bug

- micropython#13402 — "ESP32-S3: how to enter bootloader without pressing BOOT button (`machine.bootloader()` doesn't work)?" Discussion documents the same failure mode and concludes there's no software-only fix without changing peripherals.
- esp-idf#13287 — "ESP32-S3 can not exit from USB Serial/JTAG bootloader (IDFGH-12237)". Related: Espressif's own boot-flag handling on this chip is buggy.
- arduino-esp32#6762 — "esp32-s3 does not reset after upload". Same root cause.
- MicroPython PR #15108 worked around this by switching the entire stack from TinyUSB on USB-OTG to USB-Serial-JTAG (USJ); this is what `machine.bootloader()` does today on ESP32-S3 in MicroPython.

The USJ workaround is closed to us because USJ doesn't carry our HCI device class — we need OTG.

## What we tried that didn't fix it on its own

| Approach | Outcome |
|---|---|
| `esp_restart()` | Deadlocks on the BT controller's shutdown handler, which blocks against our HCI TL semaphores. Chip never resets. |
| `esp_restart_noos()` | CPU-only reset; the ROM bootloader path doesn't run, chip just resumes the app. |
| `esp_rom_software_reset_system()` | Works after a cold replug, then unreliable on subsequent cycles. |
| RTC-WDT hijack (esptool's `--after watchdog-reset` mechanism) | Same — chip resets but ROM doesn't honor `force_download_boot`. |
| USB phy switch OTG → USJ before reset (the MicroPython recipe verbatim) | Same. |
| `chip_usb_set_persist_flags(USBDC_PERSIST_ENA)` / `USBDC_BOOT_DFU` | No improvement, sometimes regression. |
| `tud_disconnect()` + `usb_dc_prepare_persist()` | Actively made cold-replug case worse. |
| Kernel-side port reset via `/sys/.../disable` on the laptop's xHCI root hub | Doesn't actually cut VBUS; chip never sees a real power cycle. |

## What works (current solution)

A combination of three things, none of which is sufficient on its own:

1. **Firmware: peripheral-reset both USB blocks before the chip reset.** Set `SYSTEM_USB_RST` (bit 23) and `SYSTEM_USB_DEVICE_RST` (bit 10) in `SYSTEM_PERIP_RST_EN1_REG`, hold for ~10 ms, then clear. Without this, the USB peripheral retains state from the running app and the ROM's USB enum fails with `-71/-110` on the cycles that *would* have entered download mode. With it, the re-enum is clean (~250 ms) when the ROM does honor `force_download_boot`. Then call `esp_rom_software_reset_system()`.

2. **Host-side fallback: VBUS power cycle via the parent hub.** When the chip-side reset doesn't enter ROM mode within ~8 s, `tools/reboot-download.py` writes `1` then `0` to the parent hub's per-port `disable` sysfs file. This actually cuts VBUS to the dongle (on a hub that supports per-port power switching), causing a true cold boot. `force_download_boot` is preserved in RTC across the brief power-off, and the cold-boot path through the ROM *does* honor it reliably.

3. **Firmware: clear `force_download_boot` at app startup.** Otherwise a stray RTS-style reset (esptool's default) bounces the chip back into download mode unintentionally on the next boot.

## Hardware requirement for full automation

The host needs a USB hub with **per-port power switching**. Most laptop xHCI root hubs don't have this — writing to the port's `disable` sysfs file kills the data lines but VBUS stays on, so the chip never cold-boots and the workaround fails. External hubs based on VIA Labs (VID `2109`), Genesys Logic, GL852, etc. typically do support it.

Without such a hub the script-side reset still works most of the time (the chip-side path succeeds on a fraction of attempts); when it doesn't, you'll need to replug manually.

## Don't go back down these paths

If you find yourself trying yet another firmware-side reset variant or `chip_usb_set_persist_flags()` combination, stop. We've been there exhaustively — the chip-side fix isn't a solved problem and may not be solvable without changing USB peripherals. Improvements should focus on the host-side fallback (faster recovery, better detection) or accept the chip's behavior as fixed.
