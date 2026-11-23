/*
 * End-to-end UX simulation: one shooting session on the EOS M screen, 720 x 480.
 *
 *   e2e <fonts dir> <out dir>
 *
 * The panel is the real code (panel.c, the same calls moth_ui.c makes).
 * Around it, LiveView and Magic Lantern's own bars are mocked so the whole flow
 * can be judged: LiveView -> INFO -> choose -> theme -> apply -> record (tally)
 * -> stop -> the camera learns the clip's real compression.
 * Writes frames at 25 fps plus storyboard.txt (what happens when).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "simlib.h"
#include "../modules/moth_ui/panel.h"

static uint8_t back[UI_W * UI_H];
static struct canvas screen = { front, UI_W, 0, 0, UI_W, UI_H, 0, 0, UI_W, UI_H };

static struct rec_profile prof[MAX_PROFILES];
static struct bpp_table bpp;
static struct panel_view view;
static int theme = THEME_MOTHLIGHT;

/* ---- mocks of what Magic Lantern itself draws in LiveView (illustrative) */
static void ml_bars(int recording, int rec_ms)
{
    const struct rec_profile * p = &prof[view.active];
    char buf[64];
    /* ML's bottom info bar in movie mode */
    gfx_fill(&screen, 0, 446, UI_W, 34, UC_BLACK);
    int x = 12;
    x += ui_text(&screen, UF_MED, x, 451, UC_WHITE, UA_LEFT, "ISO 400") + 22;
    x += ui_text(&screen, UF_MED, x, 451, UC_WHITE, UA_LEFT, "1/48") + 22;
    x += ui_text(&screen, UF_MED, x, 451, UC_WHITE, UA_LEFT, "f/2.5") + 22;
    ui_text(&screen, UF_MED, x, 451, UC_WHITE, UA_LEFT, "5600K");
    /* the Mothlight info item: need / card, coloured by verdict */
    int need = budget_kBps(&p->nominal, &bpp);
    enum budget_verdict vd = budget_verdict(need, view.card_kBps);
    snprintf(buf, sizeof(buf), "%d/%dMB/s", need / 1000, view.card_kBps / 1000);
    ui_text(&screen, UF_MED, UI_W - 12, 451, vd == BUDGET_CONTINUOUS ? UC_GREEN : vd == BUDGET_MARGINAL ? UC_YELLOW : UC_RED, UA_RIGHT, buf);

    /* ML's top bar: Crop Mood preset and RAW resolution */
    gfx_fill(&screen, 0, 0, UI_W, 30, UC_SHADE);
    snprintf(buf, sizeof(buf), "%s   RAW %dx%d %d-bit", p->name, p->nominal.width, p->nominal.height, p->nominal.bits);
    ui_text(&screen, UF_SMALL, 12, 9, UC_WHITE, UA_LEFT, buf);
    if (recording)
    {
        int s = rec_ms / 1000;
        snprintf(buf, sizeof(buf), "REC %02d:%02d  %d fr", s / 60, s % 60, rec_ms * 24 / 1000);
        gfx_dot(&screen, UI_W - 150, 15, 5, UC_RED);
        ui_text(&screen, UF_SMALL, UI_W - 12, 9, UC_WHITE, UA_RIGHT, buf);
    }
}

/* ---- what moth_ui.c draws in LiveView: the REC tally */
static void tally(int on, int t)
{
    if (!on) return;
    uint8_t col = ((t / 500) & 1) ? UC_DARK_RED : UC_RED;
    gfx_fill(&screen, 0, 0, UI_W, 4, col);
    gfx_fill(&screen, 0, UI_H - 4, UI_W, 4, col);
    gfx_fill(&screen, 0, 0, 4, UI_H, col);
    gfx_fill(&screen, UI_W - 4, 0, 4, UI_H, col);
}

/* ---- ML's NotifyBox, used by moth_ui.c after it learned from a clip */
static void notify(const char * msg)
{
    int w = ui_text_w(UF_MED, msg) + 40;
    gfx_round(&screen, (UI_W - w) / 2, 200, w, 50, 10, UC_BLACK);
    gfx_round_frame(&screen, (UI_W - w) / 2, 200, w, 50, 10, UC_GRAY(40));
    ui_text(&screen, UF_MED, UI_W / 2, 213, UC_WHITE, UA_CENTER, msg);
}

