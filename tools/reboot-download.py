#!/usr/bin/env python3
"""Reboot the ESP32-S3 BT HCI dongle into ROM download mode.

Sends a vendor-specific HCI command (opcode 0xFCFE, OGF=0x3F OCF=0xFE) over
EP0; the firmware intercepts it, sets RTC_CNTL_OPTION1.force_download_boot,
resets the OTG/USJ peripherals, and calls esp_rom_software_reset_system().

The OTG ROM bootloader on the ESP32-S3 is famously unreliable about honoring
force_download_boot after a software reset (see micropython#13402,
esp-idf#13287, arduino-esp32#6762). When the chip-side reset doesn't make
the ROM enter download mode, the script falls back to a true VBUS power
cycle via the parent hub's port-disable sysfs file — which both clears
chip state and lets force_download_boot take effect on the cold boot.

For the VBUS recovery to work the dongle must be plugged into a hub that
supports per-port power control (most laptop root hubs don't); the udev
rule in udev/80-esp32s3-hci-dev.rules grants the user write access to all
hub port-disable files automatically.

Requires `pyusb` and libusb-1.0.
"""

from __future__ import annotations

import argparse
import os
import sys
import time

try:
    import usb.core
except ImportError:
    sys.exit("pyusb is required: pip install pyusb")

# Running firmware enumerates as Openhardware.io 1d50:614d (the GUD USB Display
# allocation; adopted so the kernel gud driver auto-binds without new_id).
# Both ROM bootloader paths still report Espressif's 0x303A vendor.
DEFAULT_VID = 0x1D50
DEFAULT_PID = 0x614D
ROM_VID     = 0x303A
ROM_OTG_PID = 0x0009          # OTG ROM bootloader (script-triggered path)
ROM_USJ_PID = 0x1001          # USJ ROM bootloader (BOOT-button path)
HCI_OPCODE_REBOOT_DOWNLOAD = 0xFCFE

# USB Bluetooth HCI command transport (Bluetooth Core v5.x, Vol 4 Part B §2.2.1):
#   bmRequestType = 0x20  (class | host->device | recipient=device)
#   bRequest      = 0x00
#   wValue, wIndex = 0
#   data          = HCI command (opcode_lo, opcode_hi, plen, params...)
HCI_CMD_BMREQUESTTYPE = 0x20
HCI_CMD_BREQUEST = 0x00

REBOOT_WAIT_TIMEOUT_S = 8.0  # chip-side reset is fast when it works at all
SEND_RETRIES = 2


def find_rom_bootloader():
    for pid in (ROM_OTG_PID, ROM_USJ_PID):
        d = usb.core.find(idVendor=ROM_VID, idProduct=pid)
        if d is not None:
            return d, pid
    return None, None


def wait_for_rom_bootloader(timeout_s: float):
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        d, pid = find_rom_bootloader()
        if d is not None:
            return pid
        time.sleep(0.1)
    return None


def find_port_disable_file(vid: int, pid: int):
    """Find the sysfs `disable` file for the parent hub port hosting the
    device. Returns a path or None.

    USB sysfs hierarchy:
        /sys/bus/usb/devices/3-1.1/                 ← the dongle
        /sys/bus/usb/devices/3-1/                   ← parent hub (3-1)
        /sys/bus/usb/devices/3-1/3-1:1.0/3-1-port1/disable   ← target

    For a device at busno-N1.N2.N3..., parent hub bus path is busno-N1.N2...
    and the disable file lives under <parent>/<parent>:<intf>/<parent>-port<N3>/disable.
    """
    base = "/sys/bus/usb/devices"
    for entry in os.listdir(base):
        try:
            with open(os.path.join(base, entry, "idVendor")) as f:
                v = int(f.read().strip(), 16)
            with open(os.path.join(base, entry, "idProduct")) as f:
                p = int(f.read().strip(), 16)
        except (FileNotFoundError, ValueError):
            continue
        if v != vid or p != pid:
            continue
        # entry is e.g. "3-1.1" or "1-3.4.2". Last dotted segment = port on
        # parent hub. Drop it (and the dot) to get parent path.
        if "." not in entry or "-" not in entry:
            return None
        parent, _, last_port = entry.rpartition(".")
        # parent could be e.g. "3-1" — that's the parent hub.
        # Try common interface number 1.0; fall back to scanning.
        for intf in ("1.0", "0.0"):
            cand = os.path.join(base, parent, f"{parent}:{intf}",
                                f"{parent}-port{last_port}", "disable")
            if os.path.exists(cand):
                return cand
        # Fallback: scan for any matching port file.
        parent_dir = os.path.join(base, parent)
        for intf_dir in os.listdir(parent_dir):
            cand = os.path.join(parent_dir, intf_dir,
                                f"{parent}-port{last_port}", "disable")
            if os.path.exists(cand):
                return cand
    return None


