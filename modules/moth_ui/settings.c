/*
 * Mothlight settings page -- see settings.h.
 *
 * Copyright (C) 2026 Onur and contributors. GPL-2.0 (see LICENSE).
 */
#include "settings.h"
#include "ui_text.h"
#include "logo_icon.h"

#ifdef MODULE
    #include <dryos.h>
#else
    #include <stdio.h>
    #include <string.h>
#endif

#define SW       720
#define SH       480
#define BAR_H     52
#define HINT_H    42
#define HINT_Y   (SH - HINT_H)
#define HELP_H    46
#define HELP_Y   (HINT_Y - HELP_H)
#define LIST_Y   BAR_H
#define LIST_H   (HELP_Y - LIST_Y)
#define ROW_H     40
#define SEC_H     30
#define PADX      24

#define TH       (v->theme ? v->theme : &moth_themes[THEME_MOTHLIGHT])
#define C_BG     (TH->bg)
#define C_LINE   (TH->line)
#define C_HL     (TH->hl)
#define C_TEXT   (TH->text)
#define C_DIM    (TH->dim)
#define C_FAINT  (TH->faint)
#define C_ACCENT (TH->accent)

#define T_HL     150
#define T_SCROLL 170
#define T_TOAST  2600

static int dur(const struct settings_view * v, int ms) { return v->reduced_motion ? 0 : ms; }
static uint8_t bar_text(const struct settings_view * v) { return TH->head_style == HEAD_BAR ? UC_WHITE : C_TEXT; }
static uint8_t bar_dim(const struct settings_view * v)  { return TH->head_style == HEAD_BAR ? UC_WHITE : C_DIM; }

static uint8_t tone(const struct settings_view * v, int t)
{
    switch (t)
    {
        case ST_DIM:    return C_DIM;
        case ST_OK:     return TH->ok;
        case ST_EDGE:   return TH->edge;
        case ST_BAD:    return TH->burst;
        case ST_ACCENT: return C_ACCENT;
        default:        return C_TEXT;
    }
}

/* ---------------------------------------------------------------- geometry */

static int row_h(const struct set_row * r) { return r->kind == SR_SECTION ? SEC_H : ROW_H; }

static int row_y(const struct settings_view * v, int i)     /* list coordinates */
{
    int y = 0;
    for (int k = 0; k < i && k < v->count; k++) y += row_h(&v->row[k]);
    return y;
}

static int list_total(const struct settings_view * v) { return row_y(v, v->count); }

static int selectable(const struct settings_view * v, int i)
{
    return i >= 0 && i < v->count && v->row[i].kind == SR_ITEM;
}

static int next_item(const struct settings_view * v, int from, int dir)
{
    for (int k = 1; k <= v->count; k++)
    {
        int i = ((from + dir * k) % v->count + v->count) % v->count;
        if (selectable(v, i)) return i;
    }
    return -1;
}

static void follow(struct settings * s, const struct settings_view * v, uint32_t now, int animate)
{
    int top = row_y(v, s->sel), h = row_h(&v->row[s->sel]);
    /* keep a section header above the selection visible when it fits */
    if (s->sel > 0 && v->row[s->sel - 1].kind == SR_SECTION) top -= SEC_H;
    int target = s->scroll_px;
    if (top < target) target = top;
    if (row_y(v, s->sel) + h > target + LIST_H) target = row_y(v, s->sel) + h - LIST_H;
    int max = list_total(v) - LIST_H;
    if (target > max) target = max;
    if (target < 0) target = 0;
    if (target != s->scroll_px)
    {
        tween_retarget(&s->scroll, tween_value(&s->scroll, now), target, now, animate ? dur(v, T_SCROLL) : 0);
        s->scroll_px = target;
    }
}

static void select_row(struct settings * s, const struct settings_view * v, int i, uint32_t now)
{
    if (!selectable(v, i)) return;
    tween_retarget(&s->hl, tween_value(&s->hl, now), row_y(v, i), now, dur(v, T_HL));
    s->sel = i;
    follow(s, v, now, 1);
    s->dirty = 1;
}

/* ---------------------------------------------------------------- state */

void settings_open(struct settings * s, const struct settings_view * v, uint32_t now)
{
    int keep = s->sel;
    memset(s, 0, sizeof(*s));
    s->open = 1;
    s->sel = selectable(v, keep) ? keep : next_item(v, -1, 1);
    if (s->sel < 0) s->sel = 0;
    tween_start(&s->hl, row_y(v, s->sel), row_y(v, s->sel), now, 0, 0);
    tween_start(&s->scroll, 0, 0, now, 0, 0);
    follow(s, v, now, 0);
    s->full_redraw = 1;
}

