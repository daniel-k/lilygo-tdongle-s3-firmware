# Follow-ups

Open items left after the GUD bring-up. Status of the in-tree work:

- Super Mini bring-up (descriptor, GUD wire protocol, bulk OUT, kernel
  bind): **done** — commits `d74bf6f`, `c19c8b2`.
- T-Dongle S3 panel (ST7735 SPI driver, byte-swap, write_buffer_cb):
  **done** — commits `518f3dc`, `e5ad81b`.
- USB layout cleanup: dummy SCO IF1, switch to 1d50:614d, EDID
  non-desktop extension. **done** — commits `ed6180a`, `e5627b7`.
  Plug-and-play now works end-to-end with no host-side configuration
  beyond installing one udev rule for the dev flash workflow.

End-to-end verified via `tools/draw-text.py`: host-LE RGB565 bytes
flushed to fb push correctly-coloured frames to the ST7735 panel on
the T-Dongle at ~30 ms / 25600 bytes (~33 fps).

Each section is self-contained so a future session can pick up cold.

---

## 1. Root-cause the TinyUSB DWC2 large-OUT stall, drop the chunking workaround

**Where:** `src/gud/gud_usb.c`, `#define GUD_EDPT_XFER_MAX_SIZE 1024`

**Symptom.** A single `usbd_edpt_xfer(EP5_OUT, buffer, N)` for `N > ~1600`
bytes stalls partway through. The host posts a bulk OUT URB, the device
receives some packets (observed 1024–4608 bytes in repeat tests), then
NAKs/ignores the rest until the host URB times out (default 2 s for `gud`
kernel driver, 3 s in our pyusb test).

**Workaround in place.** Cap the per-call chunk size to 1024 bytes. The
existing chunk-continuation in `gud_driver_xfer_cb` (inherited from
gud-pico's driver) re-arms the next chunk transparently. End-to-end
throughput is ~27 ms per 25600-byte (80×160 RGB565) frame, ~36 fps,
~7.5 Mbit/s — plenty for the eventual ST7735 panel.

**Not yet ruled out.** Likely candidates:
- TinyUSB DWC2 slave-mode RX FIFO sizing. ESP32-S3's USB-OTG has 1024
  words of FIFO RAM; current sizing in `dcd_dwc2.c` is conservative
  (~56 words for GRXFSIZ with our endpoint set). Bumping GRXFSIZ may
  help.
- IRQ priority of the USB peripheral on ESP32-S3 — RX FIFO drainage
  in slave mode happens in the IRQ handler; if a higher-priority IRQ
  preempts it for too long, packets pile up and overflow.
- Interaction between EP2 OUT (BT ACL, primed 1024 B), EP4 OUT (CDC),
  and EP5 OUT (GUD) all primed simultaneously — could be a per-EP NAK
  state bug.
- Specific to managed component `espressif/tinyusb` 0.19.0~2 — worth
  checking the upstream TinyUSB issue tracker and trying a newer
  version.

**Reproduction.** Build the `t_dongle_s3` env. With firmware running and
`gud` kernel driver NOT bound to IF3 (or use
`echo '3-1.1:1.3' | sudo tee /sys/bus/usb/drivers/gud/unbind`):

```python
# venv has pyusb installed
import usb.core, usb.util, struct, time
dev = usb.core.find(idVendor=0x1D50, idProduct=0x614D)
intf = dev.get_active_configuration()[(3, 0)]
usb.util.claim_interface(dev, intf)
ep = usb.util.find_descriptor(intf, custom_match=lambda e:
    usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT)

def vset(req, idx, data): return dev.ctrl_transfer(0x41, req, idx, 3, data, timeout=2000)
vset(0x63, 0, b"\x01")  # SET_CONTROLLER_ENABLE
mode = struct.pack("<IHHHHHHHHI", 768, 80, 80, 80, 80, 160, 160, 160, 160, 0x400)
vset(0x61, 0, mode + bytes([0x40, 0]))  # SET_STATE_CHECK
vset(0x62, 0, b"")                      # SET_STATE_COMMIT
vset(0x64, 0, b"\x01")                  # SET_DISPLAY_ENABLE

# Try a 25600-byte frame. With GUD_EDPT_XFER_MAX_SIZE bumped back to
# 65472, this returns partial (e.g. 1024 / 4608 bytes) after a 3 s timeout.
vset(0x60, 0, struct.pack("<IIIIIBI", 0, 0, 80, 160, 25600, 0, 0))
ep.write(b"\xa5" * 25600, timeout=5000)
```

To experiment, temporarily set `GUD_EDPT_XFER_MAX_SIZE` back to
`(0xffff - (0xffff % GUD_BULK_OUT_SIZE))` and rebuild.

**Definition of done.** Either a TinyUSB component bump fixes it, or a
fix lands upstream (or in a fork) that allows >25600-byte single OUT
receives without stalling. Then revert `GUD_EDPT_XFER_MAX_SIZE` to the
upstream value.

---

## 2. Report a Linux gud driver kernel oops on framebuffer remove

When `modetest -M gud -s <conn>@<crtc>:80x160` exits cleanly, the kernel
oopses with a NULL pointer dereference in `gud_plane_atomic_update+0x148`
on Linux `6.19.14-arch1-1`. Stack trace:

```
BUG: kernel NULL pointer dereference, address: 00000000000005d0
RIP: gud_plane_atomic_update+0x148/0x460 [gud]
Call Trace:
  drm_atomic_helper_commit_planes
  drm_atomic_helper_commit_tail
  commit_tail
  drm_atomic_helper_commit
  drm_atomic_commit
  drm_framebuffer_remove
  drm_mode_rmfb_work_fn
  process_one_work
  worker_thread
```

The faulting instruction `mov 0x5d0(%r14), %rax` with `r14=0` indicates
the plane's atomic update is being entered with a NULL plane state /
new state during the framebuffer-removal path. Reproducer: bind gud,
run modetest with `-s` to draw a frame, press Ctrl-C / exit. Kernel
worker faults; the system survives but the workqueue thread is gone.

**Action.** Report to dri-devel@ with kernel version, repro recipe, and
a pointer to this firmware so they have a target device. Not a firmware
bug — included here so a future session doesn't waste time chasing it
through our code.

---

## 3. (lowest priority) Optional LZ4 compression

The `notro/gud-pico` upstream supports LZ4 compression on the bulk OUT
path. We deliberately did not pull in `lz4.c` / `lz4.h` (~115 KB of
source) for the v1 port — the GUD descriptor advertises
`compression = 0` so the host won't negotiate it.

If 36 fps full-frame RGB565 ever turns out to be insufficient (e.g.
for 320×240 panels or higher refresh), revisit by:
1. Lifting `lz4.c` / `lz4.h` from gud-pico into `src/gud/`.
2. Allocating a `_compress_buf` of `gud_get_buffer_length(...)` size
   in PSRAM (already noted as the long-term framebuffer location).
3. Setting `compression = GUD_COMPRESSION_LZ4` in `gud_display_cfg`,
   passing the compress_buf to `gud_driver_setup`, and re-enabling the
   `LZ4_decompress_safe` call in `gud_driver_xfer_cb` (currently
   gated with a "not built in" warning).
