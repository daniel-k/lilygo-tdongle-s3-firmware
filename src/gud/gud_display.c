// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Daniel Roussel
/*
 * GUD display backend for the LilyGo T-Dongle S3 ST7735 panel.
 *
 * The panel is wired with native orientation portrait 80x160 (RGB565), with
 * an active-area offset of (26, 1) inside the ST7735's 132x162 controller
 * RAM. We declare the GUD config as 80x160 so the host pushes pixel data
 * tightly packed for the rect, and the ST7735 driver applies the offset
 * via its set_gap() in CASET/RASET.
 *
 * Pin map (matches LilyGo's bsp_lcd example):
 *   MOSI = GPIO3   CLK  = GPIO5   CS = GPIO4
 *   DC   = GPIO2   RST  = GPIO1   BL = GPIO38 (LEDC PWM)
 *
 * write_buffer_cb / SPI synchronization: esp_lcd_panel_draw_bitmap is
 * non-blocking; the SPI peripheral DMAs from the source buffer (our GUD
 * framebuffer). The next host-driven bulk OUT writes to the same buffer,
 * so we serialize: a binary semaphore is given by the on_color_trans_done
 * callback when SPI is done, and write_buffer_cb takes it before queueing
 * the next draw.
 *
 * Compiled in only on T-Dongle S3 builds — the panel is hardware-specific.
 */

#ifdef BOARD_T_DONGLE_S3

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"

#include "esp_lcd_st7735.h"
#include "gud.h"
#include "gud_display.h"

static const char *TAG = "GUD_DISP";

#define LCD_HOST          SPI2_HOST
#define PIN_MOSI          3
#define PIN_CLK           5
#define PIN_CS            4
#define PIN_DC            2
#define PIN_RST           1
#define PIN_BL            38

#define BL_LEDC_TIMER     LEDC_TIMER_0
#define BL_LEDC_MODE      LEDC_LOW_SPEED_MODE
#define BL_LEDC_CHANNEL   LEDC_CHANNEL_0
#define BL_LEDC_FREQ_HZ   1000
#define BL_LEDC_RES       LEDC_TIMER_8_BIT
/* T-Dongle S3 backlight is inverted: duty=0 → full brightness,
 * duty=255 → off. Matches LilyGo's example "ledcWrite(0); // max brightness". */
#define BL_DUTY_ON        0
#define BL_DUTY_OFF       255

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t    s_panel;
static SemaphoreHandle_t         s_spi_done;
static bool                      s_panel_ready;

static bool IRAM_ATTR on_color_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                          esp_lcd_panel_io_event_data_t *edata,
                                          void *user_ctx)
{
    BaseType_t task_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_spi_done, &task_woken);
    return task_woken == pdTRUE;
}

static void backlight_init(void)
{
    ledc_timer_config_t tcfg = {
        .speed_mode      = BL_LEDC_MODE,
        .timer_num       = BL_LEDC_TIMER,
        .duty_resolution = BL_LEDC_RES,
        .freq_hz         = BL_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&tcfg));

    ledc_channel_config_t ccfg = {
        .gpio_num   = PIN_BL,
        .speed_mode = BL_LEDC_MODE,
        .channel    = BL_LEDC_CHANNEL,
        .timer_sel  = BL_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ccfg));
}

static void backlight_set(uint8_t duty)
{
    ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty);
    ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
}

/* Fill panel GRAM with white before DISPON so a quick power-cycle never
 * shows leftover RAM contents. RGB565 white is 0xFFFF, byte-swap-symmetric,
 * so memset(0xFF) works in either byte order. */
static void clear_panel_white(void)
{
    uint8_t *buf = heap_caps_malloc(GUD_FRAMEBUFFER_BYTES,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!buf) {
        ESP_LOGW(TAG, "clear_panel_white: alloc failed; skipping");
        return;
    }
    memset(buf, 0xFF, GUD_FRAMEBUFFER_BYTES);

    xSemaphoreTake(s_spi_done, portMAX_DELAY);
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, 0,
                                              GUD_DISPLAY_WIDTH,
                                              GUD_DISPLAY_HEIGHT, buf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "clear_panel_white draw failed: %s", esp_err_to_name(err));
        xSemaphoreGive(s_spi_done);
        heap_caps_free(buf);
        return;
    }
    /* Wait for DMA to drain so the buffer is safe to free, then restore
     * the "given" state for write_buffer_cb's first take. */
    xSemaphoreTake(s_spi_done, portMAX_DELAY);
    xSemaphoreGive(s_spi_done);
    heap_caps_free(buf);
}

