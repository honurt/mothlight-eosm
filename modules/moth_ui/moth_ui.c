/*
 * Mothlight UI for the Canon EOS M -- the camera side.
 *
 *   - the profile panel (panel.c) over LiveView: INFO opens it, SET applies
 *   - LiveView info item: active profile and its data-rate budget
 *   - REC tally: a red frame round the screen while recording
 *   - learns the real compression of your clips, per bit depth
 *
 * It drives Crop Mood and MLV Lite only through their menu entries (by name,
 * the way Lua scripts do), so it needs no changes to Magic Lantern's core
 * and keeps working when those modules are updated.
 *
 * Performance rules, because this shares a 400 MHz ARM9 with the RAW recorder:
 *   - one low-priority task; it sleeps 100-250 ms when nothing is on screen
 *   - nothing animates while recording; the tally is a few memsets at 2 Hz
 *   - the panel draws into its own buffer and copies only rows that changed
 *   - menu values are read once a second, and never while recording
 *
 * Copyright (C) 2026 Onur and contributors. GPL-2.0 (see LICENSE).
 */

#include <dryos.h>
#include <module.h>
#include <menu.h>
#include <config.h>
#include <bmp.h>
#include <lvinfo.h>
#include <property.h>
#include <propvalues.h>
#include <raw.h>
#include <fps.h>
#include <zebra.h>
#include <fio-ml.h>
#include <shoot.h>
#include <gui-common.h>
#include <lens.h>

#include "version.h"
#include "panel.h"
#include "keymap.h"
#include "repair.h"
#include "ui_text.h"
#include "budget.h"
#include "profiles.h"
#include "themes.h"

extern void set_lv_zoom(int zoom);
extern int fps_get_current_x1000();

/* ---------------------------------------------------------------- settings */

static CONFIG_INT("moth.theme",   cfg_theme,   THEME_MOTHLIGHT);
static CONFIG_INT("moth.key",     cfg_key,     1);    /* 0 off, 1 INFO (two-finger tap is ML's menu on the EOS M) */
static CONFIG_INT("moth.motion",  cfg_motion,  1);    /* 1 smooth, 0 off */
static CONFIG_INT("moth.card",    cfg_card,    0);    /* index in card_mb[] (0 = auto) */
static CONFIG_INT("moth.tally",   cfg_tally,   1);    /* 0 off, 1 frame, 2 pulsing frame */
static CONFIG_INT("moth.active",  cfg_active,  -1);
static CONFIG_INT("moth.learned", cfg_learned, 0);    /* bit (bits-10): learned from clips */
static CONFIG_INT("moth.bpp10",   cfg_bpp10,   0);
static CONFIG_INT("moth.bpp11",   cfg_bpp11,   0);
static CONFIG_INT("moth.bpp12",   cfg_bpp12,   0);
static CONFIG_INT("moth.bpp14",   cfg_bpp14,   0);

static const int card_mb[] = { 0, 30, 40, 50, 60, 65, 70, 75, 80, 90 };

/* ---------------------------------------------------------------- state */

static struct rec_profile profiles[MAX_PROFILES];
static int profile_count;
static int profiles_from_file;

static struct bpp_table bpp;
static struct panel panel;
static struct panel_view view;

static uint8_t * back_buf;                  /* the panel's own drawing buffer, only while open */
static struct canvas back;

/* keys go from the GUI task (key handler) to our task through this little ring */
static volatile int key_ring[8];
static volatile int key_head, key_tail;
static volatile int open_request;

static int overlay_active;
static volatile uint32_t last_frame_ms;     /* watchdog: the key handler gives keys back if this stops moving */
static int saved_global_draw = -1;

static int apply_phase;                     /* 0 idle, 1 settings sent, 2 waiting for LiveView */
static uint32_t apply_t0;

static struct rec_format live_fmt;          /* what RAW video is set to now (read at 1 Hz) */
static int was_recording;
static uint32_t rec_t0;
static struct rec_format rec_fmt;
static int tally_drawn;

static volatile int task_quit;

/* ---------------------------------------------------------------- small helpers */

static void push_key(int k)
{
    int next = (key_head + 1) & 7;
    if (next != key_tail) { key_ring[key_head] = k; key_head = next; }
}

