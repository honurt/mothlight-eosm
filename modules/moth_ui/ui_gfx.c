#include "ui_gfx.h"

#ifdef MODULE
    #include <dryos.h>
#else
    #include <string.h>
#endif

void gfx_clip(struct canvas * c, int x0, int y0, int x1, int y1)
{
    c->cx0 = x0 < c->bx0 ? c->bx0 : x0;    c->cy0 = y0 < c->by0 ? c->by0 : y0;
    c->cx1 = x1 > c->bx1 ? c->bx1 : x1;    c->cy1 = y1 > c->by1 ? c->by1 : y1;
}

void gfx_clip_all(struct canvas * c) { gfx_clip(c, 0, 0, UI_W, UI_H); }

static inline void span(struct canvas * c, int x0, int x1, int y, uint8_t color)
{
    if (y < c->cy0 || y >= c->cy1) return;
    if (x0 < c->cx0) x0 = c->cx0;
    if (x1 > c->cx1) x1 = c->cx1;
    if (x1 > x0) memset(c->px + y * c->pitch + x0, color, x1 - x0);
}

void gfx_fill(struct canvas * c, int x, int y, int w, int h, uint8_t color)
{
    int y0 = y < c->cy0 ? c->cy0 : y;
    int y1 = y + h > c->cy1 ? c->cy1 : y + h;
    for (int yy = y0; yy < y1; yy++) span(c, x, x + w, yy, color);
}

/* inset of row i (0 = top row) of a corner with radius r: integer circle, no sqrt */
static int corner_inset(int r, int i)
{
    int dy = 2 * (r - i) - 1;           /* doubled distance from the circle centre */
    int rr = 4 * r * r;
    int dx = 2 * r;
    while (dx > 0 && dx * dx + dy * dy > rr) dx--;
    return r - (dx + 1) / 2;
}

void gfx_round(struct canvas * c, int x, int y, int w, int h, int r, uint8_t color)
{
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    for (int i = 0; i < h; i++)
    {
        int k = i < r ? i : (i >= h - r ? h - 1 - i : -1);
        int in = k >= 0 ? corner_inset(r, k) : 0;
        span(c, x + in, x + w - in, y + i, color);
    }
}

void gfx_round_frame(struct canvas * c, int x, int y, int w, int h, int r, uint8_t color)
{
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    for (int i = 0; i < h; i++)
    {
        int k = i < r ? i : (i >= h - r ? h - 1 - i : -1);
        int in = k >= 0 ? corner_inset(r, k) : 0;
        if (i == 0 || i == h - 1) { span(c, x + in, x + w - in, y + i, color); continue; }
        /* the outline is as wide as the step to the next row, so curves stay closed */
        int kn = (i + 1 < r) ? i + 1 : (i + 1 >= h - r ? h - 2 - i : -1);
        int kp = (i - 1 < r) ? i - 1 : (i - 1 >= h - r ? h - i : -1);
        int inn = kn >= 0 ? corner_inset(r, kn) : 0;
        int inp = kp >= 0 ? corner_inset(r, kp) : 0;
        int edge = (inn < inp ? inp : inn) - in;
        if (edge < 1) edge = 1;
        span(c, x + in, x + in + edge, y + i, color);
        span(c, x + w - in - edge, x + w - in, y + i, color);
    }
}

void gfx_bar(struct canvas * c, int x, int y, int w, int h, int fill_q8, int mark_q8,
             uint8_t bg, uint8_t fg, uint8_t over, uint8_t tick)
{
    if (fill_q8 < 0) fill_q8 = 0;
    if (fill_q8 > 256) fill_q8 = 256;
    int fw = w * fill_q8 >> 8;
    int mw = mark_q8 > 0 ? (w * mark_q8 >> 8) : w;
    if (mw > w) mw = w;
    int r = h / 2;
    gfx_round(c, x, y, w, h, r, bg);
    if (fw > 0)
    {
        if (fw <= mw) gfx_round(c, x, y, fw < h ? h : fw, h, r, fg);
        else { gfx_round(c, x, y, fw, h, r, over); gfx_round(c, x, y, mw, h, r, fg); }
    }
    if (tick && mark_q8 > 0 && mw < w) gfx_fill(c, x + mw, y - 3, 2, h + 6, tick);
}

void gfx_dither(struct canvas * c, int x, int y, int w, int h, uint8_t color, int level)
{
    if (level <= 0) return;
    if (level >= 4) { gfx_fill(c, x, y, w, h, color); return; }
    /* 2x2 Bayer order: 0 3 / 2 1 -> pixel (px,py) lit if order < level */
    static const uint8_t order[2][2] = { { 0, 3 }, { 2, 1 } };
    int y0 = y < c->cy0 ? c->cy0 : y, y1 = y + h > c->cy1 ? c->cy1 : y + h;
    int x0 = x < c->cx0 ? c->cx0 : x, x1 = x + w > c->cx1 ? c->cx1 : x + w;
    for (int yy = y0; yy < y1; yy++)
    {
        uint8_t * row = c->px + yy * c->pitch;
        for (int xx = x0; xx < x1; xx++)
            if (order[yy & 1][xx & 1] < level) row[xx] = color;
    }
}

void gfx_dot(struct canvas * c, int cx, int cy, int r, uint8_t color)
{
    gfx_round(c, cx - r, cy - r, 2 * r, 2 * r, r, color);
}

void gfx_tri(struct canvas * c, int cx, int cy, int size, int dir, uint8_t color)
{
    for (int i = 0; i < size; i++)
    {
        int half = i * 2 / 2;   /* width grows by 2 px per row: a 90-degree tip */
        switch (dir)
        {
            case 0: span(c, cx - half, cx + half + 1, cy - size / 2 + i, color); break;          /* up */
            case 1: span(c, cx - half, cx + half + 1, cy + size / 2 - i, color); break;          /* down */
            case 2: gfx_fill(c, cx - size / 2 + i, cy - half, 1, 2 * half + 1, color); break;   /* left */
            default: gfx_fill(c, cx + size / 2 - i, cy - half, 1, 2 * half + 1, color); break;  /* right */
        }
    }
}

void gfx_icon(struct canvas * c, int x, int y, int w, int h, const uint8_t * px, const uint8_t colors[4])
{
    for (int yy = 0; yy < h; yy++)
    {
        int sy = y + yy;
        if (sy < c->cy0 || sy >= c->cy1) continue;
        uint8_t * row = c->px + sy * c->pitch;
        for (int xx = 0; xx < w; xx++)
        {
            int sx = x + xx, v = px[yy * w + xx];
            if (v && sx >= c->cx0 && sx < c->cx1) row[sx] = colors[v];
        }
    }
}
