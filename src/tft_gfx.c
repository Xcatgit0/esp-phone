#include "tft_gfx.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------
 * Basic primitives
 * ------------------------------------------------------------ */
void gfx_draw_hline(int x, int y, int w, uint16_t color)
{
    for (int i = 0; i < w; i++)
        tft_draw_pixel(x + i, y, color);
}

void gfx_draw_vline(int x, int y, int h, uint16_t color)
{
    for (int i = 0; i < h; i++)
        tft_draw_pixel(x, y + i, color);
}

void gfx_draw_line(int x0, int y0, int x1, int y1, uint16_t color)
{
    /* Bresenham's line algorithm */
    int dx = abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while (1) {
        tft_draw_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1)
            break;

        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void gfx_draw_rect(int x, int y, int w, int h, uint16_t color)
{
    gfx_draw_hline(x, y, w, color);
    gfx_draw_hline(x, y + h - 1, w, color);
    gfx_draw_vline(x, y, h, color);
    gfx_draw_vline(x + w - 1, y, h, color);
}

void gfx_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    for (int row = 0; row < h; row++)
        gfx_draw_hline(x, y + row, w, color);
}

void gfx_draw_circle(int cx, int cy, int r, uint16_t color)
{
    /* Midpoint circle algorithm */
    int x = r, y = 0, err = 0;

    while (x >= y) {
        tft_draw_pixel(cx + x, cy + y, color);
        tft_draw_pixel(cx + y, cy + x, color);
        tft_draw_pixel(cx - y, cy + x, color);
        tft_draw_pixel(cx - x, cy + y, color);
        tft_draw_pixel(cx - x, cy - y, color);
        tft_draw_pixel(cx - y, cy - x, color);
        tft_draw_pixel(cx + y, cy - x, color);
        tft_draw_pixel(cx + x, cy - y, color);

        y += 1;
        if (err <= 0) err += 2 * y + 1;
        if (err > 0)  { x -= 1; err -= 2 * x + 1; }
    }
}

void gfx_fill_circle(int cx, int cy, int r, uint16_t color)
{
    for (int y = -r; y <= r; y++) {
        int span = (int)sqrtf((float)(r * r - y * y));
        gfx_draw_hline(cx - span, cy + y, span * 2 + 1, color);
    }
}

/* ------------------------------------------------------------
 * PSF font loading (Linux console font format)
 * ------------------------------------------------------------ */
#define PSF1_MAGIC0 0x36
#define PSF1_MAGIC1 0x04

#define PSF2_MAGIC0 0x72
#define PSF2_MAGIC1 0xb5
#define PSF2_MAGIC2 0x4a
#define PSF2_MAGIC3 0x86

static gfx_font_t *s_default_font = NULL;

