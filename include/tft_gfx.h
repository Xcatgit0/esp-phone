#ifndef TFT_GFX_H
#define TFT_GFX_H

#include <stdint.h>
#include <stdbool.h>
#include "tft.h"

/* ------------------------------------------------------------
 * Basic drawing primitives (built on top of tft_draw_pixel)
 * ------------------------------------------------------------ */
void gfx_draw_hline(int x, int y, int w, uint16_t color);
void gfx_draw_vline(int x, int y, int h, uint16_t color);
void gfx_draw_line(int x0, int y0, int x1, int y1, uint16_t color);
void gfx_draw_rect(int x, int y, int w, int h, uint16_t color);
void gfx_fill_rect(int x, int y, int w, int h, uint16_t color);
void gfx_draw_circle(int cx, int cy, int r, uint16_t color);
void gfx_fill_circle(int cx, int cy, int r, uint16_t color);

/* ------------------------------------------------------------
 * Linux console font support (PSF1 / PSF2 format)
 * Load any .psf / .psfu file copied from a Linux system,
 * e.g. /usr/share/consolefonts/default8x16.psfu
 * ------------------------------------------------------------ */
typedef struct {
    uint8_t  *glyphs;
    uint32_t  glyph_count;
    uint32_t  glyph_size;   /* bytes per glyph */
    uint32_t  width;
    uint32_t  height;
    bool      is_psf2;
} gfx_font_t;

gfx_font_t *gfx_font_load(const char *path);
void        gfx_font_free(gfx_font_t *font);

void gfx_draw_char(int x, int y, char c, uint16_t fg, uint16_t bg,
                    const gfx_font_t *font);
void gfx_draw_string(int x, int y, const char *str, uint16_t fg,
                      uint16_t bg, const gfx_font_t *font);

void        gfx_set_default_font(gfx_font_t *font);
gfx_font_t *gfx_get_default_font(void);

#endif