static int pop_key(void)
{
    if (key_tail == key_head) return PK_NONE;
    int k = key_ring[key_tail];
    key_tail = (key_tail + 1) & 7;
    return k;
}

static int can_show_panel(void)
{
    return lv && is_movie_mode() && !gui_menu_shown() && !RECORDING && display_is_on();
}

static const struct ui_theme * theme(void)
{
    return &moth_themes[(cfg_theme >= 0 && cfg_theme < THEME_COUNT) ? cfg_theme : 0];
}

static void bpp_from_config(void)
{
    budget_default_table(&bpp);
    if (cfg_bpp10) bpp.x100[0] = cfg_bpp10;
    if (cfg_bpp11) bpp.x100[1] = cfg_bpp11;
    if (cfg_bpp12) bpp.x100[2] = cfg_bpp12;
    if (cfg_bpp14) bpp.x100[4] = cfg_bpp14;
}

static void bpp_to_config(void)
{
    cfg_bpp10 = bpp.x100[0]; cfg_bpp11 = bpp.x100[1];
    cfg_bpp12 = bpp.x100[2]; cfg_bpp14 = bpp.x100[4];
}

static int card_kBps(int * is_auto)
{
    int i = (cfg_card >= 0 && cfg_card < (int)COUNT(card_mb)) ? cfg_card : 0;
    if (card_mb[i]) { *is_auto = 0; return card_mb[i] * 1000; }
    int clock = menu_get_value_from_script("Debug", "SD Overclock");
    *is_auto = 1;
    return budget_card_kBps_for_clock(clock > 0 ? clock : 0);
}

static int free_MB(void)
{
    struct card_info * card = get_shooting_card();
    if (!card) return -1;
    return get_free_space_32k(card) / 32;
}

static void fill_view(void)
{
    view.profiles = profiles;
    view.count = profile_count;
    view.active = cfg_active < profile_count ? cfg_active : -1;
    view.bpp = &bpp;
    view.bpp_learned_mask = cfg_learned;
    view.card_kBps = card_kBps(&view.card_auto);
    view.free_MB = free_MB();
    view.battery = -1;
    view.engine = MOTH_UI_ENGINE[0] == 'c' ? "Crop Mood" : "ML dev";
    view.version = MOTH_UI_VERSION;
    view.reduced_motion = !cfg_motion;
    view.theme = theme();
    view.focal_mm = lens_info.focal_len;       /* 0 with a manual (non-electronic) lens */
}

/* ---------------------------------------------------------------- reading RAW video settings */

static int parse_int(const char ** s)
{
    int v = 0, any = 0;
    while (**s >= '0' && **s <= '9') { v = v * 10 + (**s - '0'); (*s)++; any = 1; }
    return any ? v : -1;
}

/* "ON, 3072x1308" -> width/height; bit depth from Crop Mood / Data format */
static void read_live_format(void)
{
    static struct menu_display_info info;   /* large: keep it off the task stack */
    struct rec_format f = { 0, 0, 0, 14, 1 };

    if (menu_get_value_from_script("Movie", "RAW video") > 0)
    {
        const char * s = menu_get_str_value_from_script("Movie", "RAW video", &info);
        while (s && *s && !(*s >= '0' && *s <= '9')) s++;
        if (s && *s)
        {
            int w = parse_int(&s);
            if (*s == 'x') { s++; f.width = w; f.height = parse_int(&s); }
        }
    }

    int format = menu_get_value_from_script("RAW video", "Data format");
    int mood = menu_get_value_from_script("Movie", "Crop mood");
    if (format >= 3 && mood > 0)
    {
        static const int depth[] = { 14, 12, 11, 10 };
        int b = menu_get_value_from_script("Crop mood", "Bit-depth");
        f.bits = (b >= 0 && b < 4) ? depth[b] : 14;
        f.lossless = 1;
    }
    else
    {
        static const int depth[] = { 14, 12, 10, 14, 12, 10 };
        f.bits = (format >= 0 && format < 6) ? depth[format] : 14;
        f.lossless = format >= 3;
    }
    f.fps_x1000 = fps_get_current_x1000();
    live_fmt = f;
}

/* ---------------------------------------------------------------- applying a profile */

static int set_menu(const char * menu, const char * entry, int value)
{
    int r = menu_set_value_from_script(menu, entry, value);
    return r != INT_MIN;
}

