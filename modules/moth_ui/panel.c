#include "panel.h"
#include "ui_text.h"
#include "logo_icon.h"

#ifdef MODULE
    #include <dryos.h>
#else
    #include <stdio.h>
    #include <string.h>
#endif

/*
 * Layout: the whole 720 x 480 screen.
 *
 *   +--------------------------------------------------------------+
 *   | [logo] MOTHLIGHT / Rec profiles          engine, card, SD    |  status bar
 *   +----------------------+---------------------------------------+
 *   | 3K Scope        (o)  | 3K Scope                              |
 *   | 3072x1308 10-bit     | 3072 x 1308                           |
 *   |----------------------| [10-bit] [23.976 fps] [2.35:1] [1:1]  |
 *   | 2.8K Scope      (o)  | +--------+  Sensor area               |  list | detail
 *   | ...                  | | [crop] |  2.72x crop  13.2 mm wide |
 *   |                      | +--------+  50mm frames like ~135mm  |
 *   |                      | 72.2 MB/s needed           BURST      |
 *   |                      | [=================|====]              |
 *   +----------------------+---------------------------------------+
 *   | [SET] Apply  [^v] Choose  [<>] Theme  [MENU] Close   theme   |  hint bar
 *   +--------------------------------------------------------------+
 */
#define SW       720
#define SH       480
#define BAR_H     52
#define HINT_H    42
#define HINT_Y   (SH - HINT_H)
#define LIST_W   300
#define LIST_Y   BAR_H
#define ROW_H     56
#define LIST_H   (HINT_Y - BAR_H)
#define VIS      (LIST_H / ROW_H)
#define DX0      (LIST_W + 1)               /* detail pane left edge */
#define DPAD      24

/* sensor of the EOS M, for the crop diagram */
#define SENSOR_PX_W  5184
#define SENSOR_PX_H  3456
#define SENSOR_MM_W10 223                   /* 22.3 mm */
#define DIAG_W   150
#define DIAG_H   100

/* ---- theme */
#define TH       (v->theme ? v->theme : &moth_themes[THEME_MOTHLIGHT])
#define C_BG     (TH->bg)
#define C_LINE   (TH->line)
#define C_HL     (TH->hl)
#define C_TRACK  (TH->track)
#define C_TEXT   (TH->text)
#define C_DIM    (TH->dim)
#define C_FAINT  (TH->faint)
#define C_ACCENT (TH->accent)

/* ---- timing (ms) */
#define T_OPEN     260
#define T_CLOSE    170
#define T_HL       170
#define T_SCROLL   190
#define T_BAR      340
#define T_SLIDE    200
#define T_CROP     280
#define T_ACCENT   480
#define T_CASCADE  360
#define T_STAGGER   40
#define T_DONE    1100
#define T_SPIN      90

static int dur(const struct panel_view * v, int ms) { return v->reduced_motion ? 0 : ms; }

static int delayed_q8(uint32_t now, uint32_t t0, int d)
{
    if ((int32_t)(now - t0) < 0) return 0;
    return anim_t_q8(now, t0, d);
}

/* bars and text sitting on the bars use the bar's own contrast */
static uint8_t bar_text(const struct panel_view * v)  { return TH->head_style == HEAD_BAR ? UC_WHITE : C_TEXT; }
static uint8_t bar_dim(const struct panel_view * v)   { return TH->head_style == HEAD_BAR ? UC_WHITE : C_DIM; }

static uint8_t verdict_color(const struct panel_view * v, enum budget_verdict vd)
{
    switch (vd)
    {
        case BUDGET_CONTINUOUS: return TH->ok;
        case BUDGET_MARGINAL:   return TH->edge;
        case BUDGET_BURST:      return TH->burst;
        default:                return C_DIM;
    }
}

static const char * verdict_word(enum budget_verdict vd, int big)
{
    switch (vd)
    {
        case BUDGET_CONTINUOUS: return big ? "CONTINUOUS" : "continuous";
        case BUDGET_MARGINAL:   return big ? "ON THE EDGE" : "edge";
        case BUDGET_BURST:      return big ? "BURST" : "burst";
        default:                return "";
    }
}

static int is_h264(const struct rec_profile * pr) { return pr->mood == MOOD_OFF; }

static int need_kBps(const struct panel_view * v, const struct rec_profile * pr)
{
    return is_h264(pr) ? 0 : budget_kBps(&pr->nominal, v->bpp);
}

