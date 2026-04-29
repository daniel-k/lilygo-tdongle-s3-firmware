# USB Mass Storage Class on the µSD slot

Replaces the CDC-ACM debug log (silent on master since some component
bump anyway) with a USB Mass Storage class driver backed by the
T-Dongle S3's onboard µSD slot. The dongle now enumerates as a regular
USB removable drive — Linux's `usb-storage` auto-binds, the card shows
up as `/dev/sdX`, and any host can mount / format / `dd` it like any
other USB stick.

This is purely a "another standard USB function alongside the existing
BLE HCI and GUD display" change — the firmware doesn't care what's on
the card, doesn't impose any partition layout, and doesn't read or
write the card itself.

## Endpoint budget

ESP32-S3 USB-OTG has 5 TX FIFOs total (EP0 IN + four IN endpoints).
After this change the layout is:

| EP | direction | function |
|---|---|---|
| EP1 | IN | HCI events (interrupt) |
| EP2 | IN/OUT | HCI ACL (bulk) |
| EP3 | IN/OUT | MSC bulk transfer (µSD) |
| EP4 | — | (free; was CDC) |
| EP5 | OUT | GUD framebuffer (bulk) |

Net IN endpoints used: 3 (EP1 + EP2 + EP3) plus EP0. Comfortably under
the 5 TX FIFO ceiling — leaves room for one more IN-direction add-on
later.

## Interface layout

Goes from 5 interfaces to 4, **HCI at IF0**:

| IF | Class | Description |
|---|---|---|
| 0 | 0xE0 / 0x01 / 0x01 | BT HCI |
| 1 | 0xE0 / 0x01 / 0x01 | dummy SCO (btusb N+1 quirk) |
| 2 | 0x08 (Mass Storage) | MSC, SCSI transparent, BBB |
| 3 | 0xFF (Vendor) | GUD display |

`bNumInterfaces` becomes 4 (was 5).

**Why HCI at IF0 specifically.** btusb's `alloc_ctrl_urb()` hardcodes
`dr->wIndex = 0` for HCI command control transfers
(`drivers/bluetooth/btusb.c`). TinyUSB routes class control requests
to the driver that owns the interface number in `wIndex.low`, so HCI
commands always land on whichever driver owns IF0. With anything other
than the BTH driver at IF0, that driver returns false for `bRequest=0`
and TinyUSB stalls EP0 — the host gives up with `-EPIPE` on opcode
`0x0c03` (HCI Reset) and `/dev/hciN` never comes up. We learned this
the hard way after first laying out the descriptor with MSC at IF0 for
maximum BIOS-boot compat (see "Why not MSC at IF0" below).

**Why dummy SCO at IF1.** btusb_probe() unconditionally claims
`bInterfaceNumber + 1` of an HCI device looking for a SCO ISOC
alt-setting and refuses to bind to N if that fails — so wherever HCI
lands, an empty companion has to be at the next interface. With both
inside one IAD (`bInterfaceCount = 2`) the host treats them as one
logical function.

**Why MSC at IF2 is fine.** BIOS / UEFI USB stacks all match Mass
Storage by class triple (`0x08 / 0x06 / 0x50`), not by interface
number — so a composite device with MSC at any position boots fine on
modern UEFI. Quirky / minimal / legacy boot ROMs that look only at IF0
were the original concern, but in practice the btusb constraint above
is non-negotiable and we haven't seen a real bootloader miss MSC at
IF2.

**Why GUD at IF3.** No constraint either way — it's a bare vendor
function that the kernel `gud` driver matches by VID:PID + class 0xFF.
Putting it last keeps the descriptor reading "BT (with companion) →
storage → display" with each function block self-contained.

IAD layout:

- **IAD on IF0+IF1 BT** (`bFirstInterface = 0, bInterfaceCount = 2`).
  Required for the host to know IF0 and IF1 are one logical function.