static const char * apply_start(const struct rec_profile * p)
{
    if (RECORDING) return "Stop recording first";
    if (!is_movie_mode()) return "Set the mode dial to movie";

    if (p->mood == MOOD_OFF)
    {
        set_menu("Movie", "Crop mood", 0);
        set_menu("Movie", "RAW video", 0);
        if (lv_dispsize > 1) set_lv_zoom(1);
        return 0;
    }

    static const char * preset_entry[] = { "", "Preset:", "Preset: ", "Preset:  " };
    if (!set_menu("Movie", "RAW video", 1))   return "Load mlv_lite in the Modules tab";
    if (!set_menu("Movie", "Crop mood", p->mood)) return "Load crop_rec in the Modules tab";
    set_menu("RAW video", "Data format", 3);            /* lossless; Crop Mood picks the depth */
    set_menu("Crop mood", preset_entry[(int)p->mood], p->preset);
    set_menu("Crop mood", "Aspect ratio:", p->ratio);
    set_menu("Crop mood", "Framerate:", p->fps);
    set_menu("Crop mood", "Bit-depth", p->bits);
    set_menu("RAW video", "Resolution", 11);            /* widest: clamps to the preset */
    set_menu("RAW video", "Aspect ratio", 17);          /* tallest: follows Crop Mood's frame */

    /* Crop Mood's presets run in Canon's x5 zoom on the EOS M */
    if (lv_dispsize == 1) set_lv_zoom(5);
    return 0;
}

static void apply_poll(uint32_t now)
{
    const struct rec_profile * p = &profiles[panel.applying];
    char msg[48];

    if (apply_phase == 1)
    {
        const char * err = apply_start(p);
        if (err)
        {
            panel_apply_result(&panel, 0, err, now);
            apply_phase = 0;
            return;
        }
        cfg_active = panel.applying;
        apply_phase = 2;
        apply_t0 = now;
        return;
    }

    /* phase 2: Crop Mood refreshes LiveView on its own; wait for the new frame size */
    if (now - apply_t0 < 600) return;
    if (p->mood == MOOD_OFF)
    {
        panel_apply_result(&panel, 1, "Canon H.264 ready", now);
        apply_phase = 0;
        return;
    }
    read_live_format();
    int close = live_fmt.width > 0 && ABS(live_fmt.width - p->nominal.width) <= 32;
    if (close || now - apply_t0 > 4000)
    {
        if (close)
            snprintf(msg, sizeof(msg), "Ready  %dx%d %d-bit", live_fmt.width, live_fmt.height, live_fmt.bits);
        else
            snprintf(msg, sizeof(msg), "Set. Check RAW video (%dx%d)", live_fmt.width, live_fmt.height);
        panel_apply_result(&panel, 1, msg, now);
        panel.full_redraw = 1;          /* LiveView refresh may have wiped the overlay */
        apply_phase = 0;
    }
}

/* ---------------------------------------------------------------- the overlay */

static void present(const struct present * o)
{
    uint8_t * front = bmp_vram_real();
    if (!front || !back_buf) return;
    int w = o->x1 - o->x0;
    for (int i = 0; i < o->n; i++)
        for (int y = o->op[i].y0; y < o->op[i].y1; y++)
        {
            uint8_t * dst = front + y * BMPPITCH + o->x0;
            if (o->op[i].clear) memset(dst, COLOR_EMPTY, w);
            else memcpy(dst, back.px + y * back.pitch + o->x0, w);
        }
}

/* Anything else that draws on the screen while the panel is up (Canon refreshing its
   LiveView overlay, an ML notification) would leave holes, because frames only copy the
   rows that changed. Probe a spread of pixels against the back buffer every frame and
   repaint the whole screen when they disagree. ~100 byte reads, i.e. free. */
static int screen_was_disturbed(void)
{
    uint8_t * front = bmp_vram_real();
    if (!front || !back_buf) return 0;
    return moth_screen_differs(front, BMPPITCH, back.px, back.pitch, UI_W);
}

static void present_full(void)
{
    uint8_t * front = bmp_vram_real();
    if (!front || !back_buf) return;
    for (int y = 0; y < UI_H; y++)
        memcpy(front + y * BMPPITCH, back.px + y * back.pitch, UI_W);
}