static int gauge_target(const struct panel_view * v, int sel)
{
    if (sel < 0 || sel >= v->count) return 0;
    int r = budget_ratio_q8(need_kBps(v, &v->profiles[sel]), v->card_kBps) / 2;
    return r > 256 ? 256 : r;
}

/* the part of the sensor a profile reads, in sensor pixels */
static void sensor_area(const struct rec_profile * pr, int * w, int * h)
{
    const struct rec_format * f = &pr->nominal;
    switch (pr->mood)
    {
        case MOOD_1X3: *w = f->width * 3; *h = f->height;     break;   /* bins 3 columns */
        case MOOD_3X3: *w = f->width * 3; *h = f->height * 3; break;   /* bins / skips 3x3 */
        case MOOD_1X1: *w = f->width;     *h = f->height;     break;
        default:       *w = SENSOR_PX_W;  *h = SENSOR_PX_W * 9 / 16; break;   /* Canon 1080p: full width */
    }
    if (*w > SENSOR_PX_W) *w = SENSOR_PX_W;
    if (*h > SENSOR_PX_H) *h = SENSOR_PX_H;
}

static int crop_w_target(const struct panel_view * v, int sel)
{
    int w, h; sensor_area(&v->profiles[sel], &w, &h);
    return w * DIAG_W / SENSOR_PX_W;
}

static int crop_h_target(const struct panel_view * v, int sel)
{
    int w, h; sensor_area(&v->profiles[sel], &w, &h);
    return h * DIAG_H / SENSOR_PX_H;
}

/* a frame `t` pixels thick */
static void frame_thick(struct canvas * c, int x, int y, int w, int h, int r, int t, uint8_t col)
{
    for (int i = 0; i < t; i++)
        gfx_round_frame(c, x + i, y + i, w - 2 * i, h - 2 * i, r > i ? r - i : 0, col);
}

/* a key cap with a label inside: [SET] */
static int keycap(const struct panel_view * v, struct canvas * c, int x, int y, const char * label, int glyph)
{
    int w = glyph >= 0 ? 34 : ui_text_w(UF_SMALL, label) + 14;
    uint8_t fg = bar_text(v);
    gfx_round_frame(c, x, y, w, 22, 5, bar_dim(v));
    if (glyph == 10)        /* up + down */
    {
        gfx_tri(c, x + 11, y + 11, 7, 0, fg);
        gfx_tri(c, x + 23, y + 11, 7, 1, fg);
    }
    else if (glyph == 11)   /* left + right */
    {
        gfx_tri(c, x + 11, y + 11, 7, 2, fg);
        gfx_tri(c, x + 23, y + 11, 7, 3, fg);
    }
    else ui_text(c, UF_SMALL, x + 7, y + 5, fg, UA_LEFT, label);
    return w;
}

/* ---------------------------------------------------------------- status bar */

static void render_bar(struct panel * p, const struct panel_view * v, struct canvas * c, uint32_t now)
{
    gfx_clip(c, 0, 0, SW, BAR_H);
    gfx_fill(c, 0, 0, SW, BAR_H, TH->bar);

    int x = 14;
    if (TH->head_style == HEAD_TABS)
    {
        /* the EOS M menu's look: a red tab for the page we are on */
        gfx_fill(c, 10, 8, 50, 36, UC_RED);
        ui_text(c, UF_SMALL, 35, 20, UC_WHITE, UA_CENTER, "REC");
        x = 70;
    }
    else
    {
        const uint8_t icon_colors[4] = { 0, bar_text(v), TH->accent == UC_WHITE ? UC_GRAY(70) : C_ACCENT, bar_dim(v) };
        gfx_icon(c, x, 10, 32, 32, logo_icon, icon_colors);
        x += 42;
    }
    ui_text(c, UF_SMALL, x + 1, 7, TH->head_style == HEAD_BAR ? UC_WHITE : TH->brand, UA_LEFT, "MOTHLIGHT");
    ui_text(c, UF_MED, x, 21, TH->head_style == HEAD_BAR ? UC_WHITE : TH->title, UA_LEFT, "Rec profiles");

    char buf[80], card[24] = "", bat[24] = "";
    snprintf(buf, sizeof(buf), "%s  v%s", v->engine ? v->engine : "", v->version ? v->version : "");
    ui_text(c, UF_SMALL, SW - 16, 9, bar_dim(v), UA_RIGHT, buf);
    if (v->free_MB >= 0) snprintf(card, sizeof(card), "%d.%d GB", v->free_MB / 1024, v->free_MB % 1024 * 10 / 1024);
    if (v->battery >= 0) snprintf(bat, sizeof(bat), "   BAT %d%%", v->battery);
    snprintf(buf, sizeof(buf), "SD %d MB/s%s   %s%s", v->card_kBps / 1000, v->card_auto ? "*" : "", card, bat);
    ui_text(c, UF_SMALL, SW - 16, 27, bar_text(v), UA_RIGHT, buf);

    /* a thin accent line sweeps across when the panel opens */
    if (TH->head_style == HEAD_TABS) { gfx_fill(c, 0, BAR_H - 2, SW, 2, UC_RED); return; }
    if (TH->head_style == HEAD_BAR) return;
    int e = ease_out_q8(delayed_q8(now, p->t_open + 80, dur(v, T_ACCENT)));
    gfx_fill(c, 0, BAR_H - 2, SW, 2, C_LINE);
    gfx_fill(c, 0, BAR_H - 2, SW * e >> 8, 2, C_ACCENT);
}

