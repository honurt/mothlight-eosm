/*
 * The Mothlight profile panel: an animated overlay over LiveView for
 * choosing and applying a recording profile.
 *
 * Pure UI logic: it draws into a back canvas and says which rows of it to
 * show (struct present); the camera glue (moth_ui.c) copies those rows to
 * the screen, and the PC simulator (sim/) writes them to PNGs.
 *
 * Cost model, so it stays cheap on a 400 MHz ARM9 that is also recording:
 *   - opening renders the panel once, then only copies rows (unfold);
 *   - after that only what moves is redrawn: the list while the highlight
 *     glides, a bar while it fills, the footer while the spinner turns;
 *   - when nothing moves the panel draws nothing at all.
 */
#ifndef MOTH_PANEL_H
#define MOTH_PANEL_H

#include "ui_gfx.h"
#include "ui_anim.h"
#include "profiles.h"
#include "themes.h"

enum panel_key   { PK_NONE, PK_UP, PK_DOWN, PK_LEFT, PK_RIGHT, PK_SET, PK_CLOSE,
                   PK_WHEEL_PREV, PK_WHEEL_NEXT };   /* the rear dial (the panel treats it like UP / DOWN) */
enum panel_act   { PA_NONE, PA_APPLY, PA_CLOSED_BY_USER, PA_THEME_PREV, PA_THEME_NEXT };
enum panel_state { PS_CLOSED, PS_OPENING, PS_OPEN, PS_APPLYING, PS_DONE, PS_CLOSING };

/* the frame result, bit flags */
#define PF_PRESENTED  1     /* something was copied/cleared on screen */
#define PF_ANIMATING  2     /* call again soon (~30 ms) */
#define PF_CLOSED     4     /* the close animation just ended: give the screen back */

/* What the panel shows. Filled by the glue; read-only for the panel. */
struct panel_view
{
    const struct rec_profile * profiles;
    int count;
    int active;                 /* profile last applied, -1 = none/unknown */
    const struct bpp_table * bpp;
    int bpp_learned_mask;       /* bit (bits-10) set = learned from your clips */
    int card_kBps;              /* write speed used for the verdicts */
    int card_auto;              /* card_kBps came from the SD clock setting, not measured */
    int sd_clock;               /* Debug > SD Overclock: 0 OFF, 1..3 on, -1 sd_uhs not loaded */
    int free_MB;                /* -1 unknown */
    int battery;                /* 0..100, -1 unknown */
    const char * engine;        /* "Crop Mood" */
    const char * version;       /* "0.1.0" */
    int reduced_motion;         /* 1: no animations (instant) */
    const struct ui_theme * theme;  /* NULL = Mothlight */
    int focal_mm;               /* lens focal length, 0 = unknown (no electronic lens) */
};

/* rows on the screen to copy from back to front, or to clear */
struct present_op { short y0, y1; char clear; };
struct present
{
    short x0, x1;               /* horizontal range of every op */
    int n;
    struct present_op op[6];
};

struct panel
{
    enum panel_state state;
    int sel;                    /* selected profile */
    int scroll_px;              /* list scroll target, pixels */
    struct tween hl;            /* highlight y (list coordinates) */
    struct tween scroll;        /* list scroll */
    struct tween bar;           /* gauge fill, q8 of 2x the card speed */
    struct tween slide;         /* detail pane slide-in, pixels */
    struct tween crop_w, crop_h;/* sensor diagram crop rectangle, pixels */
    struct tween reveal;        /* half-height of the visible band (open/close) */
    uint32_t t_state;           /* when the state last changed */
    uint32_t t_open;            /* for the row bars' cascade */
    uint32_t spin_tick;
    int prev_y0, prev_y1;       /* visible band shown last frame */
    int full_redraw;
    int footer_dirty, list_dirty, header_dirty, detail_dirty;
    char toast[80];
    int toast_ok;
    int applying;               /* index being applied */
};

void panel_open(struct panel * p, const struct panel_view * v, uint32_t now);
void panel_close(struct panel * p, const struct panel_view * v, uint32_t now);
int  panel_key(struct panel * p, const struct panel_view * v, int key, uint32_t now);
void panel_apply_result(struct panel * p, int ok, const char * msg, uint32_t now);
void panel_data_changed(struct panel * p);  /* card/battery/learned numbers changed: redraw text */
int  panel_frame(struct panel * p, const struct panel_view * v, struct canvas * back,
                 uint32_t now, struct present * out);

static inline int panel_is_open(const struct panel * p) { return p->state != PS_CLOSED; }

/* fully on screen: the iris has finished opening and is not closing. Only then does the
   back buffer equal what must be on the screen (used to repair foreign redraws) */
static inline int panel_settled(const struct panel * p)
{
    return (p->state == PS_OPEN || p->state == PS_APPLYING || p->state == PS_DONE)
           && p->prev_y0 <= 0 && p->prev_y1 >= 480;
}

void panel_theme_changed(struct panel * p);  /* redraw everything in the new look */

/* geometry: the panel is the whole 720 x 480 screen of the EOS M */
#define PANEL_X    0
#define PANEL_Y    0
#define PANEL_W  720
#define PANEL_H  480
#define PANEL_SHADOW 0

#endif