static int overlay_begin(void)
{
    /* the panel's own full-screen buffer (338 KB), allocated only while the panel is open */
    back_buf = malloc(UI_W * UI_H);
    if (!back_buf) return 0;
    back.px = back_buf;
    back.pitch = UI_W;
    back.bx0 = 0; back.bx1 = UI_W;
    back.by0 = 0; back.by1 = UI_H;
    gfx_clip_all(&back);

    /* ML's overlays step aside while the panel is up; Canon stops redrawing its own */
    saved_global_draw = menu_get_value_from_script("Overlay", "Global Draw");
    if (saved_global_draw > 0) menu_set_value_from_script("Overlay", "Global Draw", 0);
    canon_gui_disable_front_buffer();
    msleep(50);
    clrscr();
    last_frame_ms = get_ms_clock();
    overlay_active = 1;
    return 1;
}

static void overlay_end(void)
{
    overlay_active = 0;
    if (saved_global_draw > 0) menu_set_value_from_script("Overlay", "Global Draw", saved_global_draw);
    saved_global_draw = -1;
    canon_gui_enable_front_buffer(1);
    free(back_buf);
    back_buf = 0;
    redraw();
}

/* ---------------------------------------------------------------- REC tally */

static void tally(uint32_t now)
{
    int want = cfg_tally && RECORDING && display_is_on() && !gui_menu_shown();
    if (want)
    {
        uint8_t col = (cfg_tally == 2 && ((now / 500) & 1)) ? COLOR_DARK_RED : COLOR_RED;
        bmp_fill(col, 0, 0, 720, 4);
        bmp_fill(col, 0, 476, 720, 4);
        bmp_fill(col, 0, 0, 4, 480);
        bmp_fill(col, 716, 0, 4, 480);
        tally_drawn = 1;
    }
    else if (tally_drawn)
    {
        bmp_fill(COLOR_EMPTY, 0, 0, 720, 4);
        bmp_fill(COLOR_EMPTY, 0, 476, 720, 4);
        bmp_fill(COLOR_EMPTY, 0, 0, 4, 480);
        bmp_fill(COLOR_EMPTY, 716, 0, 4, 480);
        tally_drawn = 0;
    }
}

/* ---------------------------------------------------------------- learning from clips */

static int ends_with_mlv(const char * name)
{
    int n = strlen(name);
    return n > 4 && streq(name + n - 4, ".MLV");
}

/* the newest .MLV in the DCIM folder, plus its .M00, .M01 ... chunks, in kB.
 * The two engines list directories differently: the newer core hands modules an
 * opaque entry they must convert (the struct differs between camera generations). */
static uint32_t last_clip_kB(void)
{
    char best[64] = "";
    uint32_t best_ts = 0;
    const char * dir = get_dcim_dir();

#ifdef MOTH_ENGINE_UPSTREAM
    struct fio_file * file = alloc_fio_file();
    if (!file) return 0;
    struct fio_dirent * d = FIO_FindFirstEx(dir, file);
    if (IS_ERROR(d)) { free(file); return 0; }
    do
    {
        struct file_info fi = convert_fio_file_info(file);
        if (!(fi.mode & ATTR_DIRECTORY) && ends_with_mlv(fi.name) && (uint32_t)fi.timestamp >= best_ts)
        {
            best_ts = (uint32_t)fi.timestamp;
            snprintf(best, sizeof(best), "%s", fi.name);
        }
    } while (FIO_FindNextEx(d, file) == 0);
    FIO_FindClose(d);
    free(file);
#else
    struct fio_file file;
    struct fio_dirent * d = FIO_FindFirstEx(dir, &file);
    if (IS_ERROR(d)) return 0;
    do
    {
        if (!(file.mode & ATTR_DIRECTORY) && ends_with_mlv(file.name) && file.timestamp >= best_ts)
        {
            best_ts = file.timestamp;
            snprintf(best, sizeof(best), "%s", file.name);
        }
    } while (FIO_FindNextEx(d, &file) == 0);
    FIO_FindClose(d);
#endif
    if (!best[0]) return 0;

    char path[128];
    uint32_t total_kB = 0, size = 0;
    snprintf(path, sizeof(path), "%s%s", dir, best);
    if (FIO_GetFileSize(path, &size) == 0) total_kB += size / 1000;
    int len = strlen(path);
    for (int i = 0; i < 100; i++)
    {
        snprintf(path + len - 2, 3, "%02d", i);
        if (FIO_GetFileSize(path, &size) != 0) break;
        total_kB += size / 1000;
    }
    return total_kB;
}

