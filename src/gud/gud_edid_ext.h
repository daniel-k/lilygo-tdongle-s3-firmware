// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Daniel Roussel
/*
 * EDID extension generator. Wraps the verbatim-vendored gud_req_get_
 * connector_edid() in gud.c with a 256-byte EDID that adds a CEA-861
 * extension block carrying the Microsoft "specialized monitor" VSDB —
 * which makes the Linux DRM core mark the connector as `non-desktop`.
 * That tells GNOME / mutter (and any other normal compositor) to leave
 * the panel alone, replacing the udev seat-rerouting hack.
 *
 * Lives in its own file so the upstream gud.c stays unmodified.
 */

#ifndef GUD_EDID_EXT_H_
#define GUD_EDID_EXT_H_

#include <stddef.h>

#include "gud.h"

/* Same signature as gud_req_get() for the GUD_REQ_GET_CONNECTOR_EDID case
 * — drop-in replacement for that one request. Returns the number of bytes
 * written, or a negative GUD_STATUS_* value on error. */
int gud_get_edid_extended(const struct gud_display *disp, void *data, size_t size);

#endif
