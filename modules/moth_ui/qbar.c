/*
 * Mothlight quick bar -- see qbar.h.
 *
 * Draws into rows QB_TOP..479 of the overlay's back buffer and reports which rows to copy
 * (or clear) on the screen; everything above stays transparent, so LiveView shows.
 *
 * Copyright (C) 2026 Onur and contributors. GPL-2.0 (see LICENSE).
 */
#include "qbar.h"
#include "ui_text.h"

#ifdef MODULE
    #include <dryos.h>
#else
    #include <stdio.h>
    #include <string.h>
#endif

#define SW       720
#define SH       480
#define TILE_W   (SW / QB_COUNT)

#define TH       (v->theme ? v->theme : &moth_themes[THEME_MOTHLIGHT])
#define C_LINE   (TH->line)
#define C_HL     (TH->hl)
#define C_TEXT   (TH->text)
#define C_DIM    (TH->dim)
#define C_FAINT  (TH->faint)
#define C_ACCENT (TH->accent)

#define T_OPEN   150
#define T_CLOSE  110
#define T_HL     140
#define T_HINT   3500
#define T_TOAST  2600

static const char * labels[QB_COUNT] = { "SHUTTER", "APERTURE", "ISO", "WB", "PROFILE" };

static int dur(const struct qbar_view * v, int ms) { return v->reduced_motion ? 0 : ms; }

/* bars and text on the bar: themes with a coloured bar use white on it, like the panel */
static uint8_t on_bar(const struct qbar_view * v, uint8_t c)
{
    return TH->head_style == HEAD_BAR ? UC_WHITE : c;
}

static uint8_t tone(const struct qbar_view * v, int t)
{
    switch (t)
    {
        case QT_OK:     return TH->ok;
        case QT_EDGE:   return TH->edge;
        case QT_BURST:  return TH->burst;
        case QT_ACCENT: return C_ACCENT;
        default:        return on_bar(v, C_DIM);
    }
}

/* ---------------------------------------------------------------- state */

void qbar_open(struct qbar * q, const struct qbar_view * v, uint32_t now)
{
    int sel = (q->sel >= 0 && q->sel < QB_COUNT) ? q->sel : 0;   /* remembers the last setting */
    memset(q, 0, sizeof(*q));
    q->sel = sel;
    q->state = QB_OPENING;
    q->t_open = now;
    q->hint_until = now + T_HINT;
    q->prev_top = SH;
    q->full_redraw = 1;
    tween_start(&q->reveal, 0, SH - QB_Y, now, dur(v, T_OPEN), 0);
    tween_start(&q->hl, sel * TILE_W, sel * TILE_W, now, 0, 0);
}

void qbar_close(struct qbar * q, const struct qbar_view * v, uint32_t now)
{
    if (q->state == QB_CLOSED || q->state == QB_CLOSING) return;
    q->state = QB_CLOSING;
    tween_start(&q->reveal, tween_value(&q->reveal, now), 0, now, dur(v, T_CLOSE), 1);
}

void qbar_toast(struct qbar * q, const char * msg, int bad, uint32_t now)
{
    snprintf(q->toast, sizeof(q->toast), "%s", msg);
    q->toast_bad = bad;
    q->toast_until = now + T_TOAST;
    q->hint_until = 0;
    q->dirty = 1;
}

void qbar_changed(struct qbar * q) { q->dirty = 1; }

int qbar_key(struct qbar * q, const struct qbar_view * v, int key, uint32_t now)
{
    if (q->state != QB_OPEN && q->state != QB_OPENING) return QA_NONE;
    switch (key)
    {
        case PK_UP: case PK_DOWN:
        {
            int sel = (q->sel + (key == PK_UP ? QB_COUNT - 1 : 1)) % QB_COUNT;
            tween_retarget(&q->hl, tween_value(&q->hl, now), sel * TILE_W, now, dur(v, T_HL));
            q->sel = sel;
            q->dirty = 1;
            return QA_NONE;
        }
        case PK_WHEEL_PREV: case PK_WHEEL_NEXT:
            q->adj_item = q->sel;
            q->adj_sign = key == PK_WHEEL_NEXT ? 1 : -1;
            if (q->sel == QB_PROFILE) return QA_OPEN_PANEL;     /* the dial on PROFILE opens the list too */
            return v->item[q->sel].locked ? QA_LOCKED : QA_ADJUST;
        case PK_SET:
            if (q->sel == QB_PROFILE) return QA_OPEN_PANEL;
            q->adj_item = q->sel;
            return v->item[q->sel].locked ? QA_LOCKED : QA_NONE;
        case PK_CLOSE:
            qbar_close(q, v, now);
            return QA_CLOSE;
    }
    return QA_NONE;
}

/* ---------------------------------------------------------------- drawing */

static void render_bar(struct qbar * q, const struct qbar_view * v, struct canvas * c, uint32_t now)
{
    gfx_clip(c, 0, QB_Y, SW, SH);
    gfx_fill(c, 0, QB_Y, SW, QB_H, TH->bar);
    gfx_fill(c, 0, QB_Y, SW, 1, C_LINE);
    gfx_fill(c, 0, QB_Y + 1, SW, 2, C_ACCENT);

    int hx = tween_value(&q->hl, now);
    gfx_round(c, hx + 6, QB_Y + 8, TILE_W - 12, QB_H - 14, TH->hl_radius, C_HL);
    gfx_fill(c, hx + 22, SH - 8, TILE_W - 44, 3, C_ACCENT);

    for (int i = 0; i < QB_COUNT; i++)
    {
        int x = i * TILE_W;
        if (i) gfx_fill(c, x, QB_Y + 16, 1, QB_H - 30, C_LINE);
        int on_hl = hx > x - TILE_W / 2 && hx < x + TILE_W / 2;
        const struct qb_item_view * it = &v->item[i];
        uint8_t label_col = on_hl ? C_ACCENT : on_bar(v, C_DIM);
        uint8_t value_col = it->locked ? C_FAINT : on_hl ? TH->hl_text : on_bar(v, C_TEXT);
        int tx = x + 16, tw = TILE_W - 26;
        ui_text_fit(c, UF_SMALL, tx, QB_Y + 9, label_col, UA_LEFT, labels[i], tw);
        ui_text_fit(c, UF_MED, tx, QB_Y + 22, value_col, UA_LEFT, it->value, tw);
        ui_text_fit(c, UF_SMALL, tx, QB_Y + 50, it->locked ? C_FAINT : tone(v, it->sub_tone), UA_LEFT, it->sub, tw);
    }
    gfx_clip_all(c);
}

