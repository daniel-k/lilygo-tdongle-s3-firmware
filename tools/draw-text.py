#!/usr/bin/env python3
"""
End-to-end demo: render arbitrary text onto the LilyGo T-Dongle S3
via the firmware's GUD interface, using libusb directly (no kernel gud
driver, no DRM, no fbdev flush gymnastics).

Usage:
    tools/draw-text.py [-t TEXT] [-s SIZE] [--bg BG] [--fg FG]

Defaults render a couple of lines of ~14-pixel text in white-on-black.
The panel native orientation is portrait 80x160. Pass --landscape to
rotate the canvas 90 degrees if you'd rather hold the dongle sideways.

Requires:
    pip install pyusb Pillow
    udev/80-esp32s3-hci-dev.rules installed (libusb access without sudo)
    The kernel `gud` driver NOT bound to interface 3, since libusb has
    to claim it. If `gud` is bound, this script will tell you how to
    unbind.
"""

import argparse
import struct
import sys
import time

import usb.core
import usb.util
from PIL import Image, ImageDraw, ImageFont

VID, PID = 0x1D50, 0x614D    # Openhardware.io / GUD USB Display
GUD_IFACE = 3
WIDTH, HEIGHT = 80, 160
FB_BYTES = WIDTH * HEIGHT * 2  # RGB565

# GUD requests
GUD_REQ_GET_STATUS = 0x00
GUD_REQ_SET_BUFFER = 0x60
GUD_REQ_SET_STATE_CHECK = 0x61
GUD_REQ_SET_STATE_COMMIT = 0x62
GUD_REQ_SET_CONTROLLER_ENABLE = 0x63
GUD_REQ_SET_DISPLAY_ENABLE = 0x64
GUD_PIXEL_FORMAT_RGB565 = 0x40


def parse_color(s):
    """Accept '#rrggbb', 'r,g,b', or a name PIL understands."""
    return Image.new("RGB", (1, 1), s).getpixel((0, 0))


def rgb_to_rgb565_le(img):
    """PIL RGB image -> packed bytes in host-LE RGB565. The firmware byte-swaps
    on its way out to the ST7735 (which expects MSB-first), so we just send
    the host-native form here."""
    arr = bytearray(FB_BYTES)
    px = img.load()
    i = 0
    for y in range(img.height):
        for x in range(img.width):
            r, g, b = px[x, y][:3]
            v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            arr[i] = v & 0xFF
            arr[i + 1] = (v >> 8) & 0xFF
            i += 2
    return bytes(arr)


def find_font(size):
    # Try a few common locations for a TrueType font; fall back to PIL's
    # built-in bitmap font (which ignores `size` and is ~6x11 px).
    candidates = [
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/TTF/LiberationSans-Regular.ttf",
    ]
    for path in candidates:
        try:
            return ImageFont.truetype(path, size)
        except OSError:
            continue
    print("[!] no TrueType font found, falling back to PIL default (6x11)",
          file=sys.stderr)
    return ImageFont.load_default()


def render_text(text, size, fg, bg, landscape):
    """Draw `text` centered on an 80x160 (or 160x80 if landscape) canvas."""
    if landscape:
        canvas_w, canvas_h = HEIGHT, WIDTH
    else:
        canvas_w, canvas_h = WIDTH, HEIGHT

    img = Image.new("RGB", (canvas_w, canvas_h), bg)
    draw = ImageDraw.Draw(img)
    font = find_font(size)

    # Multi-line, anchor centre.
    bbox = draw.multiline_textbbox((0, 0), text, font=font, align="center")
    tw = bbox[2] - bbox[0]
    th = bbox[3] - bbox[1]
    cx = (canvas_w - tw) // 2 - bbox[0]
    cy = (canvas_h - th) // 2 - bbox[1]
    draw.multiline_text((cx, cy), text, fill=fg, font=font, align="center")

    if landscape:
        # Rotate so the rendered canvas matches the panel's native portrait.
        img = img.rotate(-90, expand=True)
    return img