/* the rows were rebuilt (a warning came or went): stay on the same item, without a jump */
void settings_rebuilt(struct settings * s, const struct settings_view * v, int prev_id, uint32_t now)
{
    int i = -1;
    for (int k = 0; k < v->count; k++)
        if (v->row[k].kind == SR_ITEM && v->row[k].id == prev_id) { i = k; break; }
    if (i < 0) i = selectable(v, s->sel) ? s->sel : next_item(v, -1, 1);
    if (i < 0) i = 0;
    s->sel = i;
    tween_start(&s->hl, row_y(v, i), row_y(v, i), now, 0, 0);
    follow(s, v, now, 0);
    s->full_redraw = 1;
}

void settings_toast(struct settings * s, const char * msg, int bad, uint32_t now)
{
    snprintf(s->toast, sizeof(s->toast), "%s", msg);
    s->toast_bad = bad;
    s->toast_until = now + T_TOAST;
    s->footer_dirty = 1;
}

void settings_changed(struct settings * s) { s->dirty = 1; }

int settings_key(struct settings * s, const struct settings_view * v, int key, uint32_t now)
{
    if (!s->open) return SA_NONE;
    switch (key)
    {
        case PK_UP: case PK_WHEEL_PREV:   select_row(s, v, next_item(v, s->sel, -1), now); return SA_NONE;
        case PK_DOWN: case PK_WHEEL_NEXT: select_row(s, v, next_item(v, s->sel, +1), now); return SA_NONE;
        case PK_SET:
            if (!selectable(v, s->sel)) return SA_NONE;
            s->act_id = v->row[s->sel].id;
            return SA_ACTIVATE;
        case PK_CLOSE:
            s->open = 0;
            return SA_CLOSE;
    }
    return SA_NONE;
}

int settings_tap(struct settings * s, const struct settings_view * v, int x, int y, uint32_t now)
{
    if (!s->open || y < LIST_Y || y >= LIST_Y + LIST_H || x < 0 || x >= SW) return SA_NONE;
    int ly = y - LIST_Y + tween_value(&s->scroll, now);
    for (int i = 0; i < v->count; i++)
    {
        int ry = row_y(v, i);
        if (ly >= ry && ly < ry + row_h(&v->row[i]))
        {
            if (!selectable(v, i)) return SA_NONE;
            if (i != s->sel) { select_row(s, v, i, now); return SA_NONE; }
            s->act_id = v->row[i].id;
            return SA_ACTIVATE;
        }
    }
    return SA_NONE;
}

/* ---------------------------------------------------------------- drawing */

static void battery_icon(struct canvas * c, int x, int y, int bars, uint8_t fg, uint8_t low)
{
    /* 30 x 14 body, 3 cells, a nub on the right */
    uint8_t col = bars == 0 ? low : fg;
    gfx_round_frame(c, x, y, 30, 14, 2, col);
    gfx_fill(c, x + 30, y + 4, 3, 6, col);
    int cells = bars <= 0 ? 1 : bars == 1 ? 2 : 3;
    for (int i = 0; i < cells; i++) gfx_fill(c, x + 3 + i * 9, y + 3, 7, 8, col);
}

static void render_bar(const struct settings_view * v, struct canvas * c)
{
    gfx_clip(c, 0, 0, SW, BAR_H);
    gfx_fill(c, 0, 0, SW, BAR_H, TH->bar);
    const uint8_t icon_colors[4] = { 0, bar_text(v), TH->accent == UC_WHITE ? UC_GRAY(70) : C_ACCENT, bar_dim(v) };
    gfx_icon(c, 14, 10, 32, 32, logo_icon, icon_colors);
    ui_text(c, UF_SMALL, 57, 7, TH->head_style == HEAD_BAR ? UC_WHITE : TH->brand, UA_LEFT, "MOTHLIGHT");
    ui_text(c, UF_MED, 56, 21, TH->head_style == HEAD_BAR ? UC_WHITE : TH->title, UA_LEFT, v->title[0] ? v->title : "Settings");

    char buf[32];
    int x = SW - 16;
    if (v->warn_count)
    {
        snprintf(buf, sizeof(buf), "%d warning%s", v->warn_count, v->warn_count > 1 ? "s" : "");
        x -= ui_text(c, UF_SMALL, x, 20, TH->burst, UA_RIGHT, buf) + 16;
    }
    else x -= ui_text(c, UF_SMALL, x, 20, TH->ok, UA_RIGHT, "all good") + 16;
    if (v->battery_bars >= 0) battery_icon(c, x - 34, 19, v->battery_bars, bar_text(v), TH->burst);

    if (TH->head_style != HEAD_BAR) gfx_fill(c, 0, BAR_H - 2, SW, 2, C_ACCENT);
    gfx_clip_all(c);
}