/* the strip above the bar: the controls for a few seconds after opening, then messages */
static const char * strip_text(const struct qbar * q, uint32_t now, int * bad)
{
    *bad = 0;
    if (q->toast[0] && (int32_t)(q->toast_until - now) > 0) { *bad = q->toast_bad; return q->toast; }
    if ((int32_t)(q->hint_until - now) > 0) return "UP / DOWN choose     DIAL change     INFO close";
    return 0;
}

static void render_strip(const struct qbar_view * v, struct canvas * c, const char * text, int bad)
{
    gfx_clip(c, 0, QB_STRIP_Y, SW, QB_STRIP_Y + QB_STRIP_H);
    gfx_fill(c, 0, QB_STRIP_Y, SW, QB_STRIP_H, 0);            /* transparent around the pill */
    int w = ui_text_w(UF_SMALL, text) + 28;
    if (w > SW - 20) w = SW - 20;
    int x = (SW - w) / 2;
    gfx_round(c, x, QB_STRIP_Y, w, QB_STRIP_H, QB_STRIP_H / 2, TH->bar);
    gfx_round_frame(c, x, QB_STRIP_Y, w, QB_STRIP_H, QB_STRIP_H / 2, bad ? TH->burst : C_LINE);
    ui_text_fit(c, UF_SMALL, x + 14, QB_STRIP_Y + 8, bad ? TH->burst : on_bar(v, C_TEXT), UA_LEFT, text, w - 28);
    gfx_clip_all(c);
}

static void add_op(struct present * out, int y0, int y1, int clear)
{
    if (y1 <= y0) return;
    /* merge with the previous op when they touch and are of the same kind */
    if (out->n && out->op[out->n - 1].clear == clear && y0 <= out->op[out->n - 1].y1)
    {
        if (y1 > out->op[out->n - 1].y1) out->op[out->n - 1].y1 = (short)y1;
        return;
    }
    if (out->n >= (int)(sizeof(out->op) / sizeof(out->op[0]))) return;
    out->op[out->n].y0 = (short)y0;
    out->op[out->n].y1 = (short)y1;
    out->op[out->n].clear = (char)clear;
    out->n++;
}

int qbar_frame(struct qbar * q, const struct qbar_view * v, struct canvas * c, uint32_t now, struct present * out)
{
    out->n = 0;
    out->x0 = 0;
    out->x1 = SW;
    if (q->state == QB_CLOSED) return 0;

    int top = SH - tween_value(&q->reveal, now);

    if (q->state == QB_CLOSING)
    {
        if (q->strip_shown) { add_op(out, QB_STRIP_Y, QB_STRIP_Y + QB_STRIP_H, 1); q->strip_shown = 0; }
        if (top > q->prev_top) add_op(out, q->prev_top, top, 1);
        q->prev_top = top;
        if (tween_done(&q->reveal, now))
        {
            add_op(out, top, SH, 1);
            q->state = QB_CLOSED;
            return PF_PRESENTED | PF_CLOSED;
        }
        return PF_PRESENTED | PF_ANIMATING;
    }

    int bar_dirty = 0;
    if (q->full_redraw || q->dirty || !tween_done(&q->hl, now))
    {
        render_bar(q, v, c, now);
        bar_dirty = 1;
    }

    /* the strip: only once the bar is up */
    int bad;
    const char * text = q->state == QB_OPEN ? strip_text(q, now, &bad) : 0;
    if (text && (q->full_redraw || q->dirty || !q->strip_shown))
    {
        render_strip(v, c, text, bad);
        add_op(out, QB_STRIP_Y, QB_STRIP_Y + QB_STRIP_H, 0);
        q->strip_shown = 1;
    }
    else if (!text && q->strip_shown)
    {
        gfx_clip(c, 0, QB_STRIP_Y, SW, QB_STRIP_Y + QB_STRIP_H);
        gfx_fill(c, 0, QB_STRIP_Y, SW, QB_STRIP_H, 0);
        gfx_clip_all(c);
        add_op(out, QB_STRIP_Y, QB_STRIP_Y + QB_STRIP_H, 1);
        q->strip_shown = 0;
    }
    q->full_redraw = q->dirty = 0;

    /* rows of the bar to copy: newly revealed, or everything visible if it was redrawn */
    if (bar_dirty) add_op(out, top, SH, 0);
    else if (top < q->prev_top) add_op(out, top, q->prev_top, 0);
    q->prev_top = top;

    if (q->state == QB_OPENING && tween_done(&q->reveal, now)) q->state = QB_OPEN;

    int flags = out->n ? PF_PRESENTED : 0;
    if (q->state == QB_OPENING || !tween_done(&q->hl, now) || q->strip_shown)
        flags |= PF_ANIMATING;      /* the strip expires on its own: keep frames coming */
    return flags;
}