static void watch_recording(uint32_t now)
{
    int rec = RECORDING_RAW;
    if (rec && !was_recording)
    {
        rec_t0 = now;
        rec_fmt = live_fmt;             /* read just before, while idle */
    }
    if (!rec && was_recording && rec_fmt.width > 0)
    {
        msleep(500);                    /* let the last chunk close */
        int m = budget_measure_bpp_x100(&rec_fmt, last_clip_kB(), now - rec_t0);
        if (m > 0)
        {
            budget_learn(&bpp, rec_fmt.bits, m);
            cfg_learned |= 1 << (rec_fmt.bits - 10);
            bpp_to_config();
            int b = bpp.x100[rec_fmt.bits - 10];
            NotifyBox(2000, "Learned: %d-bit = %d.%02d bit/px", rec_fmt.bits, b / 100, b % 100);
        }
    }
    was_recording = rec;
}

/* ---------------------------------------------------------------- the task */

static void moth_task(void)
{
    uint32_t last_read = 0;
    while (!task_quit)
    {
        uint32_t now = get_ms_clock();

        if (open_request && !overlay_active)
        {
            open_request = 0;
            if (can_show_panel() && overlay_begin())
            {
                fill_view();
                panel_open(&panel, &view, now);
            }
        }

        if (overlay_active)
        {
            int k;
            while ((k = pop_key()) != PK_NONE)
            {
                int act = panel_key(&panel, &view, k, now);
                if (act == PA_APPLY) apply_phase = 1;
                if (act == PA_THEME_NEXT || act == PA_THEME_PREV)
                {
                    cfg_theme = (cfg_theme + (act == PA_THEME_NEXT ? 1 : THEME_COUNT - 1)) % THEME_COUNT;
                    view.theme = theme();
                    panel_theme_changed(&panel);
                }
            }
            if (apply_phase) apply_poll(now);

            /* something took the screen from us (menu, playback, recording): close at once */
            if (!lv || gui_menu_shown() || RECORDING) panel.state = PS_CLOSING, panel.reveal.dur = 0;

            struct present out;
            last_frame_ms = now;
            view.active = cfg_active;
            int fl = panel_frame(&panel, &view, &back, now, &out);
            if (fl & PF_PRESENTED) present(&out);
            if ((panel.state == PS_OPEN || panel.state == PS_APPLYING || panel.state == PS_DONE)
                && screen_was_disturbed())
                present_full();
            if (fl & PF_CLOSED) overlay_end();
            msleep((fl & PF_ANIMATING) ? 20 : 50);
            continue;
        }

        watch_recording(now);
        tally(now);

        if (!RECORDING && lv && now - last_read > 2000)
        {
            read_live_format();
            last_read = now;
        }
        msleep(RECORDING ? 250 : 100);
    }
}

/* ---------------------------------------------------------------- keys */

static unsigned int moth_keypress_cbr(unsigned int key)
{
    if (overlay_active && get_ms_clock() - last_frame_ms > 2000)
    {
        /* the panel task stopped drawing: never hold the camera's buttons hostage */
        overlay_active = 0;
        canon_gui_enable_front_buffer(1);
        return 1;
    }
    if (overlay_active)
    {
        struct key_route r = moth_route_key(key);
        if (r.panel_key != PK_NONE) push_key(r.panel_key);
        return r.pass_to_canon;
    }

    int trigger = cfg_key == 1 && key == MODULE_KEY_INFO;
    if (trigger && can_show_panel())
    {
        key_head = key_tail = 0;
        open_request = 1;
        return 0;
    }
    return 1;
}

/* EOS M D-pad release codes (platform/EOSM.202/gui.h). ML has no portable name for them,
   so the portable handler above only sees key 0. Passed on, they make Canon redraw its
   LiveView screen over the panel (a black flash, and wiped bars). Swallow them while open. */