static void render_list(struct settings * s, const struct settings_view * v, struct canvas * c, uint32_t now)
{
    gfx_clip(c, 0, LIST_Y, SW, LIST_Y + LIST_H);
    gfx_fill(c, 0, LIST_Y, SW, LIST_H, C_BG);
    int scroll = tween_value(&s->scroll, now);
    int hl = tween_value(&s->hl, now);

    /* the highlight glides under the rows */
    int hy = LIST_Y + hl - scroll;
    gfx_round(c, 10, hy + 3, SW - 20, ROW_H - 6, TH->hl_radius, C_HL);
    gfx_round(c, 14, hy + 12, 4, ROW_H - 24, 2, C_ACCENT);

    for (int i = 0; i < v->count; i++)
    {
        const struct set_row * r = &v->row[i];
        int y = LIST_Y + row_y(v, i) - scroll;
        int h = row_h(r);
        if (y + h < LIST_Y || y >= LIST_Y + LIST_H) continue;
        int on_hl = r->kind == SR_ITEM && hy > y - ROW_H / 2 && hy < y + ROW_H / 2;
        switch (r->kind)
        {
            case SR_SECTION:
                ui_text(c, UF_SMALL, PADX, y + 13, C_ACCENT, UA_LEFT, r->label);
                gfx_fill(c, PADX + ui_text_w(UF_SMALL, r->label) + 10, y + 19, SW - 2 * PADX - ui_text_w(UF_SMALL, r->label) - 10, 1, C_LINE);
                break;
            case SR_WARN:
                gfx_dot(c, PADX + 6, y + h / 2, 6, tone(v, r->tone));
                ui_text_fit(c, UF_SMALL, PADX + 20, y + h / 2 - 6, tone(v, r->tone), UA_LEFT, r->label, SW - 2 * PADX - 20);
                break;
            case SR_NOTE:
                ui_text_fit(c, UF_SMALL, PADX + 10, y + h / 2 - 6, C_DIM, UA_LEFT, r->label, SW - 2 * PADX - 10);
                break;
            default:
            {
                uint8_t fg = on_hl ? TH->hl_text : C_TEXT;
                int vw = ui_text_w(UF_MED, r->value);
                if (vw > 250) vw = 250;
                ui_text_fit(c, UF_MED, PADX + 10, y + 8, fg, UA_LEFT, r->label, SW - 2 * PADX - 10 - vw - 24);
                uint8_t vc = r->tone == ST_NORMAL ? (on_hl ? TH->hl_text : C_ACCENT) : tone(v, r->tone);
                ui_text_fit(c, UF_MED, SW - PADX - 10, y + 8, vc, UA_RIGHT, r->value, 250);
                if (!on_hl) gfx_fill(c, PADX, y + h - 1, SW - 2 * PADX, 1, C_LINE);
                break;
            }
        }
    }

    /* scroll bar */
    int total = list_total(v);
    if (total > LIST_H)
    {
        int track = LIST_H - 16, thumb = track * LIST_H / total;
        int ty = LIST_Y + 8 + (track - thumb) * scroll / (total - LIST_H);
        gfx_round(c, SW - 8, LIST_Y + 8, 4, track, 2, C_LINE);
        gfx_round(c, SW - 8, ty, 4, thumb, 2, C_DIM);
    }
    gfx_clip_all(c);
}

/* the help for the selected row, word-wrapped onto two lines */
static void render_help(struct settings * s, const struct settings_view * v, struct canvas * c, uint32_t now)
{
    gfx_clip(c, 0, HELP_Y, SW, HINT_Y);
    gfx_fill(c, 0, HELP_Y, SW, HELP_H, TH->pane);
    gfx_fill(c, 0, HELP_Y, SW, 1, C_LINE);
    const char * help = selectable(v, s->sel) ? v->row[s->sel].help : 0;
    if (help)
    {
        char line[96];
        const char * p = help;
        for (int ln = 0; ln < 2 && *p; ln++)
        {
            int n = 0, last_space = -1;
            while (p[n] && n < (int)sizeof(line) - 1)
            {
                line[n] = p[n]; line[n + 1] = 0;
                if (p[n] == ' ') last_space = n;
                if (ui_text_w(UF_SMALL, line) > SW - 2 * PADX) { if (last_space > 0) n = last_space; break; }
                n++;
            }
            line[n] = 0;
            ui_text(c, UF_SMALL, PADX, HELP_Y + 8 + ln * 17, C_DIM, UA_LEFT, line);
            p += n;
            while (*p == ' ') p++;
        }
    }
    gfx_clip_all(c);
}

