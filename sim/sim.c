/*
 * Mothlight UI simulator: runs the real panel code on a PC.
 *
 *   sim <fonts dir> <out dir> [theme 0..5] [reduced]
 *
 * Loads Magic Lantern's .rbf fonts, plays a scripted session (open, scroll,
 * apply, auto-close) at 30 fps against a synthetic LiveView picture, and
 * writes every frame as a PPM plus stats.txt with how much work each frame
 * did (rows drawn, bytes copied to the screen). sim/render.py turns the
 * frames into an animated GIF and PNG stills.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../modules/moth_ui/panel.h"
#include "../modules/moth_ui/ui_text.h"
#include "simlib.h"

#define PITCH UI_W
static uint8_t back[UI_W * UI_H];

static uint8_t back[UI_W * UI_H];

int main(int argc, char ** argv)
{
    if (argc < 3) { fprintf(stderr, "usage: sim <fonts dir> <out dir> [theme 0..5] [reduced]\n"); return 1; }
    int theme = argc > 3 ? atoi(argv[3]) : 0;
    if (theme < 0 || theme >= THEME_COUNT) theme = 0;
    (void)PANEL_SHADOW;
    load_fonts(argv[1]);
    make_lv();

    struct rec_profile prof[MAX_PROFILES];
    struct bpp_table bpp;
    budget_default_table(&bpp);
    bpp.x100[0] = 588;                       /* pretend 10-bit was learned from clips */

    struct panel_view v = {
        .profiles = prof, .count = profiles_builtin(prof, MAX_PROFILES), .active = 3,
        .bpp = &bpp, .bpp_learned_mask = 1, .card_kBps = budget_card_kBps_for_clock(1), .card_auto = 1,
        .free_MB = 59 * 1024 + 300, .battery = 80, .engine = "Crop Mood", .version = "0.1.0",
        .reduced_motion = argc > 4, .theme = &moth_themes[theme], .focal_mm = 50,
    };

    /* the scripted session: time (ms) -> key */
    struct { int t; int key; } script[] = {
        { 900, PK_UP }, { 1150, PK_UP }, { 1400, PK_UP }, { 2100, PK_DOWN }, { 2300, PK_DOWN },
        { 2500, PK_DOWN }, { 2700, PK_DOWN }, { 2900, PK_DOWN }, { 3700, PK_UP }, { 3900, PK_UP },
        { 4050, PK_UP }, { 4200, PK_UP }, { 4350, PK_UP }, { 4500, PK_UP }, { 5300, PK_SET },
    };
    int apply_done_at = -1, si = 0;

    struct canvas c = { back, PITCH, 0, 0, UI_W, UI_H, 0, 0, UI_W, UI_H };
    struct panel p;
    memset(&p, 0, sizeof(p));
    memset(back, 0, sizeof(back));
    memset(front, 0, sizeof(front));

    char stats_path[512];
    snprintf(stats_path, sizeof(stats_path), "%s/stats.txt", argv[2]);
    FILE * stats = fopen(stats_path, "w");
    fprintf(stats, "# frame t_ms state ops rows_shown bytes_to_screen\n");

    long total_bytes = 0; int frames = 0, idle_frames = 0, closed = 0;
    panel_open(&p, &v, 0);
    for (int t = 0, n = 0; t <= 9000 && !closed; t += 33, n++)
    {
        while (si < (int)(sizeof(script) / sizeof(script[0])) && script[si].t <= t)
        {
            if (panel_key(&p, &v, script[si].key, t) == PA_APPLY) apply_done_at = t + 1100;
            si++;
        }
        if (apply_done_at >= 0 && t >= apply_done_at)
        {
            char msg[48];
            snprintf(msg, sizeof(msg), "Ready  %dx%d %d-bit", prof[p.applying].nominal.width,
                     prof[p.applying].nominal.height, prof[p.applying].nominal.bits);
            panel_apply_result(&p, 1, msg, t);
            v.active = p.applying;
            apply_done_at = -1;
        }

        struct present out;
        int fl = panel_frame(&p, &v, &c, t, &out);
        int rows = 0;
        for (int i = 0; i < out.n; i++)
            for (int y = out.op[i].y0; y < out.op[i].y1; y++)
            {
                if (out.op[i].clear) memset(front + y * UI_W + out.x0, 0, out.x1 - out.x0);
                else memcpy(front + y * UI_W + out.x0, back + y * UI_W + out.x0, out.x1 - out.x0);
                rows++;
            }
        long bytes = (long)rows * (out.x1 - out.x0);
        total_bytes += bytes; frames++;
        if (!(fl & PF_PRESENTED)) idle_frames++;
        fprintf(stats, "%d %d %d %d %d %ld\n", n, t, p.state, out.n, rows, bytes);
        write_frame(argv[2], n);
        if (fl & PF_CLOSED) closed = 1;
    }
    fprintf(stats, "# frames %d, frames that drew nothing %d, avg bytes/frame %ld\n",
            frames, idle_frames, total_bytes / (frames ? frames : 1));
    fclose(stats);
    printf("%d frames, %d idle, avg %ld bytes/frame to screen\n", frames, idle_frames, total_bytes / (frames ? frames : 1));
    return 0;
}