gfx_font_t *gfx_font_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        LOG_ERROR("gfx_font_load: cannot open %s", path);
        return NULL;
    }

    uint8_t magic[4];
    if (fread(magic, 1, 4, f) != 4) {
        LOG_ERROR("gfx_font_load: file too short: %s", path);
        fclose(f);
        return NULL;
    }

    gfx_font_t *font = calloc(1, sizeof(gfx_font_t));
    if (font == NULL) { fclose(f); return NULL; }

    if (magic[0] == PSF1_MAGIC0 && magic[1] == PSF1_MAGIC1) {
        /* PSF1: magic(2) mode(1) charsize(1) */
        uint8_t mode     = magic[2];
        uint8_t charsize = magic[3];

        font->is_psf2     = false;
        font->width       = 8;
        font->height      = charsize;
        font->glyph_size  = charsize;
        font->glyph_count = (mode & 0x01) ? 512 : 256;

        size_t data_size = (size_t)font->glyph_count * font->glyph_size;
        font->glyphs = malloc(data_size);

        if (font->glyphs == NULL || fread(font->glyphs, 1, data_size, f) != data_size) {
            LOG_ERROR("gfx_font_load: failed reading PSF1 glyph data");
            free(font->glyphs); free(font); fclose(f);
            return NULL;
        }

        LOG_INFO("Loaded PSF1 font: %ux%u, %u glyphs",
                  (unsigned)font->width, (unsigned)font->height,
                  (unsigned)font->glyph_count);
    }
    else if (magic[0] == PSF2_MAGIC0 && magic[1] == PSF2_MAGIC1 &&
             magic[2] == PSF2_MAGIC2 && magic[3] == PSF2_MAGIC3) {
        /* PSF2 header, 32 bytes total, magic (4) already read */
        uint32_t hdr[7]; /* version, headersize, flags, numglyph, bytesperglyph, height, width */

        if (fread(hdr, sizeof(uint32_t), 7, f) != 7) {
            LOG_ERROR("gfx_font_load: failed reading PSF2 header");
            free(font); fclose(f);
            return NULL;
        }

        uint32_t headersize    = hdr[1];
        uint32_t numglyph      = hdr[3];
        uint32_t bytesperglyph = hdr[4];
        uint32_t height        = hdr[5];
        uint32_t width         = hdr[6];

        font->is_psf2     = true;
        font->width       = width;
        font->height      = height;
        font->glyph_size  = bytesperglyph;
        font->glyph_count = numglyph;

        fseek(f, headersize, SEEK_SET); /* skip to real glyph offset */

        size_t data_size = (size_t)numglyph * bytesperglyph;
        font->glyphs = malloc(data_size);

        if (font->glyphs == NULL || fread(font->glyphs, 1, data_size, f) != data_size) {
            LOG_ERROR("gfx_font_load: failed reading PSF2 glyph data");
            free(font->glyphs); free(font); fclose(f);
            return NULL;
        }

        LOG_INFO("Loaded PSF2 font: %ux%u, %u glyphs",
                  (unsigned)font->width, (unsigned)font->height,
                  (unsigned)font->glyph_count);
    }
    else {
        LOG_ERROR("gfx_font_load: unrecognized font magic in %s", path);
        free(font); fclose(f);
        return NULL;
    }

    fclose(f);
    return font;
}

void gfx_font_free(gfx_font_t *font)
{
    if (font == NULL) return;
    free(font->glyphs);
    free(font);
}

void gfx_set_default_font(gfx_font_t *font) { s_default_font = font; }
gfx_font_t *gfx_get_default_font(void)      { return s_default_font; }

void gfx_draw_char(int x, int y, char c, uint16_t fg, uint16_t bg,
                    const gfx_font_t *font)
{
    if (font == NULL) font = s_default_font;
    if (font == NULL || font->glyphs == NULL) return;

    uint32_t index = (uint8_t)c;
    if (index >= font->glyph_count)
        index = 0; /* fallback glyph */

    const uint8_t *glyph = font->glyphs + (index * font->glyph_size);
    uint32_t bytes_per_row = (font->width + 7) / 8;

    for (uint32_t row = 0; row < font->height; row++) {
        for (uint32_t col = 0; col < font->width; col++) {
            uint32_t byte_index = row * bytes_per_row + (col / 8);
            uint8_t  bit_mask   = 0x80 >> (col % 8);
            bool     pixel_on   = (glyph[byte_index] & bit_mask) != 0;

            tft_draw_pixel(x + col, y + row, pixel_on ? fg : bg);
        }
    }
}

void gfx_draw_string(int x, int y, const char *str, uint16_t fg,
                      uint16_t bg, const gfx_font_t *font)
{
    if (font == NULL) font = s_default_font;
    if (font == NULL) return;

    int cursor_x = x, cursor_y = y;

    for (const char *p = str; *p != '\0'; p++) {
        if (*p == '\n') {
            cursor_x = x;
            cursor_y += font->height;
            continue;
        }
        gfx_draw_char(cursor_x, cursor_y, *p, fg, bg, font);
        cursor_x += font->width;
    }
}