#include <string.h>
#include <stdio.h>
#include "ui_text.h"

static const struct ui_rbf * fonts[UF_COUNT];

void ui_text_set_font(int id, const struct ui_rbf * f)
{
    if (id >= 0 && id < UF_COUNT) fonts[id] = f;
}

static const struct ui_rbf * get(int font)
{
    if (font < 0 || font >= UF_COUNT) font = UF_MED;
    const struct ui_rbf * f = fonts[font];
    return (f && f->cTable && f->height > 0) ? f : 0;
}

int ui_text_h(int font)
{
    const struct ui_rbf * f = get(font);
    return f ? f->height : 0;
}

int ui_text_w(int font, const char * s)
{
    const struct ui_rbf * f = get(font);
    if (!f) return 0;
    int w = 0;
    for (; *s; s++) w += (unsigned char)f->wTable[(unsigned char)*s];
    return w;
}

#ifdef MOTH_SIM
/* simulator builds count text that does not fit its area (the tests fail on it) */
int ui_text_overflows;
char ui_text_overflow_last[96];
#endif

int ui_text(struct canvas * c, int font, int x, int y, uint8_t fg, int align, const char * s)
{
    const struct ui_rbf * f = get(font);
    if (!f) return 0;
    int w = ui_text_w(font, s);
    if (align == UA_CENTER) x -= w / 2;
    else if (align == UA_RIGHT) x -= w;

    /* rows of this string that survive clipping */
    int r0 = c->cy0 - y; if (r0 < 0) r0 = 0;
    int r1 = c->cy1 - y; if (r1 > f->height) r1 = f->height;
#ifdef MOTH_SIM
    if (r1 > r0 && (x < c->cx0 || x + w > c->cx1))
    {
        ui_text_overflows++;
        snprintf(ui_text_overflow_last, sizeof(ui_text_overflow_last), "\"%s\" x %d..%d, area %d..%d", s, x, x + w, c->cx0, c->cx1);
    }
#endif
    if (r1 <= r0 || x >= c->cx1 || x + w <= c->cx0) return w;

    int stride = f->width / 8;          /* bytes per glyph row */
    for (const unsigned char * p = (const unsigned char *)s; *p; p++)
    {
        int ch = *p;
        int cw = (unsigned char)f->wTable[ch];
        if (ch >= f->charFirst && ch <= f->charLast && x + cw > c->cx0 && x < c->cx1)
        {
            const unsigned char * glyph = (const unsigned char *)f->cTable + (ch - f->charFirst) * f->charSize;
            int xa = x < c->cx0 ? c->cx0 - x : 0;
            int xb = x + cw > c->cx1 ? c->cx1 - x : cw;
            for (int r = r0; r < r1; r++)
            {
                const unsigned char * bits = glyph + r * stride;
                uint8_t * dst = c->px + (y + r) * c->pitch + x;
                for (int xx = xa; xx < xb; xx++)
                    if (bits[xx >> 3] & (1 << (xx & 7))) dst[xx] = fg;
            }
        }
        x += cw;
    }
    return w;
}

/* like ui_text, but never wider than max_w: shortens s and ends it with "..." */
int ui_text_fit(struct canvas * c, int font, int x, int y, uint8_t fg, int align, const char * s, int max_w)
{
    if (ui_text_w(font, s) <= max_w) return ui_text(c, font, x, y, fg, align, s);
    char buf[96];
    int n = snprintf(buf, sizeof(buf), "%s", s);
    if (n > (int)sizeof(buf) - 4) n = sizeof(buf) - 4;
    while (n > 0)
    {
        n--;
        while (n > 0 && buf[n - 1] == ' ') n--;     /* no "word ..." */
        memcpy(buf + n, "...", 4);
        if (ui_text_w(font, buf) <= max_w) break;
    }
    return ui_text(c, font, x, y, fg, align, buf);
}
