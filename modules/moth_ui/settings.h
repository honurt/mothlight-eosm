/*
 * Mothlight settings: one full-screen page, in plain language, for the things you actually
 * change on an EOS M -- instead of hunting through Magic Lantern's menu tabs.
 *
 *   MOTHLIGHT  Settings                                   [battery]  2 warnings
 *   ! Movie exposure is Auto: set Manual in Canon's menu
 *   LOOK
 *     Theme                                                 Mothlight
 *     ...
 *   RECORDING
 *     Card speed (SD clock)                                 160 MHz
 *   ...
 *   <help for the selected row, two lines>
 *   [SET] Change   [^v] Choose   [INFO] Back
 *
 * Pure drawing and state, like panel.c and qbar.c: the camera side (moth_ui.c) fills the
 * rows from ML's menus and Mothlight's own settings, and acts when a row is activated.
 */
#ifndef MOTH_SETTINGS_H
#define MOTH_SETTINGS_H

#include <stdint.h>
#include "ui_gfx.h"
#include "ui_anim.h"
#include "themes.h"
#include "panel.h"

enum set_row_kind { SR_SECTION, SR_ITEM, SR_WARN, SR_NOTE };
enum set_tone { ST_NORMAL, ST_DIM, ST_OK, ST_EDGE, ST_BAD, ST_ACCENT };

struct set_row
{
    uint8_t kind;           /* enum set_row_kind */
    uint8_t tone;           /* of the value (or of the warning) */
    uint8_t id;             /* the camera side's item id, for SA_ACTIVATE */
    char label[64];
    char value[28];
    const char * help;      /* static text, may be 0 */
};

#define SET_MAX_ROWS 40

struct settings_view
{
    const struct ui_theme * theme;
    int reduced_motion;
    int count;
    struct set_row row[SET_MAX_ROWS];
    int battery_bars;       /* 0 low, 1 half, 2+ full; -1 unknown */
    int warn_count;
    char title[24];         /* header; empty: "Settings" */
};

enum settings_act { SA_NONE, SA_ACTIVATE, SA_CLOSE };

struct settings
{
    int open;
    int sel;                /* index into row[] of a SR_ITEM */
    int act_id;             /* the activated row's id (SA_ACTIVATE) */
    struct tween hl;        /* highlight y, list coordinates */
    struct tween scroll;
    int scroll_px;
    uint32_t toast_until;
    char toast[72];
    int toast_bad;
    int full_redraw, dirty, footer_dirty;
};

void settings_open(struct settings * s, const struct settings_view * v, uint32_t now);
int  settings_key(struct settings * s, const struct settings_view * v, int key, uint32_t now);
int  settings_tap(struct settings * s, const struct settings_view * v, int x, int y, uint32_t now);
void settings_toast(struct settings * s, const char * msg, int bad, uint32_t now);
void settings_changed(struct settings * s);       /* values changed: redraw */
void settings_rebuilt(struct settings * s, const struct settings_view * v, int prev_id, uint32_t now);
int  settings_frame(struct settings * s, const struct settings_view * v, struct canvas * back,
                    uint32_t now, struct present * out);
static inline int settings_settled(const struct settings * s) { return s->open; }

#endif