static int bar_animating(const struct panel * p, const struct panel_view * v, uint32_t now)
{
    return TH->head_style == HEAD_PLAIN && delayed_q8(now, p->t_open + 80, dur(v, T_ACCENT)) < 256;
}

/* ---------------------------------------------------------------- list */

static int cascade_q8(const struct panel * p, const struct panel_view * v, int vis_index, uint32_t now)
{
    return ease_out_q8(delayed_q8(now, p->t_open + 100 + vis_index * T_STAGGER, dur(v, T_CASCADE)));
}

static void render_list(struct panel * p, const struct panel_view * v, struct canvas * c, uint32_t now)
{
    gfx_clip(c, 0, LIST_Y, LIST_W, LIST_Y + LIST_H);
    gfx_fill(c, 0, LIST_Y, LIST_W, LIST_H, C_BG);

    int scroll = tween_value(&p->scroll, now);
    int hl = tween_value(&p->hl, now);
    int first = scroll / ROW_H;
    int sb = v->count > VIS ? 8 : 0;        /* room for the scroll bar */

    for (int i = first; i < v->count && i <= first + VIS; i++)
    {
        int y = LIST_Y + i * ROW_H - scroll;
        gfx_fill(c, 20, y + ROW_H - 1, LIST_W - 40 - sb, 1, C_LINE);
    }

    int hy = LIST_Y + hl - scroll;
    int hx = 8, hw = LIST_W - 16 - sb;
    switch (TH->hl_style)
    {
        case HL_FRAME:
            gfx_round(c, hx, hy + 2, hw, ROW_H - 4, TH->hl_radius, C_HL);
            frame_thick(c, hx, hy + 2, hw, ROW_H - 4, TH->hl_radius, 3, C_ACCENT);
            break;
        case HL_FILL:
            gfx_round(c, hx, hy + 2, hw, ROW_H - 4, TH->hl_radius, C_HL);
            break;
        default:
            gfx_round(c, hx, hy + 3, hw, ROW_H - 6, TH->hl_radius, C_HL);
            gfx_round(c, hx + 6, hy + 15, 4, ROW_H - 30, 2, C_ACCENT);
            break;
    }

    char buf[48];
    for (int i = first; i < v->count && i <= first + VIS; i++)
    {
        const struct rec_profile * pr = &v->profiles[i];
        int y = LIST_Y + i * ROW_H - scroll;
        int on_hl = hy > y - ROW_H / 2 && hy < y + ROW_H / 2;     /* text on the highlight takes its colour */
        uint8_t name_col = on_hl ? TH->hl_text : C_TEXT;
        uint8_t sum_col  = on_hl ? TH->hl_text : C_DIM;
        int right = LIST_W - 22 - sb;

        if (i == v->active) gfx_dot(c, 25, y + 19, 4, on_hl && TH->hl_style == HL_FILL ? TH->hl_text : TH->active);
        ui_text(c, UF_MED, 36, y + 6, name_col, UA_LEFT, pr->name);
        ui_text(c, UF_SMALL, 37, y + 34, sum_col, UA_LEFT, profile_summary(pr, buf, sizeof(buf)));

        if (is_h264(pr)) continue;
        int need = need_kBps(v, pr);
        enum budget_verdict vd = budget_verdict(need, v->card_kBps);
        uint8_t col = verdict_color(v, vd);
        int fill = budget_ratio_q8(need, v->card_kBps) / 2;
        fill = (fill > 256 ? 256 : fill) * cascade_q8(p, v, i - first, now) >> 8;
        gfx_dot(c, right - 4, y + 18, 5, on_hl && TH->hl_style == HL_FILL ? TH->hl_text : col);
        gfx_bar(c, right - 44, y + 10, 32, 5, fill, 128, C_TRACK, col, TH->burst, 0);
    }

    if (sb)
    {
        int track_h = LIST_H - 16;
        int thumb_h = track_h * VIS / v->count;
        int max_scroll = (v->count - VIS) * ROW_H;
        int ty = LIST_Y + 8 + (track_h - thumb_h) * scroll / (max_scroll ? max_scroll : 1);
        gfx_round(c, LIST_W - 10, LIST_Y + 8, 4, track_h, 2, C_LINE);
        gfx_round(c, LIST_W - 10, ty, 4, thumb_h, 2, C_DIM);
    }
}

