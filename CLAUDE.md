# Notes for future Claude sessions

## Quick orientation

This is `daniel-k/lilygo-tdongle-s3-firmware`: an ESP32-S3 firmware for the LilyGo T-Dongle S3 that exposes the dongle's BLE controller, µSD slot, and 0.96" LCD over USB as standard Bluetooth HCI, USB Mass Storage, and Generic USB Display functions. Same codebase also builds a Bluetooth-HCI-only image for plain ESP32-S3 boards (e.g. Super Mini). README covers user-facing details.

Two PlatformIO build envs in `platformio.ini`:
- `t_dongle_s3` (default): full firmware. `BOARD_T_DONGLE_S3` defined.
- `super_mini`: BT-only. `BOARD_SUPER_MINI` defined.

Function-specific code (MSC, GUD) is gated by `#ifdef BOARD_T_DONGLE_S3` in `src/main.c`, `src/tusb_config.h`, `src/msc_sd.c`, and every `src/gud/*.c` so the Super Mini binary doesn't drag in the SDMMC/SPI panel/GUD class driver. Net ~96 KB smaller.

USB layout, T-Dongle build (4 interfaces, two IADs):
- IF0: BT HCI (class 0xE0/0x01/0x01), 3 endpoints
- IF1: dummy SCO (class 0xE0/0x01/0x01), 0 endpoints — satisfies btusb's N+1 quirk
- IF2: MSC (class 0x08/0x06/0x50), 2 bulk endpoints — backed by µSD
- IF3: GUD vendor (class 0xFF), 1 bulk OUT endpoint

USB layout, Super Mini build (2 interfaces, one IAD): just IF0+IF1 from the above.

**HCI must be at IF0** because btusb's `alloc_ctrl_urb()` hardcodes `dr->wIndex = 0` for HCI command control transfers — moving HCI elsewhere routes commands to whichever driver owns IF0, which stalls EP0 and the host fails opcode 0x0c03 (HCI Reset) with -EPIPE. Don't reorder without re-reading `docs/msc-sd-card.md` for the full constraint chain.

## Dev workflow — flashing without BOOT-button presses

```sh
python3 tools/reboot-download.py && pio run -t upload && \
  esptool --port /dev/ttyACM0 --after watchdog-reset run
```

This is the autonomous flash loop. You can run it yourself; no human intervention needed, no sudo, no replug. It works because:

1. **`reboot-download.py`** sends HCI vendor opcode 0xFCFE → firmware sets `force_download_boot` and resets. The script polls for the device to re-enumerate as `303a:0009`. If it doesn't (often, see "Gotchas" below), it falls back to cutting VBUS via the parent hub's per-port disable file.
2. **`pio run -t upload`** flashes via the ROM bootloader.
3. **`esptool --after watchdog-reset run`** triggers RTC-WDT system reset; this boards' RTS-line reset doesn't work on USB-OTG, but the watchdog reset does.

`pio run` defaults to the `t_dongle_s3` env. To work on the Super Mini build, pass `-e super_mini` to every `pio` invocation.

## Required setup (likely already done on this machine)

- `pip install pyusb Pillow` (in `venv/`) — pyusb for `tools/reboot-download.py`, Pillow for `tools/draw-text.py`.
- `udev/80-esp32s3-hci-dev.rules` installed in `/etc/udev/rules.d/` — only rule we need now. It grants libusb + tty access without sudo and lets us toggle the parent hub's per-port `disable` file. There used to be more rules (CDC class binding workarounds, gud DRM card seat handling); all obsolete now that we adopted the `1d50:614d` PID, added a dummy SCO IF1, and emit a Microsoft non-desktop EDID extension.
- The dongle must be plugged into a USB hub with per-port VBUS switching. The user's machine has a VIA Labs hub (VID 2109) wired in front of the dongle for exactly this. Laptop root hubs typically don't actually cut VBUS even though sysfs lets you write to `disable`.

## What you can do without asking

- `dmesg.log` in the repo root is **live-streamed** from the actual kernel ring buffer — read it any time, no sudo needed. Use it to watch USB enumeration / errors during a reset cycle.
- The port disable file at `/sys/bus/usb/devices/3-1/3-1:1.0/3-1-port<N>/disable` is writable by your user (uucp group). Writing 1 cuts VBUS, 0 restores. Hub topology may shift; the script auto-discovers the right path.
- `tools/reboot-download.py` recovers from any state (firmware running, ROM bootloader, GONE) — call it whenever you need the device in download mode.

## Important gotchas (don't relearn the hard way)

1. **The ESP32-S3 OTG ROM bootloader is broken.** It silently ignores `force_download_boot` after most software resets. See `docs/esp32s3-otg-rom-bug.md` for the full writeup — symptom, references, what we tried, what works, and what NOT to retry. The short version: chip-side fix alone is not solvable; our solution combines firmware peripheral reset with a host-side VBUS-cycle fallback.

2. **`esp_restart()` deadlocks** in our context: its IDF shutdown handlers try to deinit the BT controller, which blocks on our HCI TL semaphores. Use `esp_rom_software_reset_system()` (or the RTC-WDT hijack) for any reset from inside firmware.

3. **In `tools/reboot-download.py`, do NOT call `detach_kernel_driver()`.** Earlier the script did this and the EP0 control transfer never reached the firmware. EP0 transfers work fine while drivers hold interfaces; detaching breaks the routing.

