/*
 * Themes: every colour and shape the Mothlight UI draws comes from here,
 * so a new look is one table entry (and a PROFILES-style user file later).
 * Colours are Magic Lantern palette indices (see ui_gfx.h).
 */
#ifndef MOTH_THEMES_H
#define MOTH_THEMES_H

#include <stdint.h>

enum hl_style   { HL_FILL_PILL, HL_FRAME, HL_FILL, HL_UNDERLINE };
enum head_style { HEAD_PLAIN, HEAD_TABS, HEAD_BAR };

struct ui_theme
{
    const char * name;
    const char * blurb;
    /* surfaces */
    uint8_t bg, line, hl, track, shadow;
    uint8_t bar, pane;      /* status/hint bars, detail pane */
    /* text */
    uint8_t text, dim, faint, hl_text, title;
    /* accents */
    uint8_t accent, ok, edge, burst, active;
    /* shape */
    uint8_t radius, hl_radius;
    uint8_t hl_style, head_style;
    uint8_t frame;          /* outline colour, 0 = none */
    uint8_t brand;          /* colour of the MOTHLIGHT word */
};

enum { THEME_MOTHLIGHT, THEME_STOCK, THEME_STUDIO, THEME_AMBER, THEME_SIGNAL, THEME_MATERIAL, THEME_SOFT,
       THEME_GLASS, THEME_DAYLIGHT, THEME_NIGHT, THEME_MONO, THEME_COUNT };

extern const struct ui_theme moth_themes[THEME_COUNT];

#endif
