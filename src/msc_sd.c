// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Daniel Roussel
/*
 * SDMMC slot 1 (4-bit mode) bridge to the TinyUSB MSC class driver.
 *
 * Pin map per the LilyGo T-Dongle S3 silkscreen:
 *   CLK = GPIO12,  CMD = GPIO16
 *   D0  = GPIO14,  D1  = GPIO17,  D2  = GPIO21,  D3  = GPIO18
 *
 * No card-detect / write-protect lines are wired on the dongle; we
 * pass SDMMC_SLOT_NO_CD / SDMMC_SLOT_NO_WP. Internal pullups are
 * enabled as a belt-and-braces measure even though SDMMC strictly
 * requires external pullups (the dongle has them on the data lines).
 *
 * Compiled in only on T-Dongle S3 builds (BOARD_T_DONGLE_S3). Plain
 * ESP32-S3 boards have no µSD slot wired, so the entire MSC function
 * is omitted from the firmware on those targets.
 */

#ifdef BOARD_T_DONGLE_S3

#include <string.h>

#include "esp_log.h"
#include "esp_check.h"

#include "driver/sdmmc_host.h"
#include "driver/sdmmc_default_configs.h"
#include "sdmmc_cmd.h"

#include "tusb.h"
#include "class/msc/msc.h"

#include "msc_sd.h"

static const char *TAG = "MSC_SD";

#define PIN_SD_CLK   12
#define PIN_SD_CMD   16
#define PIN_SD_D0    14
#define PIN_SD_D1    17
#define PIN_SD_D2    21
#define PIN_SD_D3    18

static sdmmc_card_t s_card;
static bool s_card_ready;

void msc_sd_init(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;
    /* Cap clock at 20 MHz initially (SDMMC_FREQ_DEFAULT). The peripheral
     * + card negotiation will raise this if the card supports it. */

    sdmmc_slot_config_t slot = {
        .clk = PIN_SD_CLK,
        .cmd = PIN_SD_CMD,
        .d0  = PIN_SD_D0,
        .d1  = PIN_SD_D1,
        .d2  = PIN_SD_D2,
        .d3  = PIN_SD_D3,
        .d4 = -1, .d5 = -1, .d6 = -1, .d7 = -1,
        .cd = SDMMC_SLOT_NO_CD,
        .wp = SDMMC_SLOT_NO_WP,
        .width = 4,
        .flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP,
    };

    esp_err_t err = sdmmc_host_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sdmmc_host_init: %s", esp_err_to_name(err));
        return;
    }
    err = sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sdmmc_host_init_slot: %s", esp_err_to_name(err));
        return;
    }
    err = sdmmc_card_init(&host, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sdmmc_card_init: %s — MSC will report no medium",
                 esp_err_to_name(err));
        return;
    }

    s_card_ready = true;
    ESP_LOGI(TAG, "SD card ready: %llu MB, %u-bit, %u kHz",
             ((uint64_t)s_card.csd.capacity * s_card.csd.sector_size) >> 20,
             s_card.log_bus_width,
             s_card.real_freq_khz);
}

bool msc_sd_ready(void)
{
    return s_card_ready;
}

/* ---- TinyUSB MSC callbacks ---------------------------------------- */

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16],
                        uint8_t product_rev[4])
{
    (void)lun;
    memcpy(vendor_id,   "Espressif",      8);
    memcpy(product_id,  "ESP32-S3 uSD   ", 16);
    memcpy(product_rev, "1.00",            4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    (void)lun;
    return s_card_ready;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    (void)lun;
    if (!s_card_ready) {
        *block_count = 0;
        *block_size = 0;
        return;
    }
    *block_count = s_card.csd.capacity;
    *block_size  = s_card.csd.sector_size;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition,
                           bool start, bool load_eject)
{
    (void)lun; (void)power_condition; (void)start; (void)load_eject;
    /* Accept eject without doing anything — we don't have a way to
     * "unmount" the card, and a re-insert isn't physically supported
     * on this dongle. */
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t bufsize)
{
    (void)lun;
    if (!s_card_ready) return -1;
    /* TinyUSB hands us aligned full-sector reads when EP_BUFSIZE matches
     * the sector size; offset is unused in that case. Defensively, we
     * still bail if it ever fires non-zero. */
    if (offset != 0) return -1;
    if (bufsize % s_card.csd.sector_size) return -1;

    size_t sector_count = bufsize / s_card.csd.sector_size;
    esp_err_t err = sdmmc_read_sectors(&s_card, buffer, lba, sector_count);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read lba=%lu cnt=%u: %s",
                 (unsigned long)lba, (unsigned)sector_count,
                 esp_err_to_name(err));
        return -1;
    }
    return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           uint8_t *buffer, uint32_t bufsize)
{
    (void)lun;
    if (!s_card_ready) return -1;
    if (offset != 0) return -1;
    if (bufsize % s_card.csd.sector_size) return -1;

    size_t sector_count = bufsize / s_card.csd.sector_size;
    esp_err_t err = sdmmc_write_sectors(&s_card, buffer, lba, sector_count);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "write lba=%lu cnt=%u: %s",
                 (unsigned long)lba, (unsigned)sector_count,
                 esp_err_to_name(err));
        return -1;
    }
    return (int32_t)bufsize;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16],
                        void *buffer, uint16_t bufsize)
{
    (void)lun; (void)scsi_cmd; (void)buffer; (void)bufsize;
    /* Reject anything we don't explicitly handle (PREVENT_ALLOW_REMOVAL,
     * vendor commands, etc.) — TinyUSB will translate the negative
     * return into a SCSI CHECK_CONDITION response. */
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
    return -1;
}

#endif /* BOARD_T_DONGLE_S3 */