#define EOSM_UNPRESS_UP    0x2b
#define EOSM_UNPRESS_RIGHT 0x27
#define EOSM_UNPRESS_LEFT  0x29
#define EOSM_UNPRESS_DOWN  0x2d

static unsigned int moth_keypress_raw_cbr(unsigned int raw)
{
    if (!overlay_active) return 1;
    const struct event * e = (const struct event *) raw;
    if (e && e->type == 0)
        switch (e->param)
        {
            case EOSM_UNPRESS_UP: case EOSM_UNPRESS_RIGHT:
            case EOSM_UNPRESS_LEFT: case EOSM_UNPRESS_DOWN:
                return 0;
        }
    return 1;
}

/* ---------------------------------------------------------------- menu */

static MENU_SELECT_FUNC(open_from_menu)
{
    open_request = 1;
    gui_stop_menu();            /* the task opens the panel once the ML menu has closed */
}

static MENU_UPDATE_FUNC(main_update)
{
    if (cfg_active >= 0 && cfg_active < profile_count)
        MENU_SET_VALUE("%s", profiles[cfg_active].name);
    else
        MENU_SET_VALUE("no profile yet");
    if (!is_movie_mode()) MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Set the mode dial to movie.");
}

static MENU_UPDATE_FUNC(learned_update)
{
    MENU_SET_VALUE("%s", cfg_learned ? "from your clips" : "typical values");
    MENU_SET_HELP("bit/px: 10-bit %d.%02d  11-bit %d.%02d  12-bit %d.%02d  14-bit %d.%02d",
                  bpp.x100[0] / 100, bpp.x100[0] % 100, bpp.x100[1] / 100, bpp.x100[1] % 100,
                  bpp.x100[2] / 100, bpp.x100[2] % 100, bpp.x100[4] / 100, bpp.x100[4] % 100);
}

static MENU_SELECT_FUNC(learned_reset)
{
    cfg_learned = cfg_bpp10 = cfg_bpp11 = cfg_bpp12 = cfg_bpp14 = 0;
    bpp_from_config();
}

static MENU_UPDATE_FUNC(about_update)
{
    MENU_SET_VALUE("v%s", MOTH_UI_VERSION);
    MENU_SET_HELP("%s by Onur. Engine: %s. %d profiles%s.", MOTH_UI_NAME,
                  MOTH_UI_ENGINE, profile_count, profiles_from_file ? " from PROFILES.TXT" : " (built in)");
}

static struct menu_entry moth_menu[] =
{
    {
        .name    = "Mothlight",
        .select  = open_from_menu,
        .update  = main_update,
        .help    = "Recording profiles for the EOS M. Opens the profile panel.",
        .help2   = "Shortcut in LiveView: the INFO button (change it below).",
        .depends_on = DEP_MOVIE_MODE,
        .children = (struct menu_entry[]) {
            { .name = "Open profiles", .select = open_from_menu, .help = "Choose and apply a recording profile." },
            {
                .name = "Theme", .priv = &cfg_theme, .max = THEME_COUNT - 1,
                .choices = CHOICES("Mothlight", "Stock", "Studio", "Amber", "Signal", "Material",
                                   "Soft", "Glass", "Daylight", "Night", "Mono"),
                .help = "How the panel looks. In the panel, LEFT / RIGHT switch it live.",
            },
            {
                .name = "Panel key", .priv = &cfg_key, .max = 1,
                .choices = CHOICES("OFF", "INFO"),
                .help = "INFO opens the panel in LiveView (movie mode, not recording).",
                .help2 = "OFF gives INFO back to Canon; open the panel from this menu instead.",
            },
            {
                .name = "Animations", .priv = &cfg_motion, .max = 1,
                .choices = CHOICES("OFF", "Smooth"),
                .help = "Panel animations. They never run while recording.",
            },
            {
                .name = "Card write speed", .priv = &cfg_card, .max = COUNT(card_mb) - 1,
                .choices = CHOICES("Auto (SD clock)", "30 MB/s", "40 MB/s", "50 MB/s", "60 MB/s",
                                   "65 MB/s", "70 MB/s", "75 MB/s", "80 MB/s", "90 MB/s"),
                .help = "Used for the continuous / edge / burst verdicts.",
                .help2 = "Measure your card with Debug > Benchmarks, then set it here.",
            },
            {
                .name = "REC tally", .priv = &cfg_tally, .max = 2,
                .choices = CHOICES("OFF", "Red frame", "Pulsing frame"),
                .help = "A red frame round the screen while recording.",
            },
            {
                .name = "Compression", .update = learned_update, .select = learned_reset,
                .help = "Learned from your clips after each recording. SET resets it.",
            },
            { .name = "About", .update = about_update, .icon_type = IT_ALWAYS_ON },
            MENU_EOL,
        },
    },
};

