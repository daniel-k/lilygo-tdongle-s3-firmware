// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Daniel Roussel
/*
 * Display configuration for the GUD function.
 * Pin map and panel parameters target the LilyGo T-Dongle S3 ST7735 0.96"
 * 80x160 RGB565 panel. On other boards (e.g. the Super Mini, no LCD) the
 * SPI/LCD init still runs but produces no visible output.
 */

#ifndef GUD_DISPLAY_H_
#define GUD_DISPLAY_H_

#include "gud.h"

/* Native panel orientation: portrait, 80x160, RGB565. */
#define GUD_DISPLAY_WIDTH   80
#define GUD_DISPLAY_HEIGHT  160
#define GUD_FRAMEBUFFER_BYTES (GUD_DISPLAY_WIDTH * GUD_DISPLAY_HEIGHT * 2)

extern const struct gud_display gud_display_cfg;

/* Initialize SPI bus, ST7735 panel, and LEDC backlight. Must be called
 * before USB enumerates so the panel is ready for the first flush. */
void gud_display_panel_init(void);

#endif
