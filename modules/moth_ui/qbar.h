/*
 * Mothlight quick bar: the camera's own exposure settings, in a Canon-style strip at the
 * bottom of LiveView -- because Crop Mood hides Canon's controls in x5 zoom (every RAW
 * profile), and Magic Lantern's way is a menu list.
 *
 *   SHUTTER     APERTURE    ISO        WB         PROFILE
 *   1/50        f/2.8       800        5600K      2.5K Cinema
 *   180°        f/1.4-16    native     Kelvin     OK  ~20 min
 *
 * On the EOS M in LiveView Canon keeps the rear dial (shutter) and LEFT / RIGHT for itself,
 * so: SET moves to the next setting, UP / DOWN change it, the dial is Canon's shutter (the bar
 * follows it), UP / DOWN on PROFILE open the profile list, INFO closes. A tap on a tile
 * selects it (and on PROFILE opens the list) when the touch position can be read. Like panel.c this is pure drawing and state: the camera side
 * (moth_ui.c) reads and sets the values; the PC simulators run the same code.
 */
#ifndef MOTH_QBAR_H
#define MOTH_QBAR_H

#include <stdint.h>
#include "ui_gfx.h"
#include "ui_anim.h"
#include "themes.h"
#include "panel.h"          /* PK_* keys, PF_* flags, struct present */

enum qb_item { QB_SHUTTER, QB_APERTURE, QB_ISO, QB_WB, QB_FOCUS, QB_PROFILE, QB_MENU, QB_COUNT };
enum qb_tone { QT_DIM, QT_OK, QT_EDGE, QT_BURST, QT_ACCENT };

struct qb_item_view
{
    char value[24];
    char sub[28];
    uint8_t sub_tone;       /* enum qb_tone */
    uint8_t locked;         /* cannot be changed now (auto exposure, manual lens): shown dim */
};

struct qbar_view
{
    const struct ui_theme * theme;
    int reduced_motion;
    struct qb_item_view item[QB_COUNT];
    int battery_bars;       /* 0 low, 1 half, 2+ full; -1 unknown */
};

/* tile geometry: widths follow what each one shows (the profile name needs room) */
extern const short qb_tile_x[QB_COUNT + 1];

enum qbar_state { QB_CLOSED, QB_OPENING, QB_OPEN, QB_CLOSING };

/* what a key asks the camera side to do */
enum qbar_act { QA_NONE, QA_ADJUST, QA_OPEN_PANEL, QA_CLOSE, QA_LOCKED, QA_OPEN_SETTINGS };

struct qbar
{
    enum qbar_state state;
    int sel;
    int adj_item, adj_sign;         /* for QA_ADJUST / QA_LOCKED */
    struct tween reveal;            /* rows of the bar on screen, sliding up from the bottom */
    struct tween hl, hl_w;          /* highlight x and width */
    uint32_t t_open, hint_until, toast_until;
    char toast[64];
    int toast_bad;
    int prev_top;                   /* topmost row shown last frame (SH = nothing) */
    int strip_shown;                /* the hint / toast strip above the bar is on screen */
    int full_redraw, dirty;
};

void qbar_open(struct qbar * q, const struct qbar_view * v, uint32_t now);
void qbar_close(struct qbar * q, const struct qbar_view * v, uint32_t now);
int  qbar_key(struct qbar * q, const struct qbar_view * v, int key, uint32_t now);
/* a tap at screen (x, y): selects the tile under it; a tap on the selected tile activates it */
int  qbar_tap(struct qbar * q, const struct qbar_view * v, int x, int y, uint32_t now);
void qbar_toast(struct qbar * q, const char * msg, int bad, uint32_t now);
void qbar_changed(struct qbar * q);         /* values changed: redraw the tiles */
int  qbar_frame(struct qbar * q, const struct qbar_view * v, struct canvas * back,
                uint32_t now, struct present * out);

static inline int qbar_settled(const struct qbar * q) { return q->state == QB_OPEN; }

/* geometry: a 72 px bar at the bottom, a 30 px strip above it for hints and messages */
#define QB_H        72
#define QB_Y        (480 - QB_H)
#define QB_STRIP_H  30
#define QB_STRIP_Y  (QB_Y - QB_STRIP_H - 6)
#define QB_TOP      QB_STRIP_Y          /* everything the bar owns is below this row */

#endif
