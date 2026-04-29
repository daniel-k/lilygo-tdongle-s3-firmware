// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2022 Xinyuan-LilyGO
/*
 * Lifted verbatim from Xinyuan-LilyGO/T-Dongle-S3 examples/lcd/bsp_lcd
 * (commit fda1b85). See licenses/T-Dongle-S3-LICENSE for the upstream MIT
 * terms.
 *
 * ST7735 panel driver for the esp_lcd component, used by the T-Dongle S3's
 * 0.96" 80x160 RGB565 panel. The IDF-bundled esp_lcd component only provides
 * st7789 by default; this is the small vendor companion that adds st7735.
 */

#pragma once

#include "driver/spi_master.h"
#include "esp_lcd_panel_vendor.h"

//--------------------------------------------------------------------------------
// System Function Command List and Description
//--------------------------------------------------------------------------------

#define ST7735_SWRESET 0x01   // Software Reset
#define ST7735_RDDID 0x04     // Read Display ID
#define ST7735_RDDST 0x09     // Read Display Status
#define ST7735_RDDPM 0x0A     // Read Display Power Mode
#define ST7735_RDDMADCTL 0x0B // Read Display MADCTL
#define ST7735_RDDCOLMOD 0x0C // Read Display Pixel Format
#define ST7735_RDDIM 0x0D     // Read Display Image Mode
#define ST7735_RDDSM 0x0E     // Read Display Signal Mode
#define ST7735_RDDSDR 0x0F    // Read Display Self-Diagnostic Result
#define ST7735_SLPIN 0x10     // Sleep In
#define ST7735_SLPOUT 0x11    // Sleep Out
#define ST7735_PTLON 0x12     // Partial Display Mode On
#define ST7735_NORON 0x13     // Normal Display Mode On
#define ST7735_INVOFF 0x20    // Display Inversion OFF
#define ST7735_INVON 0x21     // Display Inversion On
#define ST7735_GAMSET 0x26    // Gamma Set
#define ST7735_DISPOFF 0x28   // Display Off
#define ST7735_DISPON 0x29    // Display On
#define ST7735_CASET 0x2A     // Column Address Set
#define ST7735_RASET 0x2B     // Row Address Set
#define ST7735_RAMWR 0x2C     // Memory Write
#define ST7735_RGBSET 0x2D    // Color Setting for 4K, 65K and 262K
#define ST7735_RAMRD 0x2E     // Memory Read
#define ST7735_PTLAR 0x30     // Partial Area
#define ST7735_SCRLAR 0x33    // Scroll Area Set
#define ST7735_TEOFF 0x34     // Tearing Effect Line OFF
#define ST7735_TEON 0x35      // Tearing Effect Line ON
#define ST7735_MADCTL 0x36    // Memory Data Access Control
#define ST7735_VSCSAD 0x37    // Vertical Scroll Start Address of RAM
#define ST7735_IDMOFF 0x38    // Idle Mode Off
#define ST7735_IDMON 0x39     // Idle Mode On
#define ST7735_COLMOD 0x3A    // Interface Pixel Format
#define ST7735_RDID1 0xDA     // Read ID1 Value
#define ST7735_RDID2 0xDB     // Read ID2 Value
#define ST7735_RDID3 0xDC     // Read ID3 Value

//--------------------------------------------------------------------------------
// Panel Function Command List
//--------------------------------------------------------------------------------

#define ST7735_FRMCTR1 0xB1
#define ST7735_FRMCTR2 0xB2
#define ST7735_FRMCTR3 0xB3
#define ST7735_INVCTR 0xB4
#define ST7735_PWCTR1 0xC0
#define ST7735_PWCTR2 0xC1
#define ST7735_PWCTR3 0xC2
#define ST7735_PWCTR4 0xC3
#define ST7735_PWCTR5 0xC4
#define ST7735_VMCTR1 0xC5
#define ST7735_VMOFCTR 0xC7
#define ST7735_WRID2 0xD1
#define ST7735_WRID3 0xD2
#define ST7735_NVFCTR1 0xD9
#define ST7735_NVFCTR2 0xDE
#define ST7735_NVFCTR3 0xDF
#define ST7735_GMCTRP1 0xE0
#define ST7735_GMCTRN1 0xE1
#define ST7735_GCV 0xFC

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct
    {
        int cmd;
        const void *data;
        size_t data_bytes;
        unsigned int delay_ms;
    } st7735_lcd_init_cmd_t;

    typedef struct
    {
        const st7735_lcd_init_cmd_t *init_cmds;
        uint16_t init_cmds_size;
    } st7735_vendor_config_t;

    esp_err_t esp_lcd_new_panel_st7735(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel);

#define ST7735_PANEL_BUS_SPI_CONFIG(sclk, mosi, max_trans_sz) \
    {                                                         \
        .mosi_io_num = mosi,                                  \
        .miso_io_num = -1,                                    \
        .sclk_io_num = sclk,                                  \
        .quadwp_io_num = -1,                                  \
        .quadhd_io_num = -1,                                  \
        .data4_io_num = -1,                                   \
        .data5_io_num = -1,                                   \
        .data6_io_num = -1,                                   \
        .data7_io_num = -1,                                   \
        .max_transfer_sz = max_trans_sz,                      \
        .flags = 0,                                           \
        .intr_flags = 0                                       \
    }

#define ST7735_PANEL_IO_SPI_CONFIG(cs, dc, callback, callback_ctx) \
    {                                                              \
        .cs_gpio_num = cs,                                         \
        .dc_gpio_num = dc,                                         \
        .spi_mode = 0,                                             \
        .pclk_hz = 40 * 1000 * 1000,                               \
        .trans_queue_depth = 10,                                   \
        .on_color_trans_done = callback,                           \
        .user_ctx = callback_ctx,                                  \
        .lcd_cmd_bits = 8,                                         \
        .lcd_param_bits = 8,                                       \
    }

#ifdef __cplusplus
}
#endif
