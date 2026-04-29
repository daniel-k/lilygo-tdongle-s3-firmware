// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Daniel Roussel
/*
 * Build a 256-byte EDID = base block (from gud.c, untouched) + CEA-861
 * extension carrying a Microsoft Display Identification VSDB (OUI 0xCA125C,
 * version=1) that sets the DRM `non-desktop` connector property to true.
 *
 * Kernel reference: drivers/gpu/drm/drm_edid.c, drm_parse_microsoft_vsdb():
 *   - cea_db_is_microsoft_vsdb() requires payload length == 21 bytes
 *   - version 1 or 2 → unconditional non-desktop
 *   - version 3 + bit 6 of byte 5 cleared → non-desktop
 *   - we use version 1 (simplest, no flags to get right)
 *
 * VSDB byte layout inside the CEA extension (db[] = bytes from tag/length on):
 *   db[0]    = (TAG << 5) | LEN  = (3 << 5) | 21 = 0x75
 *   db[1..3] = OUI 0x5C 0x12 0xCA  (LSB first)
 *   db[4]    = Microsoft VSDB version  ← set to 1
 *   db[5..21]= reserved, all zeros
 *
 * Compiled in only on T-Dongle S3 builds — Super Mini has no GUD function.
 */

#ifdef BOARD_T_DONGLE_S3

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "gud.h"
#include "gud_edid_ext.h"

#define BASE_EDID_SIZE     128
#define EXT_BLOCK_SIZE     128
#define EXTENDED_EDID_SIZE (BASE_EDID_SIZE + EXT_BLOCK_SIZE)

#define MICROSOFT_OUI_BYTE0 0x5C
#define MICROSOFT_OUI_BYTE1 0x12
#define MICROSOFT_OUI_BYTE2 0xCA

/* CEA-861 vendor-specific data block tag is 3; payload length 21 = 3 OUI +
 * 18 vendor bytes. Microsoft VSDB version byte goes at vendor offset 0. */
#define VSDB_TAG_LEN_BYTE  ((3u << 5) | 21u)
#define VSDB_TOTAL_BYTES   (1 + 21)  /* tag/length byte + payload */

#define MICROSOFT_VSDB_VERSION_NON_DESKTOP 1

static uint8_t edid_block_checksum(const uint8_t *block, size_t len)
{
    uint8_t cs = 0;
    for (size_t i = 0; i < len; i++) cs += block[i];
    return (uint8_t)(0x100u - cs);
}

int gud_get_edid_extended(const struct gud_display *disp, void *data, size_t size)
{
    if (size == 0) return -GUD_STATUS_PROTOCOL_ERROR;

    /* Build the full 256-byte EDID locally, then copy the requested prefix.
     * The host sometimes asks for 128 bytes first (just the base block),
     * sees [126]=1 = "one extension follows", and re-asks for 256. */
    uint8_t edid[EXTENDED_EDID_SIZE];
    memset(edid, 0, sizeof(edid));

    /* Base block: pull from gud.c so we keep the verbatim-vendored EDID
     * generator as the single source of truth. We then bump the extension
     * count and recompute the checksum. */
    int n = gud_req_get(disp, GUD_REQ_GET_CONNECTOR_EDID, 0,
                        edid, BASE_EDID_SIZE);
    if (n != BASE_EDID_SIZE) {
        /* Upstream returned 0 (caller wLength too small) or an error;
         * pass through unchanged. */
        if (n <= 0) return n;
        size_t copy = (size_t)n < size ? (size_t)n : size;
        memcpy(data, edid, copy);
        return (int)copy;
    }

    edid[126] = 1;  /* one extension block follows */
    edid[127] = edid_block_checksum(edid, 127);

    /* CEA-861 extension block at offset 128. Layout:
     *   [0]   tag = 0x02 (CEA extension)
     *   [1]   revision = 3
     *   [2]   dtd_offset = 4 + VSDB_TOTAL = 26 (no DTDs follow our VSDB)
     *   [3]   flags (audio/video format support; 0 = none)
     *   [4..] data blocks: just one VSDB
     *   [127] checksum
     */
    uint8_t *ext = edid + BASE_EDID_SIZE;
    ext[0] = 0x02;
    ext[1] = 0x03;
    ext[2] = (uint8_t)(4 + VSDB_TOTAL_BYTES);
    ext[3] = 0;

    /* Microsoft VSDB starts at byte 4 of the extension block. */
    uint8_t *vsdb = ext + 4;
    vsdb[0] = VSDB_TAG_LEN_BYTE;
    vsdb[1] = MICROSOFT_OUI_BYTE0;
    vsdb[2] = MICROSOFT_OUI_BYTE1;
    vsdb[3] = MICROSOFT_OUI_BYTE2;
    vsdb[4] = MICROSOFT_VSDB_VERSION_NON_DESKTOP;
    /* vsdb[5..21] already zero from the memset */

    ext[127] = edid_block_checksum(ext, 127);

    size_t copy = size < EXTENDED_EDID_SIZE ? size : EXTENDED_EDID_SIZE;
    memcpy(data, edid, copy);
    return (int)copy;
}

#endif /* BOARD_T_DONGLE_S3 */