/* ---------------------------------------------------------------- LiveView info item */

static LVINFO_UPDATE_FUNC(moth_info)
{
    LVINFO_BUFFER(24);
    if (cfg_active < 0 || cfg_active >= profile_count || RECORDING) return;
    const struct rec_profile * p = &profiles[cfg_active];
    if (p->mood == MOOD_OFF) return;
    int is_auto;
    int card = card_kBps(&is_auto);
    int need = budget_kBps(live_fmt.width > 0 ? &live_fmt : &p->nominal, &bpp);
    snprintf(buffer, sizeof(buffer), "%d/%dMB/s", need / 1000, card / 1000);
    enum budget_verdict vd = budget_verdict(need, card);
    item->color_fg = vd == BUDGET_CONTINUOUS ? COLOR_GREEN1 : vd == BUDGET_MARGINAL ? COLOR_YELLOW : COLOR_RED;
}

static struct lvinfo_item info_items[] = {
    { .name = "Mothlight budget", .which_bar = LV_BOTTOM_BAR_ONLY, .update = moth_info, .preferred_position = 30 },
};

/* ---------------------------------------------------------------- init */

static void load_profiles(void)
{
    char path[64];
    snprintf(path, sizeof(path), "%sPROFILES.TXT", get_config_dir());
    int size = 0;
    char * raw = (char *)read_entire_file(path, &size);
    if (raw && size > 0)
    {
        char * text = malloc(size + 1);
        if (text)
        {
            memcpy(text, raw, size);
            text[size] = 0;
            int bad = 0;
            profile_count = profiles_parse(text, profiles, MAX_PROFILES, &bad);
            if (bad) printf("[moth] PROFILES.TXT: line %d not understood, skipped\n", bad);
            free(text);
        }
        fio_free(raw);
    }
    profiles_from_file = profile_count > 0;
    if (!profiles_from_file) profile_count = profiles_builtin(profiles, MAX_PROFILES);
}

static unsigned int moth_init(void)
{
    if (!is_camera("EOSM", "2.0.2"))
    {
        printf("[moth] Mothlight is made for the EOS M 2.0.2 only\n");
        return CBR_RET_ERROR;
    }
    for (int i = 0; i < UF_COUNT; i++)
        ui_text_set_font(i, (const struct ui_rbf *)font_dynamic[i].bitmap);
    load_profiles();
    bpp_from_config();
    menu_add("Movie", moth_menu, COUNT(moth_menu));
    lvinfo_add_items(info_items, COUNT(info_items));
    task_create("moth_ui", 0x1e, 0x2000, moth_task, 0);
    return 0;
}

static unsigned int moth_deinit(void)
{
    task_quit = 1;
    return 0;
}

MODULE_INFO_START()
    MODULE_INIT(moth_init)
    MODULE_DEINIT(moth_deinit)
MODULE_INFO_END()

MODULE_CBRS_START()
    MODULE_CBR(CBR_KEYPRESS, moth_keypress_cbr, 0)
    MODULE_CBR(CBR_KEYPRESS_RAW, moth_keypress_raw_cbr, 0)
MODULE_CBRS_END()

MODULE_CONFIGS_START()
    MODULE_CONFIG(cfg_theme)
    MODULE_CONFIG(cfg_key)
    MODULE_CONFIG(cfg_motion)
    MODULE_CONFIG(cfg_card)
    MODULE_CONFIG(cfg_tally)
    MODULE_CONFIG(cfg_active)
    MODULE_CONFIG(cfg_learned)
    MODULE_CONFIG(cfg_bpp10)
    MODULE_CONFIG(cfg_bpp11)
    MODULE_CONFIG(cfg_bpp12)
    MODULE_CONFIG(cfg_bpp14)
MODULE_CONFIGS_END()