def vbus_power_cycle(disable_file: str, off_s: float = 1.0) -> bool:
    """Cut VBUS to the port for off_s, then restore. Returns True if write
    succeeded. The host re-enumerates whatever boots from cold."""
    try:
        with open(disable_file, "w") as f:
            f.write("1")
        time.sleep(off_s)
        with open(disable_file, "w") as f:
            f.write("0")
        return True
    except (PermissionError, OSError) as exc:
        print(f"VBUS cycle failed ({disable_file}): {exc}", file=sys.stderr)
        return False


def send_reboot_opcode(dev) -> None:
    payload = bytes([
        HCI_OPCODE_REBOOT_DOWNLOAD & 0xFF,
        (HCI_OPCODE_REBOOT_DOWNLOAD >> 8) & 0xFF,
        0x00,  # parameter length
    ])
    # EP0 control transfers don't require detaching kernel drivers — EP0 is
    # shared and Linux's usbdevfs forwards SETUP straight to the device. The
    # firmware resets mid-transfer, so libusb will typically raise ENODEV/EIO;
    # that's success, not failure.
    try:
        dev.ctrl_transfer(HCI_CMD_BMREQUESTTYPE, HCI_CMD_BREQUEST,
                          0, 0, payload, timeout=500)
    except usb.core.USBError:
        pass  # expected when the device resets before the status stage


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Reboot the ESP32-S3 BT HCI dongle into ROM download mode.")
    parser.add_argument("--vid", type=lambda x: int(x, 0), default=DEFAULT_VID,
                        help=f"USB vendor ID (default 0x{DEFAULT_VID:04x})")
    parser.add_argument("--pid", type=lambda x: int(x, 0), default=DEFAULT_PID,
                        help=f"running-firmware PID (default 0x{DEFAULT_PID:04x})")
    args = parser.parse_args()

    # Already in ROM mode? Nothing to do.
    _, rom_pid = find_rom_bootloader()
    if rom_pid is not None:
        print(f"Already in ROM bootloader ({ROM_VID:04x}:{rom_pid:04x}).")
        return 0

    disable_file = find_port_disable_file(args.vid, args.pid)
    if disable_file is None:
        print("Note: parent hub port-disable file not found; VBUS recovery "
              "won't be available. Plug into a hub with per-port power.",
              file=sys.stderr)

    for attempt in range(1, SEND_RETRIES + 1):
        # The chip may have already entered download mode (slow re-enum, or
        # force_download_boot survived a previous cycle).
        rom_pid = wait_for_rom_bootloader(0.5)
        if rom_pid is not None:
            print(f"Device entered ROM bootloader ({ROM_VID:04x}:{rom_pid:04x}).")
            return 0

        dev = usb.core.find(idVendor=args.vid, idProduct=args.pid)
        if dev is not None:
            send_reboot_opcode(dev)
            del dev
            rom_pid = wait_for_rom_bootloader(REBOOT_WAIT_TIMEOUT_S)
            if rom_pid is not None:
                print(f"Device entered ROM bootloader "
                      f"({ROM_VID:04x}:{rom_pid:04x}).")
                return 0

        # Either chip is GONE, or firmware-side reset didn't enter ROM.
        # Fall back to a true VBUS power cycle.
        if disable_file is not None and attempt < SEND_RETRIES + 1:
            print(f"Attempt {attempt}/{SEND_RETRIES}: firmware-side reset "
                  f"didn't enter ROM, doing VBUS power cycle...",
                  file=sys.stderr)
            if vbus_power_cycle(disable_file):
                rom_pid = wait_for_rom_bootloader(REBOOT_WAIT_TIMEOUT_S)
                if rom_pid is not None:
                    print(f"Device entered ROM bootloader after VBUS cycle "
                          f"({ROM_VID:04x}:{rom_pid:04x}).")
                    return 0

    print(f"Device did not enter ROM bootloader after {SEND_RETRIES} "
          f"attempts. Use the BOOT button to enter download mode manually.",
          file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