void gud_display_panel_init(void)
{
    s_spi_done = xSemaphoreCreateBinary();
    xSemaphoreGive(s_spi_done);

    backlight_init();
    /* Force backlight on at init; we ignore gud's display_enable signal
     * (see display_enable_cb / controller_enable_cb for rationale). */
    backlight_set(BL_DUTY_ON);

    spi_bus_config_t bus_cfg = ST7735_PANEL_BUS_SPI_CONFIG(
        PIN_CLK, PIN_MOSI, GUD_FRAMEBUFFER_BYTES);
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = ST7735_PANEL_IO_SPI_CONFIG(
        PIN_CS, PIN_DC, on_color_trans_done, NULL);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &s_io));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_RST,
        .color_space    = ESP_LCD_COLOR_SPACE_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7735(s_io, &panel_cfg, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, 26, 1));

    clear_panel_white();
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    s_panel_ready = true;
    ESP_LOGI(TAG, "ST7735 panel ready (80x160 RGB565, gap 26,1)");
}

static int controller_enable_cb(const struct gud_display *disp, uint8_t enable)
{
    (void)disp;
    ESP_LOGI(TAG, "controller_enable=%u (backlight forced on)", enable);
    /* Always keep DISPON — gud sends DISPOFF when the DRM pipe disables,
     * which fires every time the last client closes /dev/fb2 (so even a
     * single `dd > /dev/fb2` cycle would blank the panel). Trade DPMS
     * power saving for a panel that's reliably visible. */
    if (s_panel_ready) {
        esp_lcd_panel_disp_on_off(s_panel, true);
    }
    return 0;
}

static int display_enable_cb(const struct gud_display *disp, uint8_t enable)
{
    (void)disp;
    ESP_LOGI(TAG, "display_enable=%u (ignored — backlight is force-on)", enable);
    return 0;
}

static int state_commit_cb(const struct gud_display *disp,
                           const struct gud_state_req *state, uint8_t num_properties)
{
    (void)disp;
    ESP_LOGI(TAG, "state_commit mode=%ux%u format=0x%02x num_props=%u",
             state->mode.hdisplay, state->mode.vdisplay, state->format, num_properties);
    return 0;
}

/* Host RGB565 is host-native = little-endian (low byte first in memory).
 * The ST7735 reads each 16-bit pixel MSB-first off the SPI wire, so a raw
 * pass-through would map host-red 0xF800 (bytes 0x00, 0xF8) to wire pixel
 * 0x00F8 — a dim blue. Swap pairs in-place before queueing the DMA. */
static inline void rgb565_byte_swap_inplace(void *buf, size_t bytes)
{
    /* Walk in 32-bit chunks for speed; bytes is always even (RGB565). */
    uint32_t *p = (uint32_t *)buf;
    size_t words = bytes >> 2;
    while (words--) {
        uint32_t v = *p;
        *p++ = ((v & 0xff00ff00u) >> 8) | ((v & 0x00ff00ffu) << 8);
    }
    if (bytes & 2) {
        uint16_t *p16 = (uint16_t *)p;
        uint16_t v = *p16;
        *p16 = (uint16_t)((v << 8) | (v >> 8));
    }
}

static void write_buffer_cb(const struct gud_display *disp,
                            const struct gud_set_buffer_req *set_buf, void *buf)
{
    (void)disp;
    if (!s_panel_ready) return;

    size_t bytes = (size_t)set_buf->width * set_buf->height * 2;

    /* Wait for the previous SPI flush to finish so the source buffer
     * (the GUD framebuffer) isn't overwritten by the next bulk receive
     * mid-DMA, AND the byte-swap doesn't trample bytes still being read
     * by the previous transfer. */
    xSemaphoreTake(s_spi_done, portMAX_DELAY);

    rgb565_byte_swap_inplace(buf, bytes);

    esp_err_t err = esp_lcd_panel_draw_bitmap(
        s_panel,
        set_buf->x, set_buf->y,
        set_buf->x + set_buf->width, set_buf->y + set_buf->height,
        buf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "draw_bitmap failed: %s", esp_err_to_name(err));
        /* on_color_trans_done won't fire — give the semaphore back so
         * we don't deadlock the next frame. */
        xSemaphoreGive(s_spi_done);
    }
}

static const uint8_t pixel_formats[] = {
    GUD_PIXEL_FORMAT_RGB565,
};

static const struct gud_display_edid edid_info = {
    .name         = "T-Dongle S3",
    .pnp          = "ESP",
    .product_code = 0x614D,
    .year         = 2025,
    .width_mm     = 18,
    .height_mm    = 9,
    .bit_depth    = 6,
    .gamma        = 220,
};

const struct gud_display gud_display_cfg = {
    .width  = GUD_DISPLAY_WIDTH,
    .height = GUD_DISPLAY_HEIGHT,

    .compression     = 0,
    .max_buffer_size = 0,

    .formats     = pixel_formats,
    .num_formats = sizeof(pixel_formats) / sizeof(pixel_formats[0]),

    .edid = &edid_info,

    .controller_enable = controller_enable_cb,
    .display_enable    = display_enable_cb,
    .state_commit      = state_commit_cb,
    .write_buffer      = write_buffer_cb,
};

#endif /* BOARD_T_DONGLE_S3 */