static int keycap(const struct settings_view * v, struct canvas * c, int x, int y, const char * label, int arrows)
{
    int w = arrows ? 34 : ui_text_w(UF_SMALL, label) + 14;
    gfx_round_frame(c, x, y, w, 22, 5, bar_dim(v));
    if (arrows) { gfx_tri(c, x + 11, y + 11, 7, 0, bar_text(v)); gfx_tri(c, x + 23, y + 11, 7, 1, bar_text(v)); }
    else ui_text(c, UF_SMALL, x + 7, y + 5, bar_text(v), UA_LEFT, label);
    return w;
}

static void render_hint(struct settings * s, const struct settings_view * v, struct canvas * c, uint32_t now)
{
    gfx_clip(c, 0, HINT_Y, SW, SH);
    gfx_fill(c, 0, HINT_Y, SW, HINT_H, TH->bar);
    gfx_fill(c, 0, HINT_Y, SW, 1, C_LINE);
    int y = HINT_Y + HINT_H / 2 - 11;
    if ((int32_t)(s->toast_until - now) > 0)
    {
        gfx_dot(c, 30, HINT_Y + HINT_H / 2, 7, s->toast_bad ? TH->burst : TH->ok);
        ui_text_fit(c, UF_MED, 48, y, s->toast_bad ? TH->burst : bar_text(v), UA_LEFT, s->toast, SW - 64);
    }
    else
    {
        int x = 16;
        uint8_t fg = bar_text(v);
        x += keycap(v, c, x, y, "SET", 0) + 6;   x += ui_text(c, UF_SMALL, x, y + 5, fg, UA_LEFT, v->set_word ? v->set_word : "Change") + 22;
        x += keycap(v, c, x, y, "", 1) + 6;      x += ui_text(c, UF_SMALL, x, y + 5, fg, UA_LEFT, "Choose") + 22;
        x += keycap(v, c, x, y, "INFO", 0) + 6;  ui_text(c, UF_SMALL, x, y + 5, fg, UA_LEFT, "Back");
        ui_text(c, UF_SMALL, SW - 16, y + 5, bar_dim(v), UA_RIGHT, "Full ML menu: two-finger tap");
    }
    gfx_clip_all(c);
}

int settings_frame(struct settings * s, const struct settings_view * v, struct canvas * c, uint32_t now, struct present * out)
{
    out->n = 0;
    out->x0 = 0;
    out->x1 = SW;
    if (!s->open) return 0;

    int animating = !tween_done(&s->hl, now) || !tween_done(&s->scroll, now);
    int toast_on = (int32_t)(s->toast_until - now) > 0;
    static int toast_was;               /* the toast just expired: put the hints back */
    if (toast_was && !toast_on) s->footer_dirty = 1;
    toast_was = toast_on;

    int y0 = SH, y1 = 0;
#define DIRTY(a, b) do { if ((a) < y0) y0 = (a); if ((b) > y1) y1 = (b); } while (0)
    if (s->full_redraw)
    {
        render_bar(v, c); render_list(s, v, c, now); render_help(s, v, c, now); render_hint(s, v, c, now);
        DIRTY(0, SH);
    }
    else
    {
        if (s->dirty) { render_bar(v, c); DIRTY(0, BAR_H); }
        if (s->dirty || animating) { render_list(s, v, c, now); render_help(s, v, c, now); DIRTY(LIST_Y, HINT_Y); }
        if (s->dirty || s->footer_dirty) { render_hint(s, v, c, now); DIRTY(HINT_Y, SH); }
    }
#undef DIRTY
    s->full_redraw = s->dirty = s->footer_dirty = 0;
    if (y1 > y0)
    {
        out->op[0].y0 = (short)y0; out->op[0].y1 = (short)y1; out->op[0].clear = 0;
        out->n = 1;
    }
    int flags = out->n ? PF_PRESENTED : 0;
    if (animating || toast_on) flags |= PF_ANIMATING;
    return flags;
}