- **No IAD on IF2 MSC.** Bare single-interface function, recognised by
  every host's MSC driver; spec is fine with this even when the device
  declares `bDeviceClass = 0xEF` because nothing in IF2 needs grouping.
- **IAD on IF3 GUD.** Single interface, but the IAD makes the function
  boundary explicit and matches the pattern the BT half uses.

## Hardware: T-Dongle S3 µSD pins

From the board silkscreen / LilyGo's pin map:

| Signal | GPIO |
|---|---|
| SDMMC_CLK | 12 |
| SDMMC_CMD | 16 |
| SDMMC_D0 | 14 |
| SDMMC_D1 | 17 |
| SDMMC_D2 | 21 |
| SDMMC_D3 | 18 |

These do **not** overlap with anything we already use:

- LCD (SPI2): GPIO 1, 2, 3, 4, 5.
- LCD backlight (LEDC): GPIO 38.
- USB: GPIO 19, 20.
- Onboard APA102 RGB LED: GPIO 39, 40 (unused by us).
- Boot button: GPIO 0.

So we use the **SDMMC peripheral in 4-bit mode** — the fast path
LilyGo wired the board for. ~25 MB/s theoretical SD throughput, vs.
~5 MB/s for SPI mode. Doesn't matter for our use case (USB FS caps
us at 1 MB/s either way), but it's the simpler integration path with
ESP-IDF: the standard `sdmmc_host` driver, no SPI bus to allocate
(SPI2 stays the LCD's, SPI3 stays free). Slot 1 on the S3 supports
4-bit, with all six pins routable through the GPIO matrix to the
specific GPIOs above.

## Implementation

### `tusb_config.h`

```c
#define CFG_TUD_CDC             0   // was 1
#define CFG_TUD_MSC             1
#define CFG_TUD_MSC_EP_BUFSIZE  512  // one SD sector
```

### `src/msc_sd.{c,h}`

- `msc_sd_init()` — initialises the SDMMC peripheral via ESP-IDF's
  `sdmmc_host_init` + `sdmmc_host_init_slot`. Slot 1, 4-bit width,
  GPIO matrix routes (CLK=12, CMD=16, D0=14, D1=17, D2=21, D3=18).
  Probes the card with `sdmmc_card_init`, caches the resulting
  `sdmmc_card_t` for the read/write callbacks. Non-fatal on no-card —
  the firmware still comes up with HCI + GUD working, MSC reports "no
  medium".
- TinyUSB MSC callback implementations:
  - `tud_msc_inquiry_cb` — vendor/product/revision strings.
  - `tud_msc_test_unit_ready_cb` — true once the card is mounted.
  - `tud_msc_capacity_cb` — `card->csd.capacity` blocks ×
    `card->csd.sector_size`.
  - `tud_msc_read10_cb` / `tud_msc_write10_cb` — translate the LBA +
    count to `sdmmc_read_sectors` / `sdmmc_write_sectors` calls.
  - `tud_msc_start_stop_cb` — accept eject/load (mostly no-op).
  - `tud_msc_scsi_cb` — reject anything we don't explicitly handle.

### `src/main.c`

- Remove CDC strings (`STRID_CDC` and "ESP32-S3 BT HCI Debug").
- Remove the CDC IAD + `TUD_CDC_DESCRIPTOR` from `desc_configuration`.
- `desc_configuration` body is now: BT IAD → IF0 HCI (3 EPs) → IF1
  dummy SCO (0 EPs) → IF2 MSC (2 EPs) → GUD IAD → IF3 GUD (1 EP).
- `bNumInterfaces` 5 → 4.
- `EP_MSC_IN`/`EP_MSC_OUT` claim 0x83/0x03; `EP_CDC_*` defines deleted.
- `btd_open` keeps drv_len = 39 (IF + 3 EPs + dummy IF) — class match
  is on `bInterfaceClass == 0xE0`, not on interface number.
- Remove `log_vprintf_hook` and the `esp_log_set_vprintf` call.
- Call `msc_sd_init()` once before `tud_rhport_init()` (matches the
  existing pattern: panel init → framebuffer alloc → USB).

### `tools/draw-text.py`

`GUD_IFACE = 3` (was 4 in the pre-MSC layout).

## Verifying via `lsusb -v`

Expected:
- `bNumInterfaces = 4`
- IF0 class 0xE0/0x01/0x01 — `btusb` auto-binds, claims IF1 too.
- IF1 class 0xE0/0x01/0x01, zero endpoints — held by `btusb` as the
  dummy SCO companion.
- IF2 class 0x08 / subclass 0x06 / proto 0x50 — Linux's `usb-storage`
  auto-binds.
- IF3 class 0xFF — `gud` auto-binds (matches `1d50:614d` in its
  static id_table).
- `bluetoothctl list` shows `/dev/hciN` powered.
- `udisksctl status` shows the card.
- `parted /dev/sdX print` works.

## Verifying boot

Format the µSD GPT + ESP, drop a known-good UEFI shell image. Plug
dongle into a real PC, enter BIOS, select the dongle in boot menu,
should land in EFI shell prompt. Confirms the host treats it as a
proper bootable removable drive.

## Testing milestones

1. **MSC enumerates as removable drive.** `lsblk` shows `/dev/sdX`,
   right capacity. Read from a freshly-formatted FAT32 partition.
2. **Sustained read**: `dd if=/dev/sdX bs=1M count=200 of=/dev/null`
   should hit ~1 MB/s steady (USB FS limit).
3. **Sustained write**: same direction reversed. Probably ~0.7–0.9 MB/s
   given SD-over-SPI write overhead.
4. **Boot from dongle**: format µSD with a UEFI shell or GRUB
   standalone, boot a target machine from it.
5. **Coexistence**: while a host is reading from MSC, run
   `tools/draw-text.py` from a different host to confirm BLE + GUD
   still work simultaneously.

## Risks / edge cases

- **Hot insert/remove of the µSD card**: the T-Dongle has no card-detect
  switch wired. If the card is missing at boot, `msc_sd_init` is non-
  fatal — the firmware comes up with HCI + GUD working, MSC reports
  "no medium" until the next boot.
- **SD speed class variation**: cheap cards may write at <500 KB/s.
  Doesn't affect the boot/read path which is the main use case.
- **Power**: SD card current draw 50–100 mA during reads on top of
  the BT controller / LCD. USB 2.0 budget is 500 mA, we're under it
  even at peak.
- **Concurrent USB activity**: MSC and GUD both use bulk endpoints.
  No FIFO contention since they're separate EPs, but TinyUSB's task
  servicing is single-threaded — heavy MSC I/O might delay GUD bulk
  packets. The chunking workaround we already have for GUD (1024-byte
  chunks) should still be sufficient; might re-validate
  `draw-text.py` performance during a sustained MSC read.

## Recovering from a bad descriptor

`tools/reboot-download.py` sends `bmRequestType=0x20, bRequest=0,
wIndex=0` so the firmware-side reset only works if BTH owns IF0
(which routes the vendor opcode `0xFCFE` to `btd_control_xfer_cb`).
If a future change ever puts a different driver at IF0, the script's
firmware-side path will stop working — fall back to either the VBUS
power cycle (only effective if `force_download_boot` was already
set, which it won't be after a clean boot) or send the opcode with
an explicit `wIndex` matching the BT HCI interface number. We hit
this exact loop while bringing this branch up.

## Out-of-band: removing the CDC log

This change deletes the firmware log channel entirely. The channel
had been silent on master anyway, so no regression in observable
behaviour. If we later want a log channel back, two options:

- Re-enable USB-Serial-JTAG console (adds bytes via the secondary
  USB PHY — but that would conflict with OTG since we share one
  PHY).
- Add a `LOG` characteristic to the GUD vendor interface as a sub-
  protocol (custom, but stays inside our existing IF). Probably the
  right answer if/when this matters.

Neither is in scope here.