4. **`reboot-download.py` sends the vendor opcode with `wIndex=0`**, which routes via TinyUSB to the BTH class driver only because BTH owns IF0. If a future change ever puts a different driver at IF0 (and HCI won't even work then anyway — see "HCI must be at IF0" above), the firmware-side reset path stops working. Recovery: send the same control transfer with `wIndex=<HCI_IF_NUM>` (we did this exact maneuver while bringing up MSC — see `docs/msc-sd-card.md` "Recovering from a bad descriptor").

5. **`/dev/ttyACM0` ambiguity.** When the device is in ROM bootloader, ttyACM0 is the ROM's CDC ACM (esptool talks SLIP over it). The running firmware no longer has a CDC log channel — that was removed when MSC was added.

6. **USB IDs to know:**
   - `1d50:614d` — running firmware. Both T-Dongle and Super Mini builds. Adopted from the OpenHardware.io / GUD USB Display allocation so the kernel `gud` driver auto-binds (T-Dongle build only has a vendor IF for it to bind to).
   - `303a:0009` — ROM bootloader on USB-OTG (script-triggered path).
   - `303a:1001` — ROM bootloader on USB-Serial-JTAG (BOOT-button path).
   The script accepts either ROM PID as success.

7. **`btusb` N+1 quirk is satisfied by a dummy SCO interface** at IF1. btusb_probe unconditionally claims interface N+1 of an HCI device looking for an ISOC SCO alt-setting and fails the entire probe if it can't get it. Our IF1 is a do-nothing wireless-class interface with zero ISOC endpoints — btusb claims it without ever activating ISOC and `/dev/hciN` comes up cleanly.

## When the chip is wedged ("GONE" from `lsusb`)

Run `tools/reboot-download.py` — its VBUS-cycle fallback brings the chip back. If even that fails (very rare), it means the hub itself isn't responding; try `echo 1 > /sys/bus/usb/devices/3-1/3-1:1.0/3-1-port<N>/disable; sleep 2; echo 0 > ...` directly. As a last resort, ask the user to physically replug.

## Testing the functions

- **BT HCI**: `bluetoothctl list` shows the dongle as a controller with manufacturer 0x02e5 (Espressif). `dmesg.log` lists `Bluetooth: MGMT ver` after enumeration. The hciN index depends on what other controllers are present.
- **MSC** (T-Dongle only): `lsblk` shows `/dev/sdX`, GNOME/udisks auto-mounts FAT partitions to `/run/media/$USER/`. dmesg shows `usb-storage 3-1.X:1.2: USB Mass Storage device detected`.
- **GUD** (T-Dongle only): The host's `gud` driver auto-binds to IF3 (matches `1d50:614d` in its static id_table). `tools/draw-text.py -t "..."` is the easiest end-to-end smoke test — it claims IF3 via libusb, sends the GUD setup sequence, renders text via Pillow, and pushes a single SET_BUFFER + bulk OUT. ~33 ms wall time. Requires gud to NOT be bound to IF3 (libusb claim conflicts with kernel driver). If gud is bound, unbind: `echo '<port>:1.3' | sudo tee /sys/bus/usb/drivers/gud/unbind`.
- `modetest -M gud` enumerates the connector. `modetest -M gud -s <conn>@<crtc>:80x160` (from a TTY or with mutter killed) draws a test pattern via real DRM atomic — but Linux 6.19 has a NULL-deref in `gud_plane_atomic_update` when modetest exits cleanly (TODO #2).
- The DRM card is exposed as **non-desktop** via a Microsoft VSDB in our EDID, so GNOME / mutter ignore it. As a side effect there is **no `/dev/fbN`** for our card: `drm_fb_helper` filters non-desktop connectors out of fbdev-emulation, so plug-in dmesg always shows `gud …: [drm] Cannot find any crtc or sizes` (twice — that's expected, not a bug). Use `/dev/dri/cardN` directly (modetest, libdrm) or `tools/draw-text.py` (libusb, bypasses the kernel driver entirely).

## What's checked in / expected workflow

- `src/main.c` — BT HCI bridge, USB descriptors, TinyUSB class driver registration, app_main. Conditional code blocks gated by `BOARD_T_DONGLE_S3`.
- `src/msc_sd.{c,h}` — SDMMC slot 1 (4-bit) bridge to TinyUSB MSC class. T-Dongle only (entire .c body is `#ifdef BOARD_T_DONGLE_S3`).
- `src/gud/` — GUD function. T-Dongle only. `gud.{c,h}` lifted verbatim from notro/gud-pico (MIT). `gud_usb.{c,h}` adapted from gud-pico's driver.c. `gud_display.{c,h}` is the T-Dongle ST7735 backend (SPI panel + LEDC backlight + write_buffer_cb byte-swap). `gud_edid_ext.{c,h}` builds the 256-byte EDID with Microsoft non-desktop VSDB. `esp_lcd_st7735.{c,h}` lifted verbatim from LilyGo T-Dongle-S3 (MIT). All .c files wrapped in `#ifdef BOARD_T_DONGLE_S3`.
- `tools/` for host-side scripts: `reboot-download.py` (autonomous flash), `draw-text.py` (end-to-end smoke test).
- `udev/80-esp32s3-hci-dev.rules` for libusb + per-port disable access.
- `licenses/` and `THIRDPARTY.md` document upstream licenses for vendored sources.
- Build is PlatformIO + ESP-IDF. `pio run` is incremental. Default env is `t_dongle_s3`; pass `-e super_mini` for the BT-only build.
- `venv/` exists with pyusb + Pillow installed.

## When you finish a task

- Don't proactively commit unless asked. The user prefers to review.
- PR descriptions: summary + motivation only, no "Test plan" section (per global preferences).
