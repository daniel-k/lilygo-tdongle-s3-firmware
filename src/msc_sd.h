// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Daniel Roussel
/*
 * USB Mass Storage Class glue: exposes the T-Dongle S3's onboard µSD
 * card slot as a generic USB removable drive. Uses ESP-IDF's
 * `sdmmc_host` peripheral driver in 4-bit mode (slot 1, GPIO matrix
 * routes to the pins LilyGo wired the board for).
 *
 * The TinyUSB MSC class driver (CFG_TUD_MSC=1 in tusb_config.h) calls
 * the tud_msc_*_cb functions implemented in msc_sd.c — those translate
 * SCSI READ_10 / WRITE_10 commands to sdmmc_read_sectors /
 * sdmmc_write_sectors against the cached card handle.
 */

#ifndef MSC_SD_H_
#define MSC_SD_H_

#include <stdbool.h>

/* Initialise the SDMMC peripheral, probe the card, cache the
 * resulting card handle for later read/write callbacks. Must be
 * called once before tud_rhport_init() so the card is ready by the
 * time the host enumerates. Non-fatal on no-card-present: the
 * firmware still comes up with HCI + GUD working, MSC just reports
 * "no medium" until a card is inserted (and this isn't currently
 * re-probed at runtime — but the T-Dongle is a fixed-card device, so
 * cards aren't expected to be hot-swapped). */
void msc_sd_init(void);

/* True once a card is detected and ready for I/O. The TinyUSB
 * tud_msc_test_unit_ready_cb wires through to this. */
bool msc_sd_ready(void);

#endif
