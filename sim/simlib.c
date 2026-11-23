#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "simlib.h"

uint8_t front[UI_W * UI_H];
uint8_t lv[UI_W * UI_H * 3];

/* ---- fonts, read exactly as ML's rbf_font.c does */
struct ui_rbf * load_rbf(const char * path)
{
    FILE * f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    struct ui_rbf * r = calloc(1, sizeof(*r));
    if (fread(r, 1, 116, f) != 116) { fprintf(stderr, "bad font %s\n", path); exit(1); }
    r->width = 8 * r->charSize / r->height;
    r->charCount = r->charLast - r->charFirst + 1;
    memset(r->wTable, r->width, 256);
    fseek(f, r->_wmapAddr, SEEK_SET);
    if (fread(&r->wTable[r->charFirst], 1, r->charCount, f) != (size_t)r->charCount) exit(1);
    r->cTable = malloc(r->charCount * r->charSize);
    fseek(f, r->_cmapAddr, SEEK_SET);
    if (fread(r->cTable, 1, r->charCount * r->charSize, f) != (size_t)(r->charCount * r->charSize)) exit(1);
    fclose(f);
    return r;
}

/* ---- ML palette (DIGIC 5), close enough for previews */
void pal(uint8_t i, int * r, int * g, int * b)
{
    if (i >= 38 && i <= 79) { int v = (i - 38) * 255 / 41; *r = *g = *b = v; return; }
    switch (i)
    {
        case 0x01: *r = 240; *g = 240; *b = 240; return;
        case 0x02: *r = 0;   *g = 0;   *b = 0;   return;
        case 0x05: *r = 40;  *g = 200; *b = 220; return;
        case 0x06: *r = 70;  *g = 180; *b = 70;  return;
        case 0x07: *r = 60;  *g = 205; *b = 95;  return;
        case 0x09: *r = 90;  *g = 165; *b = 250; return;
        case 0x0E: *r = 220; *g = 60;  *b = 200; return;
        case 0x08: *r = 235; *g = 60;  *b = 55;  return;
        case 0x0B: *r = 40;  *g = 80;  *b = 230; return;
        case 0x0C: *r = 140; *g = 20;  *b = 20;  return;
        case 0x0F: *r = 240; *g = 210; *b = 50;  return;
        case 0x13: *r = 245; *g = 140; *b = 30;  return;
        default:   *r = 255; *g = 0;   *b = 255; return;     /* unknown index: magenta, stands out */
    }
}

void make_lv(void)
{
    /* a dusk street: sky gradient, a sun, dark buildings */
    for (int y = 0; y < UI_H; y++)
        for (int x = 0; x < UI_W; x++)
        {
            uint8_t * p = &lv[(y * UI_W + x) * 3];
            int t = y * 255 / UI_H;
            int r = 250 - t / 2, g = 150 - t / 3, b = 110 + t / 4;
            int dx = x - 520, dy = y - 300;
            if (dx * dx + dy * dy < 55 * 55) { r = 255; g = 220; b = 150; }
            int roof = 330 + ((x / 90) % 3) * 30 - ((x / 37) % 2) * 12;
            if (y > roof) { r = 25 + (x % 90 < 6) * 10; g = 22; b = 30; if ((x / 18 + y / 22) % 7 == 0 && y < 450) { r = 230; g = 180; b = 90; } }
            p[0] = r < 0 ? 0 : r > 255 ? 255 : r;
            p[1] = g < 0 ? 0 : g > 255 ? 255 : g;
            p[2] = b < 0 ? 0 : b > 255 ? 255 : b;
        }
}

void write_frame(const char * dir, int n)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/frame%04d.ppm", dir, n);
    FILE * f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", UI_W, UI_H);
    for (int i = 0; i < UI_W * UI_H; i++)
    {
        uint8_t c = front[i];
        int r = lv[i * 3], g = lv[i * 3 + 1], b = lv[i * 3 + 2];
        if (c == 0x03) { r /= 2; g /= 2; b /= 2; }
        else if (c != 0x00) pal(c, &r, &g, &b);
        fputc(r, f); fputc(g, f); fputc(b, f);
    }
    fclose(f);
}


void load_fonts(const char * dir)
{
    static const char * names[UF_COUNT] = { "term12", "term20", "argnor23", "argnor28", "argnor32" };
    for (int i = 0; i < UF_COUNT; i++)
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s.rbf", dir, names[i]);
        ui_text_set_font(i, load_rbf(path));
    }
}