static int list_animating(const struct panel * p, const struct panel_view * v, uint32_t now)
{
    if (!tween_done(&p->hl, now) || !tween_done(&p->scroll, now)) return 1;
    return delayed_q8(now, p->t_open + 100 + VIS * T_STAGGER, dur(v, T_CASCADE)) < 256;
}

/* ---------------------------------------------------------------- detail pane */

static int chip(const struct panel_view * v, struct canvas * c, int x, int y, const char * s)
{
    int w = ui_text_w(UF_SMALL, s) + 18;
    gfx_round(c, x, y, w, 24, 12, C_TRACK);
    ui_text(c, UF_SMALL, x + 9, y + 6, C_TEXT, UA_LEFT, s);
    return w + 8;
}

static void render_detail(struct panel * p, const struct panel_view * v, struct canvas * c, uint32_t now)
{
    gfx_clip(c, DX0 - 1, BAR_H, SW, HINT_Y);
    gfx_fill(c, DX0 - 1, BAR_H, 1, LIST_H, C_LINE);
    gfx_fill(c, DX0, BAR_H, SW - DX0, LIST_H, TH->pane);
    if (p->sel < 0 || p->sel >= v->count) return;

    const struct rec_profile * pr = &v->profiles[p->sel];
    const struct rec_format * f = &pr->nominal;
    int x = DX0 + DPAD + tween_value(&p->slide, now);
    char buf[96];

    ui_text(c, UF_MED, x, BAR_H + 12, C_DIM, UA_LEFT, pr->name);
    if (is_h264(pr))
    {
        ui_text(c, UF_LARGE, x, BAR_H + 40, C_TEXT, UA_LEFT, "1920 x 1080");
        int cx = x;
        cx += chip(v, c, cx, BAR_H + 86, "H.264");
        cx += chip(v, c, cx, BAR_H + 86, "with sound");
        chip(v, c, cx, BAR_H + 86, "RAW off");
        ui_text(c, UF_SMALL, x, BAR_H + 130, C_DIM, UA_LEFT, "Canon's own recording: full sensor width, small files,");
        ui_text(c, UF_SMALL, x, BAR_H + 146, C_DIM, UA_LEFT, "no card speed worries. Good for long takes and proxies.");
        return;
    }

    snprintf(buf, sizeof(buf), "%d x %d", f->width, f->height);
    int rw = ui_text(c, UF_LARGE, x, BAR_H + 40, C_TEXT, UA_LEFT, buf);
    if (pr->squeeze == 3)
    {
        snprintf(buf, sizeof(buf), "= %d x %d de-squeezed", f->width * 3, f->height);
        ui_text(c, UF_SMALL, x + rw + 12, BAR_H + 54, C_DIM, UA_LEFT, buf);
    }

    /* chips: bit depth, frame rate, ratio, readout */
    static const char * ratio_s[] = { "16:9", "2:1", "2.20:1", "2.35:1", "2.39:1" };
    static const char * mode_s[] = { "", "1:1", "1x3", "3x3" };
    int cx = x;
    snprintf(buf, sizeof(buf), "%d-bit", f->bits);
    cx += chip(v, c, cx, BAR_H + 86, buf);
    if (f->fps_x1000 == 23976)      snprintf(buf, sizeof(buf), "23.976 fps");
    else if (f->fps_x1000 == 29970) snprintf(buf, sizeof(buf), "29.97 fps");
    else                            snprintf(buf, sizeof(buf), "%d.%d fps", f->fps_x1000 / 1000, f->fps_x1000 % 1000 / 100);
    cx += chip(v, c, cx, BAR_H + 86, buf);
    if (pr->mood != MOOD_3X3 || pr->preset == P33_HFR) cx += chip(v, c, cx, BAR_H + 86, ratio_s[pr->ratio < 5 ? pr->ratio : 0]);
    chip(v, c, cx, BAR_H + 86, mode_s[pr->mood < 4 ? pr->mood : 0]);

    /* sensor diagram: the whole APS-C sensor, and the part this profile reads */
    int dy = BAR_H + 128;
    gfx_fill(c, x, dy, DIAG_W, DIAG_H, C_BG);
    gfx_round_frame(c, x, dy, DIAG_W, DIAG_H, 0, C_FAINT);
    int cw = tween_value(&p->crop_w, now), ch = tween_value(&p->crop_h, now);
    int ox = x + (DIAG_W - cw) / 2, oy = dy + (DIAG_H - ch) / 2;
    gfx_dither(c, ox, oy, cw, ch, C_ACCENT, 2);
    gfx_round_frame(c, ox, oy, cw, ch, 0, C_ACCENT);
    ui_text(c, UF_SMALL, x + 4, dy + DIAG_H + 6, C_FAINT, UA_LEFT, "APS-C sensor");

    int sw_px, sh_px;
    sensor_area(pr, &sw_px, &sh_px);
    int crop_x100 = SENSOR_PX_W * 100 / (sw_px ? sw_px : 1);          /* vs the APS-C frame */
    int ff_x100 = crop_x100 * 161 / 100;                             /* vs full frame (EOS M: 1.61x) */
    int mm_x10 = SENSOR_MM_W10 * sw_px / SENSOR_PX_W;
    int tx = x + DIAG_W + 20;
    ui_text(c, UF_SMALL, tx, dy + 2, C_DIM, UA_LEFT, "Sensor area");
    snprintf(buf, sizeof(buf), "%d.%02dx crop", ff_x100 / 100, ff_x100 % 100);
    ui_text(c, UF_MED, tx, dy + 18, C_TEXT, UA_LEFT, buf);
    snprintf(buf, sizeof(buf), "%d.%d mm wide, %s", mm_x10 / 10, mm_x10 % 10,
             mm_x10 <= 135 ? "S16 class" : mm_x10 <= 200 ? "M4/3 class" : "S35 class");
    ui_text(c, UF_SMALL, tx, dy + 48, C_DIM, UA_LEFT, buf);
    if (v->focal_mm > 0)
    {
        snprintf(buf, sizeof(buf), "%dmm looks like %dmm FF", v->focal_mm, v->focal_mm * ff_x100 / 100);
        ui_text(c, UF_SMALL, tx, dy + 66, C_TEXT, UA_LEFT, buf);
    }
    else ui_text(c, UF_SMALL, tx, dy + 66, C_FAINT, UA_LEFT, "manual lens");

    /* the data-rate gauge */
    int gy = BAR_H + 260;
    int need = need_kBps(v, pr);
    enum budget_verdict vd = budget_verdict(need, v->card_kBps);
    snprintf(buf, sizeof(buf), "%d.%d MB/s", need / 1000, need % 1000 / 100);
    int nw = ui_text(c, UF_MED, x, gy, C_TEXT, UA_LEFT, buf);
    ui_text(c, UF_SMALL, x + nw + 8, gy + 9, C_DIM, UA_LEFT, "needed");
    ui_text(c, UF_MED, SW - DPAD, gy, verdict_color(v, vd), UA_RIGHT, verdict_word(vd, 1));
    int gw = SW - DPAD - (DX0 + DPAD);
    gfx_bar(c, DX0 + DPAD, gy + 32, gw, 10, tween_value(&p->bar, now), 128, C_TRACK, verdict_color(v, vd), TH->burst, C_DIM);
    snprintf(buf, sizeof(buf), "card %d MB/s%s", v->card_kBps / 1000, v->card_auto ? " (est. from SD clock)" : "");
    ui_text(c, UF_SMALL, x, gy + 52, C_DIM, UA_LEFT, buf);

    int mins = budget_minutes_on_card(need, v->free_MB);
    int bpp = budget_bpp_x100(f, v->bpp);
    int learned = (v->bpp_learned_mask >> (f->bits - 10)) & 1;
    if (mins >= 0) snprintf(buf, sizeof(buf), "~%d min fit on the card   %d.%02d bit/px %s", mins, bpp / 100, bpp % 100, learned ? "(learned)" : "(typical)");
    else           snprintf(buf, sizeof(buf), "%d.%02d bit/px %s", bpp / 100, bpp % 100, learned ? "(learned)" : "(typical)");
    ui_text(c, UF_SMALL, x, gy + 70, C_DIM, UA_LEFT, buf);
}

