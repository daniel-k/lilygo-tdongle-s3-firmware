# LilyGo T-Dongle S3 firmware

Turns a [LilyGo T-Dongle S3](https://lilygo.cc/products/t-dongle-s3) into a
USB composite device that exposes the dongle's onboard hardware as four
standard USB functions:

- **Bluetooth HCI controller** — the ESP32-S3's BLE controller, presented
  to the host as a plain BT HCI device (`/dev/hciN`), recognised by BlueZ
  / `bluetoothctl` / `btmgmt` with no driver patches.
- **USB Mass Storage** — the onboard µSD slot exposed as a removable
  drive (`/dev/sdX`). Driver-less on every modern OS; the dongle becomes
  a regular USB stick.
- **Generic USB Display** — the 0.96" 80×160 ST7735 LCD driven via the
  in-tree Linux [`gud`](https://github.com/notro/gud) driver
  (`/dev/dri/cardN`).
- **HID gamepad** — the BOOT button (GPIO0) bridged to the host as a
  single-button gamepad (`/dev/input/eventN` + `/dev/input/jsN`,
  reporting `BTN_A` on press / release). Lets you wire the button to
  arbitrary host actions via `udev` / `uinput` / `evtest`.

When plugged into a recent Linux host with no special configuration,
all four Just Work: `btusb` binds the HCI half, `usb-storage` binds the
storage interface, `gud` binds the display interface, and `usbhid`
binds the gamepad. No `new_id`, no kernel patches, no manual unbinds.

The firmware also builds for plain ESP32-S3 boards (e.g. ESP32-S3 Super
Mini) as a Bluetooth-HCI-only image — no LCD, no µSD, just the BLE
controller exposed over USB. Useful as a small dedicated BT controller
for hosts that don't have one or whose built-in radio is broken.

## Hardware support

| Build env       | Board                | BT HCI | MSC (µSD) | GUD (LCD) | HID (BOOT btn) |
|-----------------|----------------------|:------:|:---------:|:---------:|:--------------:|
| `t_dongle_s3`   | LilyGo T-Dongle S3   |   ✓    |     ✓     |     ✓     |       ✓        |
| `super_mini`    | ESP32-S3 Super Mini  |   ✓    |     —     |     —     |       —        |

Should work on any ESP32-S3 board with native USB on GPIO19/GPIO20
under the `super_mini` env. The `t_dongle_s3` env hardcodes the
T-Dongle's pin map for the LCD (SPI + LEDC backlight) and the µSD slot
(SDMMC slot 1, 4-bit mode).

## How it works

The firmware runs the ESP32-S3's BLE controller in controller-only mode
and bridges its H4 transport to a USB BT HCI class interface. Mass
Storage is backed by the SDMMC peripheral; the GUD display function is
adapted from [notro/gud-pico](https://github.com/notro/gud-pico) (see
[`THIRDPARTY.md`](THIRDPARTY.md)) with an ST7735 SPI backend.

### USB layout — `t_dongle_s3` build

Five interfaces, two IADs:

| IF | Class                | Endpoints                  | Purpose                 |
|---:|----------------------|----------------------------|-------------------------|
|  0 | `0xE0/0x01/0x01`     | EP1 INT IN, EP2 BULK IN/OUT | BT HCI                  |
|  1 | `0xE0/0x01/0x01`     | none                        | Dummy SCO (btusb quirk) |
|  2 | `0x08/0x06/0x50`     | EP3 BULK IN/OUT             | MSC (SCSI BBB)          |
|  3 | `0xFF/0x00/0x00`     | EP5 BULK OUT                | GUD vendor              |
|  4 | `0x03/0x00/0x00`     | EP4 INT IN                  | HID gamepad (1 button)  |

Five IN endpoints (EP0 + EP1 + EP2 + EP3 + EP4) fills all five TX FIFOs
the ESP32-S3 USB-OTG core provides — no further IN endpoints can be
added on this build without dropping one.

### USB layout — `super_mini` build

Two interfaces, one IAD:

| IF | Class                | Endpoints                  | Purpose                |
|---:|----------------------|----------------------------|------------------------|
|  0 | `0xE0/0x01/0x01`     | EP1 INT IN, EP2 BULK IN/OUT | BT HCI                 |
|  1 | `0xE0/0x01/0x01`     | none                        | Dummy SCO (btusb quirk)|

### USB IDs

Both builds enumerate as `1d50:614d` — the OpenHardware.io / GUD USB
Display allocation. The Linux `gud` driver carries this exact pair in
its static `id_table` and auto-binds to a vendor-class interface, so on
the `t_dongle_s3` build the display works with no `new_id` fiddling.
`btusb` matches by **interface class** (`0xE0/0x01/0x01`) not VID:PID,
so the HCI half works the same regardless. On `super_mini` there's no
vendor interface, so `gud` simply doesn't bind — harmless.

The `iManufacturer` / `iProduct` strings ("Espressif" / "ESP32-S3 BT HCI
+ MSC + GUD + HID" or "ESP32-S3 BT HCI") keep the device identifiable
in `lsusb`.

### Why dummy SCO at IF1

`btusb_probe()` in the Linux kernel unconditionally claims
`bInterfaceNumber + 1` of an HCI device looking for a SCO ISOC
alt-setting and fails the entire probe if it can't get it. We're
BLE-only and have no real SCO to advertise, so we offer a do-nothing
IF1 just to satisfy that quirk; without it `/dev/hciN` doesn't come up.

### Why HCI must be at IF0

`btusb`'s `alloc_ctrl_urb()` hardcodes `dr->wIndex = 0` for HCI command
control transfers. TinyUSB routes class control requests to the driver
that owns the interface number in `wIndex.low`, so HCI commands always
land on whichever driver owns IF0. With anything other than the BTH
class driver at IF0, that driver returns false for `bRequest=0` and
TinyUSB stalls EP0 — the host gives up with `-EPIPE` on opcode `0x0c03`
(HCI Reset) and `/dev/hciN` never comes up.

## Building

Requires PlatformIO.

```sh
# Default — full T-Dongle S3 firmware
pio run

# Explicit env selection
pio run -e t_dongle_s3       # full firmware (BT + MSC + GUD + HID)
pio run -e super_mini        # BT-only

pio run -e t_dongle_s3 -t upload   # flash (see download mode below)
```

The two envs differ only in the `BOARD_T_DONGLE_S3` / `BOARD_SUPER_MINI`
build flag; `src/main.c`, `src/tusb_config.h`, `src/msc_sd.c`,
`src/hid_button.c`, and `src/gud/*.c` `#ifdef`-gate the function-specific
code so the Super Mini binary doesn't pull in the SDMMC driver, the SPI
panel driver, the GUD class driver, or the HID class driver. Net result:
~96 KB smaller binary on Super Mini.

## Flashing

The firmware takes over the USB port for OTG, so the USB Serial/JTAG
interface used for flashing is unavailable during normal operation.

### Option A — software trigger (preferred for the dev loop)

```sh
python3 tools/reboot-download.py && pio run -t upload && \
  esptool --port /dev/ttyACM0 --after watchdog-reset run
```

The script first sends an HCI vendor command (opcode `0xFCFE`, OGF=0x3F
OCF=0xFE) over EP0. The firmware intercepts it, sets
`RTC_CNTL_OPTION1.force_download_boot`, peripheral-resets the OTG block,
and calls `esp_rom_software_reset_system()`. The script polls for the
device to re-enumerate as `303a:0009` (USB-OTG ROM bootloader). If that
doesn't happen within ~8 s — the ESP32-S3 OTG ROM bootloader is
famously unreliable about honoring `force_download_boot` after software
reset (see
[micropython#13402](https://github.com/orgs/micropython/discussions/13402),
[esp-idf#13287](https://github.com/espressif/esp-idf/issues/13287),
[arduino-esp32#6762](https://github.com/espressif/arduino-esp32/issues/6762))
— the script falls back to a true VBUS power cycle via the parent hub's
per-port `disable` file, which both clears chip USB peripheral state
and lets `force_download_boot` (preserved in RTC) take effect on the
cold boot. After flashing, `esptool --after watchdog-reset run`
triggers the chip's RTC watchdog to do a system-level reset that boots
the app cleanly.

Hardware requirement for full automation: the dongle must be plugged
into a USB hub that supports per-port power switching (most laptop root
hubs don't — they just disconnect the data lines without cutting VBUS,
so `force_download_boot` doesn't get the cold boot it needs). External
hubs based on VIA Labs / GL852 / etc. controllers usually do support it.
Without such a hub, the script-side reset still works most of the time;
when it doesn't you'll need to replug manually.

Setup (one-time):

- `pip install pyusb Pillow` for `tools/reboot-download.py` and
  `tools/draw-text.py`.
- Install the udev rule from `udev/`:
  - `80-esp32s3-hci-dev.rules` grants the user libusb + tty access (no
    sudo) and grants write access to per-port `disable` files on any
    USB hub (used for VBUS recovery during the dev flash loop).
  ```sh
  sudo cp udev/*.rules /etc/udev/rules.d/
  sudo udevadm control --reload
  # then replug the device once
  ```

### Option B — physical BOOT button (always works)

1. **Hold the BOOT button** on the board.
2. **Unplug and replug** the USB cable (while holding BOOT).
3. **Release BOOT** — the chip is now in ROM download mode.
4. Run `pio run -t upload`.

## Usage on Linux

After plugging in the dongle, the kernel auto-binds drivers per the
USB layout above:

```sh
# Verify the device enumerated
lsusb | grep 1d50:614d
# 1d50:614d ... ESP32-S3 BT HCI + MSC + GUD + HID   (t_dongle_s3 build)
# 1d50:614d ... ESP32-S3 BT HCI                     (super_mini build)
```

### Bluetooth

```sh
bluetoothctl list                          # shows the new controller
sudo btmgmt --index hciN power on
bluetoothctl
```

The `hciN` index depends on what other controllers are present;
`bluetoothctl list` shows them all.

### Mass Storage (T-Dongle only)

```sh
lsblk                                      # /dev/sdX appears
udisksctl status                           # GNOME-style mount info
sudo parted /dev/sdX print                 # partition table
```

GNOME / KDE auto-mount is the easiest way to interact with the card.
`dd`-ing UEFI shell or distro install media onto it makes the dongle a
bootable USB drive — modern UEFI matches MSC by class triple, not by
interface number, so booting works fine even though MSC is at IF2.

### Display (T-Dongle only)

```sh
# Render text via libusb directly (bypasses the kernel gud driver)
tools/draw-text.py -t "Hello" -s 32 --fg yellow

# Or via DRM (requires the gud kernel driver bound to IF3)
modetest -M gud
modetest -M gud -s <conn>@<crtc>:80x160     # from a TTY
```

The DRM card is marked **non-desktop** via a Microsoft VSDB in our EDID,
so GNOME / mutter ignore the panel. As a side effect there is **no
`/dev/fbN`** for our card: `drm_fb_helper` filters non-desktop
connectors out of fbdev-emulation, so plug-in dmesg always shows `gud
…: [drm] Cannot find any crtc or sizes` (twice — that's expected, not
a bug). Use `/dev/dri/cardN` directly (modetest, libdrm) or
`tools/draw-text.py` (libusb, bypasses the kernel driver entirely).

### BOOT button (T-Dongle only)

The dongle's BOOT button on GPIO0 is exposed as a one-button gamepad.
Linux maps Generic Desktop / Game Pad / Button-1 usage to `BTN_A`
(`BTN_GAMEPAD` / `BTN_SOUTH`):

```sh
# Watch presses (requires read access to /dev/input/eventN — typically
# in the `input` group, or run as root)
sudo evtest /dev/input/eventN          # press BOOT — see EV_KEY BTN_A 1/0

# udev-readable joydev wrapper — works without root if your distro
# ships /dev/input/jsN as world-readable
jstest /dev/input/jsN
```

The button is debounced in firmware (20 ms hysteresis); reports are
sent on transitions only, so there's no idle USB chatter. GPIO0 is also
the chip's strapping pin — held low at reset triggers ROM download
mode — so leaving it pressed during boot will drop you into the ROM
bootloader instead of running firmware. Same as plain ESP32-S3 boards.

## Configuration

Key settings in `sdkconfig.defaults`:

| Setting | Purpose |
|---|---|
| `CONFIG_BT_CONTROLLER_ONLY=y` | BLE controller without host stack |
| `CONFIG_BT_CTRL_HCI_MODE_UART_H4=y` | H4 transport for HCI |
| `CONFIG_USJ_ENABLE_USB_SERIAL_JTAG=n` | Free USB PHY for OTG use |
| `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y` | No console on USB Serial/JTAG |
| `CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y` | Match actual flash size |

## Project structure

```
src/
  main.c             - BT HCI <-> USB bridge, descriptors, class driver registration
  tusb_config.h      - TinyUSB configuration
  idf_component.yml  - Dependency on espressif/tinyusb
  msc_sd.{c,h}       - SDMMC slot 1 bridge to TinyUSB MSC class       [T-Dongle only]
  gud/               - Generic USB Display function and ST7735 panel  [T-Dongle only]
  hid_button.{c,h}   - HID gamepad bridge for the BOOT button (GPIO0) [T-Dongle only]
sdkconfig.defaults   - ESP-IDF Kconfig overrides
platformio.ini       - PlatformIO project config (two envs: t_dongle_s3, super_mini)
docs/                - design notes / known-bug writeups
tools/               - host-side dev scripts (reboot-download, draw-text)
udev/                - udev rule for libusb + hub-port-disable access
licenses/            - upstream licenses for vendored code
THIRDPARTY.md        - attribution table
```

## Licensing

Original code in this repository is released under the
[MIT License](LICENSE). A few files in `src/gud/` are vendored from
upstream MIT-licensed projects; their original copyright notices are
preserved in the file headers and the upstream LICENSE texts are
reproduced verbatim under `licenses/`. See [`THIRDPARTY.md`](THIRDPARTY.md)
for the full attribution table.
