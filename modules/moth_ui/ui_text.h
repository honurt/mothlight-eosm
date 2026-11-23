/*
 * Text with a transparent background, drawn from Magic Lantern's own RBF fonts.
 *
 * ML's bmp_printf always paints a background box behind each glyph, which
 * breaks as soon as text sits on a highlight that is moving. This draws
 * only the glyph pixels, straight into a canvas. On the camera the fonts are
 * the ones ML already loaded (font_dynamic[]); on a PC they are read from the
 * same .rbf files, so previews use the real pixels.
 */
#ifndef EOSM_UI_TEXT_H
#define EOSM_UI_TEXT_H

#include "ui_gfx.h"

/* ML's dynamic font slots (loaded in this order by ML at boot) */
enum ui_font { UF_SMALL = 0, UF_MONO = 1, UF_MED = 2, UF_MEDL = 3, UF_LARGE = 4, UF_COUNT = 5 };
enum ui_align { UA_LEFT = 0, UA_CENTER = 1, UA_RIGHT = 2 };

/* layout-compatible with ML's `font` (src/rbf_font.h) */
struct ui_rbf
{
    int magic1, magic2;
    char name[64];
    int charSize, points, height, maxWidth, charFirst, charLast;
    int _unknown4, _wmapAddr, _cmapAddr, descent, intline;
    int charCount;
    int width;
    char wTable[256];
    char * cTable;
    int cTableSize, cTableSizeMax;
};

void ui_text_set_font(int id, const struct ui_rbf * f);
int  ui_text_h(int font);
int  ui_text_w(int font, const char * s);
/* draws s with its top-left (UA_LEFT), top-centre or top-right at (x, y); returns the width */
int  ui_text(struct canvas * c, int font, int x, int y, uint8_t fg, int align, const char * s);
/* the same, shortened with "..." to fit max_w pixels */
int  ui_text_fit(struct canvas * c, int font, int x, int y, uint8_t fg, int align, const char * s, int max_w);
#ifdef MOTH_SIM
extern int ui_text_overflows;
extern char ui_text_overflow_last[96];
extern char ui_text_drawn[16384];
void ui_text_drawn_reset(void);
#endif

#endif