static int detail_animating(const struct panel * p, uint32_t now)
{
    return !tween_done(&p->slide, now) || !tween_done(&p->crop_w, now) ||
           !tween_done(&p->crop_h, now) || !tween_done(&p->bar, now);
}

/* ---------------------------------------------------------------- hint bar */

static void spinner(struct canvas * c, const struct panel_view * v, int cx, int cy, uint32_t now)
{
    static const signed char dx[8] = { 0, 7, 10, 7, 0, -7, -10, -7 };
    static const signed char dy[8] = { -10, -7, 0, 7, 10, 7, 0, -7 };
    int lit = (now / T_SPIN) & 7;
    for (int i = 0; i < 8; i++)
    {
        int age = (lit - i) & 7;
        uint8_t col = age == 0 ? bar_text(v) : age == 1 ? bar_dim(v) : C_LINE;
        gfx_dot(c, cx + dx[i], cy + dy[i], age == 0 ? 3 : 2, col);
    }
}

static void render_hint(struct panel * p, const struct panel_view * v, struct canvas * c, uint32_t now)
{
    gfx_clip(c, 0, HINT_Y, SW, SH);
    gfx_fill(c, 0, HINT_Y, SW, HINT_H, TH->bar);
    gfx_fill(c, 0, HINT_Y, SW, 1, C_LINE);
    int cy = HINT_Y + HINT_H / 2;

    if (p->state == PS_APPLYING)
    {
        char buf[64];
        spinner(c, v, 30, cy, now);
        snprintf(buf, sizeof(buf), "Applying %s ...", v->profiles[p->applying].name);
        ui_text(c, UF_MED, 52, cy - 11, bar_text(v), UA_LEFT, buf);
        return;
    }
    if (p->state == PS_DONE)
    {
        int e = ease_back_q8(anim_t_q8(now, p->t_state, dur(v, 260)));
        gfx_dot(c, 30, cy, 2 + 8 * e / 256, p->toast_ok ? TH->ok : TH->burst);
        ui_text(c, UF_MED, 52, cy - 11, p->toast_ok ? bar_text(v) : TH->burst, UA_LEFT, p->toast);
        return;
    }

    int x = 16, y = cy - 11;
    uint8_t fg = bar_text(v);
    x += keycap(v, c, x, y, "SET", -1) + 6;   x += ui_text(c, UF_SMALL, x, y + 5, fg, UA_LEFT, "Apply") + 22;
    x += keycap(v, c, x, y, "", 10) + 6;      x += ui_text(c, UF_SMALL, x, y + 5, fg, UA_LEFT, "Choose") + 22;
    x += keycap(v, c, x, y, "", 11) + 6;      x += ui_text(c, UF_SMALL, x, y + 5, fg, UA_LEFT, "Theme") + 22;
    x += keycap(v, c, x, y, "MENU", -1) + 6;  ui_text(c, UF_SMALL, x, y + 5, fg, UA_LEFT, "Close");

    char buf[32];
    snprintf(buf, sizeof(buf), "%s theme", TH->name);
    ui_text(c, UF_SMALL, SW - 16, y + 5, bar_dim(v), UA_RIGHT, buf);
}

