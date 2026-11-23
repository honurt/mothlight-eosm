/* fonts and frame dumps for camsim; kept apart because simlib's "lv" image buffer
   would collide with ML's "lv" flag in the module under test */
#include <string.h>
#include <stdio.h>
#include "../simlib.h"
struct font { const void * bitmap; };
extern struct font font_dynamic[];

void camsim_load_fonts(const char * dir)
{
    static const char * names[UF_COUNT] = { "term12", "term20", "argnor23", "argnor28", "argnor32" };
    for (int i = 0; i < UF_COUNT; i++)
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s.rbf", dir, names[i]);
        font_dynamic[i].bitmap = load_rbf(path);
    }
    make_lv();
}

void camsim_dump(const uint8_t * bmp960, const char * dir, int n)
{
    for (int y = 0; y < UI_H; y++) memcpy(front + y * UI_W, bmp960 + y * 960, UI_W);
    write_frame(dir, n);
}