int main(int argc, char ** argv)
{
    if (argc < 3) { fprintf(stderr, "usage: e2e <fonts dir> <out dir>\n"); return 1; }
    load_fonts(argv[1]);
    make_lv();
    budget_default_table(&bpp);

    view.profiles = prof;
    view.count = profiles_builtin(prof, MAX_PROFILES);
    view.active = 3;                                   /* 2.5K Cinema */
    view.bpp = &bpp;
    view.card_kBps = budget_card_kBps_for_clock(1);
    view.card_auto = 1;
    view.free_MB = 49 * 1024;
    view.battery = -1;
    view.engine = "Crop Mood";
    view.version = "0.1.0";
    view.focal_mm = 50;
    view.theme = &moth_themes[theme];

    struct { int t; int key; const char * what; } script[] = {
        { 1200, -1,       "INFO pressed in LiveView: the panel opens like an iris" },
        { 2300, PK_UP,    "UP: 2.8K Scope (detail pane slides, crop box shrinks, gauge refills)" },
        { 2700, PK_UP,    "UP: 3K Wide 2.39" },
        { 3100, PK_UP,    "UP: 3K Scope -- BURST on a 65 MB/s card" },
        { 4300, PK_RIGHT, "RIGHT: theme Stock, live" },
        { 5300, PK_RIGHT, "RIGHT: theme Studio" },
        { 6300, PK_LEFT,  "LEFT: back to Stock" },
        { 6900, PK_LEFT,  "LEFT: back to Mothlight" },
        { 7700, PK_SET,   "SET: apply 3K Scope (Crop Mood + MLV Lite set by menu name, x5 zoom)" },
        { 0, 0, 0 },
    };
    const int t_apply_done = 8900, t_rec = 11000, t_stop = 15000, t_end = 18500;

    char path[512];
    snprintf(path, sizeof(path), "%s/storyboard.txt", argv[2]);
    FILE * story = fopen(path, "w");

    struct panel p;
    memset(&p, 0, sizeof(p));
    struct canvas c = { back, UI_W, 0, 0, UI_W, UI_H, 0, 0, UI_W, UI_H };
    int panel_up = 0, si = 0, applied = 0, learned = 0, max_rows = 0, frames = 0, drawn = 0;
    long bytes = 0;

    for (int t = 0, n = 0; t <= t_end; t += 40, n++)
    {
        while (script[si].what && script[si].t <= t)
        {
            fprintf(story, "%5d ms  frame %3d  %s\n", t, n, script[si].what);
            if (script[si].key < 0) { panel_open(&p, &view, t); panel_up = 1; }
            else
            {
                int act = panel_key(&p, &view, script[si].key, t);
                if (act == PA_THEME_NEXT || act == PA_THEME_PREV)
                {
                    theme = (theme + (act == PA_THEME_NEXT ? 1 : THEME_COUNT - 1)) % THEME_COUNT;
                    view.theme = &moth_themes[theme];
                    panel_theme_changed(&p);
                }
            }
            si++;
        }
        if (!applied && t >= t_apply_done && p.state == PS_APPLYING)
        {
            char msg[64];
            snprintf(msg, sizeof(msg), "Ready  %dx%d %d-bit", prof[p.applying].nominal.width,
                     prof[p.applying].nominal.height, prof[p.applying].nominal.bits);
            view.active = p.applying;
            panel_apply_result(&p, 1, msg, t);
            applied = 1;
            fprintf(story, "%5d ms  frame %3d  applied: %s; the panel closes by itself after 1.1 s\n", t, n, msg);
        }

        int recording = t >= t_rec && t < t_stop;
        if (t == t_rec)  fprintf(story, "%5d ms  frame %3d  REC: red tally frame round the screen, no animations while recording\n", t, n);
        if (t == t_stop)
        {
            /* a 4 s clip whose files came out at 5.71 bit/px */
            uint32_t kB = (uint32_t)(3072LL * 1308 * 23976 / 1000 * 4 * 571 / 800 / 1000);
            int m = budget_measure_bpp_x100(&prof[view.active].nominal, kB, 4000);
            budget_learn(&bpp, 10, m);
            view.bpp_learned_mask |= 1;
            learned = t;
            fprintf(story, "%5d ms  frame %3d  STOP: clip measured at %d.%02d bit/px, 10-bit estimate now %d.%02d\n",
                    t, n, m / 100, m % 100, bpp.x100[0] / 100, bpp.x100[0] % 100);
        }

        /* LiveView layer (what ML and Canon draw), then the panel on top */
        if (!panel_up)
        {
            memset(front, 0, sizeof(front));
            ml_bars(recording, t - t_rec);
            tally(recording, t);
            if (learned && t - learned < 2200)
            {
                char msg[64];
                snprintf(msg, sizeof(msg), "Learned: 10-bit = %d.%02d bit/px", bpp.x100[0] / 100, bpp.x100[0] % 100);
                notify(msg);
            }
        }
        if (panel_up)
        {
            if (p.state == PS_OPENING && t == 1200)
            {
                /* the glue clears ML's overlays before the iris opens */
                memset(front, 0, sizeof(front));
            }
            struct present out;
            int fl = panel_frame(&p, &view, &c, t, &out);
            int rows = 0;
            for (int i = 0; i < out.n; i++)
                for (int y = out.op[i].y0; y < out.op[i].y1; y++, rows++)
                {
                    if (out.op[i].clear) memset(front + y * UI_W + out.x0, 0, out.x1 - out.x0);
                    else memcpy(front + y * UI_W + out.x0, back + y * UI_W + out.x0, out.x1 - out.x0);
                }
            if (rows > max_rows) max_rows = rows;
            bytes += (long)rows * UI_W;
            frames++;
            if (rows) drawn++;
            if (fl & PF_CLOSED)
            {
                panel_up = 0;
                fprintf(story, "%5d ms  frame %3d  panel closed; ML overlays come back with the new mode\n", t, n);
            }
        }
        write_frame(argv[2], n);
    }
    fprintf(story, "\npanel frames: %d, frames that drew: %d, average %ld bytes/frame copied to the screen, peak %d rows\n",
            frames, drawn, bytes / (frames ? frames : 1), max_rows);
    fclose(story);
    printf("done: %d panel frames, %d drew, avg %ld B/frame\n", frames, drawn, bytes / (frames ? frames : 1));
    return 0;
}