static int hint_animating(const struct panel * p, const struct panel_view * v, uint32_t now)
{
    if (p->state == PS_APPLYING) return 1;
    if (p->state == PS_DONE) return anim_t_q8(now, p->t_state, dur(v, 260)) < 256;
    return 0;
}

/* ---------------------------------------------------------------- state */

static int max_scroll(const struct panel_view * v)
{
    int m = (v->count - VIS) * ROW_H;
    return m > 0 ? m : 0;
}

static void retarget_detail(struct panel * p, const struct panel_view * v, uint32_t now, int slide)
{
    tween_start(&p->slide, slide, 0, now, dur(v, T_SLIDE), 0);
    tween_start(&p->crop_w, tween_value(&p->crop_w, now), crop_w_target(v, p->sel), now, dur(v, T_CROP), 0);
    tween_start(&p->crop_h, tween_value(&p->crop_h, now), crop_h_target(v, p->sel), now, dur(v, T_CROP), 0);
    tween_start(&p->bar, tween_value(&p->bar, now), gauge_target(v, p->sel), now, dur(v, T_BAR), 0);
    p->detail_dirty = 1;
}

static void select_row(struct panel * p, const struct panel_view * v, int sel, uint32_t now)
{
    if (v->count <= 0) return;
    if (sel < 0) sel = 0;
    if (sel >= v->count) sel = v->count - 1;
    if (sel == p->sel) return;
    int down = sel > p->sel;
    p->sel = sel;

    tween_start(&p->hl, tween_value(&p->hl, now), sel * ROW_H, now, dur(v, T_HL), 2);

    int top = sel * ROW_H, target = p->scroll_px;
    if (top < target) target = top;
    if (top + ROW_H > target + LIST_H) target = top + ROW_H - LIST_H;
    if (target > max_scroll(v)) target = max_scroll(v);
    if (target != p->scroll_px)
    {
        p->scroll_px = target;
        tween_start(&p->scroll, tween_value(&p->scroll, now), target, now, dur(v, T_SCROLL), 1);
    }
    retarget_detail(p, v, now, down ? 18 : -18);
    p->list_dirty = 1;
}

