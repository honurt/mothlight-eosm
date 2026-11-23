/*
 * Minimal 8-bit drawing for the EOS M screen overlay (720 x 480, palette indices).
 *
 * Everything writes straight into a canvas (on the camera: Canon's idle
 * bitmap buffer; on a PC: a plain array) with whole-row memset, which is
 * an order of magnitude faster than per-pixel bmp_putpixel calls.
 * All functions clip to the canvas clip rectangle.
 */
#ifndef EOSM_UI_GFX_H
#define EOSM_UI_GFX_H

#include <stdint.h>

#define UI_W 720
#define UI_H 480

struct canvas
{
    uint8_t * px;           /* pixel (0,0) */
    int pitch;              /* bytes per row (960 on the EOS M) */
    int cx0, cy0, cx1, cy1; /* clip rectangle, x1/y1 exclusive */
    int bx0, by0, bx1, by1; /* hard bounds of the memory behind px: no clip ever goes past them */
};

/* Magic Lantern palette indices used by the UI (same on every DIGIC 5 camera) */
enum ui_color
{
    UC_CLEAR    = 0x00,     /* fully transparent: LiveView shows through */
    UC_WHITE    = 0x01,
    UC_BLACK    = 0x02,
    UC_SHADE    = 0x03,     /* semi-transparent black */
    UC_CYAN     = 0x05,
    UC_GREEN1   = 0x06,
    UC_GREEN    = 0x07,
    UC_RED      = 0x08,
    UC_LIGHT_BLUE = 0x09,
    UC_BLUE     = 0x0B,
    UC_DARK_RED = 0x0C,
    UC_MAGENTA  = 0x0E,
    UC_YELLOW   = 0x0F,
    UC_ORANGE   = 0x13,
};
#define UC_GRAY(pct) ((uint8_t)(38 + (pct) * 41 / 100))   /* ML's 42-step gray ramp */

void gfx_clip(struct canvas * c, int x0, int y0, int x1, int y1);
void gfx_clip_all(struct canvas * c);
void gfx_fill(struct canvas * c, int x, int y, int w, int h, uint8_t color);
void gfx_round(struct canvas * c, int x, int y, int w, int h, int r, uint8_t color);
void gfx_round_frame(struct canvas * c, int x, int y, int w, int h, int r, uint8_t color);
/* horizontal bar: fill_q8 of 256 = full; the part past `mark_q8` is drawn in `over`,
 * and a 1 px tick in `tick` marks it (tick 0 = no tick) */
void gfx_bar(struct canvas * c, int x, int y, int w, int h, int fill_q8, int mark_q8,
             uint8_t bg, uint8_t fg, uint8_t over, uint8_t tick);
/* ordered 2x2 dither of `color` over what is there: level 0..4 (4 = solid). Used for fades. */
void gfx_dither(struct canvas * c, int x, int y, int w, int h, uint8_t color, int level);
void gfx_dot(struct canvas * c, int cx, int cy, int r, uint8_t color);
/* small filled triangle pointing up/down/left/right (0..3), for button glyphs */
void gfx_tri(struct canvas * c, int cx, int cy, int size, int dir, uint8_t color);
/* 2-bit icon: values 1..3 map to colors[1..3], 0 is transparent */
void gfx_icon(struct canvas * c, int x, int y, int w, int h, const uint8_t * px, const uint8_t colors[4]);

#endif