def gud_setup(dev):
    """Send the GUD setup sequence: controller on, RGB565 mode, display on."""
    def vset(req, data):
        return dev.ctrl_transfer(0x41, req, 0, GUD_IFACE, data, timeout=2000)
    def vget(req, length):
        return bytes(dev.ctrl_transfer(0xC1, req, 0, GUD_IFACE, length, timeout=2000))

    vset(GUD_REQ_SET_CONTROLLER_ENABLE, b"\x01")
    mode = struct.pack(
        "<IHHHHHHHHI",
        768,                              # clock kHz (matches GET_MODES)
        WIDTH, WIDTH, WIDTH, WIDTH,       # h: display, sync_start, sync_end, total
        HEIGHT, HEIGHT, HEIGHT, HEIGHT,   # v: same
        0x400,                            # flags = PREFERRED
    )
    vset(GUD_REQ_SET_STATE_CHECK, mode + bytes([GUD_PIXEL_FORMAT_RGB565, 0]))
    vset(GUD_REQ_SET_STATE_COMMIT, b"")
    vset(GUD_REQ_SET_DISPLAY_ENABLE, b"\x01")
    status = vget(GUD_REQ_GET_STATUS, 1)
    if status != b"\x00":
        raise RuntimeError(f"GUD setup status nonzero: {status.hex()}")


def gud_flush(dev, ep_out, fb):
    """Send a full-frame SET_BUFFER + bulk OUT for the panel."""
    if len(fb) != FB_BYTES:
        raise ValueError(f"framebuffer must be {FB_BYTES} bytes, got {len(fb)}")
    req = struct.pack("<IIIIIBI", 0, 0, WIDTH, HEIGHT, FB_BYTES, 0, 0)
    dev.ctrl_transfer(0x41, GUD_REQ_SET_BUFFER, 0, GUD_IFACE, req, timeout=2000)
    n = ep_out.write(fb, timeout=5000)
    if n != FB_BYTES:
        raise RuntimeError(f"bulk OUT short: wrote {n}/{FB_BYTES}")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("-t", "--text", default="Hello\nT-Dongle!",
                   help="text to display (\\n-separated for multi-line)")
    p.add_argument("-s", "--size", type=int, default=18,
                   help="font size in pixels (default 18)")
    p.add_argument("--bg", default="black", help="background colour")
    p.add_argument("--fg", default="white", help="foreground colour")
    p.add_argument("--landscape", action="store_true",
                   help="rotate canvas 90deg so 160x80 reads horizontally")
    p.add_argument("--save", metavar="PATH",
                   help="write rendered PNG here too (debug)")
    args = p.parse_args()

    text = args.text.replace("\\n", "\n")
    img = render_text(text, args.size, parse_color(args.fg),
                      parse_color(args.bg), args.landscape)
    if args.save:
        img.save(args.save)
        print(f"saved preview to {args.save}")

    fb = rgb_to_rgb565_le(img)

    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        sys.exit(f"USB device {VID:04x}:{PID:04x} not found — is it plugged in?")

    cfg = dev.get_active_configuration()
    intf = cfg[(GUD_IFACE, 0)]
    try:
        usb.util.claim_interface(dev, intf)
    except usb.core.USBError as e:
        if "busy" in str(e).lower():
            sys.exit("interface 3 is busy — kernel `gud` driver probably has it.\n"
                     "  echo '3-1.1:1.3' | sudo tee /sys/bus/usb/drivers/gud/unbind")
        raise

    try:
        ep_out = usb.util.find_descriptor(intf, custom_match=lambda e:
            usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT)
        gud_setup(dev)
        t0 = time.time()
        gud_flush(dev, ep_out, fb)
        ms = (time.time() - t0) * 1000
        print(f"flushed {FB_BYTES} bytes in {ms:.1f} ms — text shown on panel")
    finally:
        usb.util.release_interface(dev, intf)


if __name__ == "__main__":
    main()