void panel_open(struct panel * p, const struct panel_view * v, uint32_t now)
{
    int sel = (v->active >= 0 && v->active < v->count) ? v->active : 0;
    memset(p, 0, sizeof(*p));
    p->state = PS_OPENING;
    p->t_state = p->t_open = now;
    p->sel = sel;
    p->scroll_px = sel * ROW_H + ROW_H > LIST_H ? sel * ROW_H + ROW_H - LIST_H : 0;
    if (p->scroll_px > max_scroll(v)) p->scroll_px = max_scroll(v);
    tween_start(&p->hl, sel * ROW_H, sel * ROW_H, now, 0, 2);
    tween_start(&p->scroll, p->scroll_px, p->scroll_px, now, 0, 1);
    tween_start(&p->crop_w, DIAG_W, DIAG_W, now, 0, 0);
    tween_start(&p->crop_h, DIAG_H, DIAG_H, now, 0, 0);
    retarget_detail(p, v, now, 0);
    p->bar.dur = (uint16_t)dur(v, T_BAR + 200);
    tween_start(&p->reveal, 0, SH / 2, now, dur(v, T_OPEN), 0);
    p->prev_y0 = p->prev_y1 = SH / 2;
    p->full_redraw = 1;
}

void panel_close(struct panel * p, const struct panel_view * v, uint32_t now)
{
    if (p->state == PS_CLOSED || p->state == PS_CLOSING) return;
    p->state = PS_CLOSING;
    p->t_state = now;
    tween_start(&p->reveal, tween_value(&p->reveal, now), 0, now, dur(v, T_CLOSE), 1);
}

void panel_theme_changed(struct panel * p)
{
    p->full_redraw = 1;
}

int panel_key(struct panel * p, const struct panel_view * v, int key, uint32_t now)
{
    switch (p->state)
    {
        case PS_OPENING:
        case PS_OPEN:
            switch (key)
            {
                case PK_UP:    select_row(p, v, p->sel - 1, now); break;
                case PK_DOWN:  select_row(p, v, p->sel + 1, now); break;
                case PK_LEFT:  return PA_THEME_PREV;
                case PK_RIGHT: return PA_THEME_NEXT;
                case PK_SET:
                    if (v->count <= 0) break;
                    p->state = PS_APPLYING;
                    p->t_state = now;
                    p->applying = p->sel;
                    p->footer_dirty = 1;
                    return PA_APPLY;
                case PK_CLOSE:
                    panel_close(p, v, now);
                    return PA_CLOSED_BY_USER;
            }
            return PA_NONE;
        case PS_DONE:
            if (key != PK_NONE) { panel_close(p, v, now); return PA_CLOSED_BY_USER; }
            return PA_NONE;
        default:
            return PA_NONE;
    }
}

void panel_apply_result(struct panel * p, int ok, const char * msg, uint32_t now)
{
    if (p->state != PS_APPLYING) return;
    p->state = PS_DONE;
    p->t_state = now;
    p->toast_ok = ok;
    snprintf(p->toast, sizeof(p->toast), "%s", msg ? msg : (ok ? "Ready" : "Could not apply"));
    p->footer_dirty = p->list_dirty = 1;
}

void panel_data_changed(struct panel * p)
{
    p->header_dirty = p->list_dirty = p->footer_dirty = p->detail_dirty = 1;
}

/* ---------------------------------------------------------------- one frame */

static void add_op(struct present * out, int y0, int y1, int clear)
{
    if (y1 <= y0 || out->n >= (int)(sizeof(out->op) / sizeof(out->op[0]))) return;
    out->op[out->n].y0 = (short)y0;
    out->op[out->n].y1 = (short)y1;
    out->op[out->n].clear = (char)clear;
    out->n++;
}

