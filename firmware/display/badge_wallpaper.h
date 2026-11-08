/* Device-local RGB565 wallpaper, streamed from flash through LVGL's line reader.
 * No embedded bitmap, filesystem or full-image RAM allocation. Registered and
 * used only by the UI task. The source descriptor is always 360x360 at zoom 256. */
#ifndef BADGE_WALLPAPER_H
#define BADGE_WALLPAPER_H
#include <stdint.h>
#include <string.h>
#define BADGE_WP_SLOT 0x2A0000u
#define BADGE_WP_LEGACY_SLOT 0x300000u
#define BADGE_WP_MAGIC 0x474D4B4Cu
#define BADGE_WP_MAX 360u
#define BADGE_WP_SIZE (16u + BADGE_WP_MAX * BADGE_WP_MAX * 2u)
typedef struct { uint32_t magic; uint16_t w, h; uint32_t size, reserved; } badge_wp_header;
static badge_wp_header badge_wp;
static uint32_t badge_wp_base;
static lv_img_dsc_t badge_wp_source;
static const uint8_t badge_wp_token[4] = {'B','W','P',1};
static lv_img_decoder_t *badge_wp_decoder;
static uint8_t badge_wp_row[BADGE_WP_MAX * 2u];
extern int tzflash_read(void *buf, unsigned int len, unsigned int offset);

static int badge_wp_valid(const badge_wp_header *h, unsigned limit)
{
    return h->magic == BADGE_WP_MAGIC && h->w >= 16 && h->h >= 16 &&
           h->w <= limit && h->h <= limit && h->size == (uint32_t)h->w*h->h*2u;
}

static lv_res_t badge_wp_info(lv_img_decoder_t *decoder, const void *src, lv_img_header_t *header)
{
    (void)decoder;
    if (src != &badge_wp_source || !badge_wp_base) return LV_RES_INV;
    *header = badge_wp_source.header;
    header->cf = LV_IMG_CF_TRUE_COLOR;
    return LV_RES_OK;
}

static lv_res_t badge_wp_open(lv_img_decoder_t *decoder, lv_img_decoder_dsc_t *dsc)
{
    if (badge_wp_info(decoder, dsc->src, &dsc->header) != LV_RES_OK) return LV_RES_INV;
    dsc->img_data = NULL; /* LVGL must call the bounded line reader. */
    return LV_RES_OK;
}

static lv_res_t badge_wp_read_line(lv_img_decoder_t *decoder, lv_img_decoder_dsc_t *dsc,
                                  lv_coord_t x, lv_coord_t y, lv_coord_t len, uint8_t *buf)
{
    uint32_t source_y, address;
    unsigned i;
    (void)decoder;
    if (dsc->src != &badge_wp_source || !badge_wp_base || !buf || x < 0 || y < 0 ||
        len <= 0 || y >= 360 || x >= 360 || len > 360-x) return LV_RES_INV;
    source_y = (uint32_t)y * badge_wp.h / 360u;
    address = badge_wp_base + 16u + source_y * badge_wp.w * 2u;
    if (badge_wp.w == 360) {
        /* Full resolution: read only the visible span directly into LVGL's row. */
        return tzflash_read(buf, (unsigned)len*2u, address + (unsigned)x*2u) < 0 ? LV_RES_INV : LV_RES_OK;
    }
    /* Preserve older small wallpapers without ever enlarging their RAM buffer.
     * New 360x360 records take the direct path above, with no resampling. */
    if (tzflash_read(badge_wp_row, badge_wp.w*2u, address) < 0) return LV_RES_INV;
    for (i = 0; i < (unsigned)len; ++i) {
        unsigned source_x = ((unsigned)x+i)*badge_wp.w/360u;
        buf[2*i] = badge_wp_row[2*source_x]; buf[2*i+1] = badge_wp_row[2*source_x+1];
    }
    return LV_RES_OK;
}

static const lv_img_dsc_t *lock_img_resolve(void)
{
    badge_wp_base = 0;
    if (tzflash_read(&badge_wp, sizeof(badge_wp), BADGE_WP_SLOT) >= 0 && badge_wp_valid(&badge_wp, 360))
        badge_wp_base = BADGE_WP_SLOT;
    else if (tzflash_read(&badge_wp, sizeof(badge_wp), BADGE_WP_LEGACY_SLOT) >= 0 && badge_wp_valid(&badge_wp, 144))
        badge_wp_base = BADGE_WP_LEGACY_SLOT;
    if (!badge_wp_base) return NULL; /* Existing black screen/instructions are the fallback. */
    if (!badge_wp_decoder) {
        badge_wp_decoder = lv_img_decoder_create();
        if (!badge_wp_decoder) { badge_wp_base = 0; return NULL; }
        lv_img_decoder_set_info_cb(badge_wp_decoder, badge_wp_info);
        lv_img_decoder_set_open_cb(badge_wp_decoder, badge_wp_open);
        lv_img_decoder_set_read_line_cb(badge_wp_decoder, badge_wp_read_line);
    }
    memset(&badge_wp_source, 0, sizeof(badge_wp_source));
    badge_wp_source.header.w = badge_wp_source.header.h = 360;
    /* Vendor LVGL rejects variable sources with NULL data before consulting
     * custom decoders. Use an opaque user-encoded token, never a fake pixel map;
     * the built-in decoder cannot accidentally treat it as a full RGB565 image. */
    badge_wp_source.header.cf = LV_IMG_CF_USER_ENCODED_0;
    badge_wp_source.data_size = sizeof(badge_wp_token);
    badge_wp_source.data = badge_wp_token;
    return &badge_wp_source;
}
#endif