int panel_frame(struct panel * p, const struct panel_view * v, struct canvas * c,
                uint32_t now, struct present * out)
{
    out->n = 0;
    out->x0 = 0;
    out->x1 = SW;
    if (p->state == PS_CLOSED) return 0;

    /* open and close like an iris: a band that grows from (or shrinks to) the middle */
    int half = tween_value(&p->reveal, now);
    int b0 = SH / 2 - half, b1 = SH / 2 + half;
    if (b0 < 0) b0 = 0;
    if (b1 > SH) b1 = SH;

    if (p->state == PS_CLOSING)
    {
        add_op(out, p->prev_y0, b0 < p->prev_y1 ? b0 : p->prev_y1, 1);
        add_op(out, b1 > p->prev_y0 ? b1 : p->prev_y0, p->prev_y1, 1);
        p->prev_y0 = b0; p->prev_y1 = b1;
        if (tween_done(&p->reveal, now))
        {
            add_op(out, b0, b1, 1);
            p->state = PS_CLOSED;
            return PF_PRESENTED | PF_CLOSED;
        }
        return PF_PRESENTED | PF_ANIMATING;
    }

    int d0 = 1 << 30, d1 = -1;
#define DIRTY(a, b) do { if ((a) < d0) d0 = (a); if ((b) > d1) d1 = (b); } while (0)

    if (p->full_redraw)
    {
        render_bar(p, v, c, now);
        render_list(p, v, c, now);
        render_detail(p, v, c, now);
        render_hint(p, v, c, now);
        p->full_redraw = p->header_dirty = p->list_dirty = p->footer_dirty = p->detail_dirty = 0;
        DIRTY(0, SH);
    }
    else
    {
        if (p->header_dirty || bar_animating(p, v, now))
        { render_bar(p, v, c, now); p->header_dirty = 0; DIRTY(0, BAR_H); }

        if (p->list_dirty || list_animating(p, v, now))
        { render_list(p, v, c, now); p->list_dirty = 0; DIRTY(LIST_Y, LIST_Y + LIST_H); }

        if (p->detail_dirty || detail_animating(p, now))
        { render_detail(p, v, c, now); p->detail_dirty = 0; DIRTY(BAR_H, HINT_Y); }

        int spin = p->state == PS_APPLYING ? (int)(now / T_SPIN) : -1;
        if (p->footer_dirty || (p->state == PS_APPLYING ? spin != (int)p->spin_tick : hint_animating(p, v, now)))
        { render_hint(p, v, c, now); p->footer_dirty = 0; p->spin_tick = (uint32_t)spin; DIRTY(HINT_Y, SH); }
    }
    gfx_clip_all(c);
#undef DIRTY

    /* rows to copy: newly revealed by the iris, plus whatever was redrawn inside the band.
     * Merge them so no row is copied twice in one frame. */
    int r[3][2], nr = 0;
    if (b0 < p->prev_y0) { r[nr][0] = b0; r[nr][1] = p->prev_y0 < b1 ? p->prev_y0 : b1; nr++; }
    if (b1 > p->prev_y1) { r[nr][0] = p->prev_y1 > b0 ? p->prev_y1 : b0; r[nr][1] = b1; nr++; }
    if (d1 > d0)         { r[nr][0] = d0 < b0 ? b0 : d0; r[nr][1] = d1 > b1 ? b1 : d1; nr++; }
    for (int i = 0; i < nr; i++)                /* sort by start (3 items) */
        for (int j = i + 1; j < nr; j++)
            if (r[j][0] < r[i][0]) { int a = r[i][0], b = r[i][1]; r[i][0] = r[j][0]; r[i][1] = r[j][1]; r[j][0] = a; r[j][1] = b; }
    for (int i = 0; i < nr; i++)
    {
        if (r[i][1] <= r[i][0]) continue;
        if (out->n && r[i][0] <= out->op[out->n - 1].y1)
        {
            if (r[i][1] > out->op[out->n - 1].y1) out->op[out->n - 1].y1 = (short)r[i][1];
        }
        else add_op(out, r[i][0], r[i][1], 0);
    }
    p->prev_y0 = b0; p->prev_y1 = b1;

    if (p->state == PS_OPENING && tween_done(&p->reveal, now)) { p->state = PS_OPEN; p->t_state = now; }

    int flags = out->n ? PF_PRESENTED : 0;
    if (p->state == PS_OPENING || bar_animating(p, v, now) || list_animating(p, v, now) ||
        detail_animating(p, now) || hint_animating(p, v, now) || p->state == PS_DONE)
        flags |= PF_ANIMATING;

    if (p->state == PS_DONE && p->toast_ok && (now - p->t_state) > (uint32_t)T_DONE)
        panel_close(p, v, now);
    return flags;
}
