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
static volatile int opening;                /* overlay_begin() in progress (the GUI task must not re-request) */

static int overlay_active;
static volatile uint32_t last_frame_ms;     /* watchdog: the key handler gives keys back if this stops moving */
static volatile int keys_released;          /* watchdog tripped: keys go to Canon until the task cleans up */
static volatile int handing_back;           /* REC / PLAY / shutter pressed: everything passes while we close */

/* While the panel is up, ML must not treat LiveView as idle. On the EOS M its core catches
   DOWN and SET in idle LiveView for a long-press check, and on release redraws the screen:
   it wipes the overlay and paints black letterbox bars (idle_kill_flicker) -- the black flash.
   gui_state is only ML's copy of Canon's GUI state (Canon is not affected); a value Canon never
   uses makes those checks skip. It is put back the moment we close, before any key that needs
   ML's idle handling (REC for MLV Lite) is passed on. If Canon changes state meanwhile, its
   property handler overwrites our value and we leave it alone. */
#define MOTH_GUI_STATE 0x6D
static volatile int saved_gui_state = -1;
static void hold_gui_state(void)
{
    if (gui_state == MOTH_GUI_STATE) return;
    saved_gui_state = gui_state;
    gui_state = MOTH_GUI_STATE;
}
static void release_gui_state(void)
{
    if (gui_state == MOTH_GUI_STATE && saved_gui_state >= 0) gui_state = saved_gui_state;
    saved_gui_state = -1;
}
static uint32_t lv_lost_at;                 /* LiveView went away (Crop Mood restarts it while applying) */
static int saved_global_draw = -1;

static int apply_phase;                     /* 0 idle, 1 start, 2 helper busy, 3 waiting for LiveView */
static uint32_t apply_t0;
static volatile int apply_helper;           /* 1 running, 2 finished */
static const char * volatile apply_err;
static uint32_t repaint_until;              /* repaint every frame until then (after keys, during apply) */
static int was_lv = 1;

static struct rec_format live_fmt;          /* what RAW video is set to now (read at 1 Hz) */
static int was_recording;
static uint32_t rec_t0;
static struct rec_format rec_fmt;
static int tally_drawn;

static volatile int task_quit;

/* ---------------------------------------------------------------- event log
 * What the camera actually sent while the panel was up, and why it closed: written to
 * ML/LOGS/MOTHLOG.TXT after the panel closes (never while recording). Buttons behave
 * differently on the camera than in any simulator; this is how we find out how. */

enum { CLOSE_KEY = 1, CLOSE_KEY_PASS, CLOSE_MENU, CLOSE_REC, CLOSE_PHOTO, CLOSE_DISPLAY, CLOSE_LV,
       CLOSE_WATCHDOG, CLOSE_DONE };
static const char * close_why[] = { "", "closed by a key", "closed by REC/PLAY/shutter", "ML menu opened",
                                    "recording started", "photo mode", "display off", "LiveView gone",
                                    "watchdog (task stalled)", "auto-close after apply" };

struct log_ev { uint32_t t; short code; char kind; signed char a, b; };
#define LOG_N 192
static struct log_ev log_ring[LOG_N];
static volatile unsigned log_n;
static volatile int log_dirty;
static volatile uint32_t closed_at;          /* keep logging buttons for 3 s after the panel closes */
#define LOG_AFTER_CLOSE_MS 3000
static int logging(void) { return overlay_active || get_ms_clock() - closed_at < LOG_AFTER_CLOSE_MS; }

static void mlog(char kind, int code, int a, int b)
{
    unsigned i = log_n++ % LOG_N;
    log_ring[i].t = get_ms_clock();
    log_ring[i].code = (short)code;
    log_ring[i].kind = kind;
    log_ring[i].a = (signed char)a;
    log_ring[i].b = (signed char)b;
}

static int log_line(char * out, int len, const struct log_ev * e)
{
    static const char * pk[] = { "-", "up", "down", "left", "right", "set", "close" };
    const char * k = (e->a >= 0 && e->a <= 6) ? pk[(int)e->a] : (e->a == PK_CLOSE_NOW ? "close-now" : "?");
    switch (e->kind)
    {
        case 'K': return snprintf(out, len, "%7d  button 0x%02x%s%s\n", (int)e->t, e->code & 0xffff,
                                  e->b ? " (fake)" : "", e->a ? "" : "   [panel closed]");
        case 'P': return snprintf(out, len, "%7d  portable key %d  -> panel %s, %s\n", (int)e->t, e->code, k,
                                  e->b ? "passed on to Canon" : "kept");
        case 'O': return snprintf(out, len, "%7d  OPEN  theme %d, %d profiles\n", (int)e->t, e->code, e->a);
        case 'C': return snprintf(out, len, "%7d  CLOSE %s\n", (int)e->t,
                                  e->code >= 0 && e->code < (int)COUNT(close_why) ? close_why[e->code] : "?");
        case 'R': return snprintf(out, len, "%7d  repaint (something drew over the panel)\n", (int)e->t);
        case 'T': return snprintf(out, len, "%7d  theme -> %d\n", (int)e->t, e->code);
        case 'A': return snprintf(out, len, "%7d  apply step %d (%d)\n", (int)e->t, e->code, e->a);
        case 'L': return snprintf(out, len, "%7d  LiveView %s\n", (int)e->t, e->code ? "back" : "gone");
    }
    return 0;
}

static void log_flush(void)
{
    log_dirty = 0;
    int n = log_n < LOG_N ? (int)log_n : LOG_N;
    int cap = n * 72 + 160;
    char * buf = malloc(cap);
    if (!buf) return;
    int len = snprintf(buf, cap, "Mothlight %s (%s) event log, newest last. Times in ms since power on.\n\n",
                       MOTH_UI_VERSION, MOTH_UI_ENGINE);
    for (unsigned i = log_n - n; i != log_n && len < cap - 80; i++)
        len += log_line(buf + len, cap - len, &log_ring[i % LOG_N]);
    FILE * f = FIO_CreateFile("ML/LOGS/MOTHLOG.TXT");
    if (f)
    {
        FIO_WriteFile(f, buf, len);
        FIO_CloseFile(f);
    }
    free(buf);
}

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
    int clock = menu_get_value_from_script("Debug", "SD Overclock");
    view.sd_clock = clock == INT_MIN ? -1 : clock;
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

    /* check first, change after: never leave a profile half applied */
    static const char * preset_entry[] = { "", "Preset:", "Preset: ", "Preset:  " };
    if (menu_get_value_from_script("Movie", "RAW video") == INT_MIN) return "Load mlv_lite in the Modules tab";
    if (menu_get_value_from_script("Movie", "Crop mood") == INT_MIN) return "Load crop_rec in the Modules tab";
    set_menu("Movie", "RAW video", 1);
    set_menu("Movie", "Crop mood", p->mood);
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

/* Crop Mood and set_lv_zoom block for up to a second while LiveView restarts. They run in
   a helper task so the panel keeps repainting over whatever the restart draws. */
static void apply_helper_task(void * arg)
{
    int idx = (int)(intptr_t)arg;
    apply_err = apply_start(&profiles[idx]);
    if (!apply_err) cfg_active = idx;       /* applied, even if the panel was closed meanwhile */
    apply_helper = 2;
}

static void apply_poll(uint32_t now)
{
    const struct rec_profile * p = &profiles[panel.applying];
    char msg[48];

    if (apply_phase == 1)
    {
        if (apply_helper == 1) return;      /* the previous one is still finishing */
        apply_helper = 1;
        apply_err = 0;
        apply_phase = 2;
        apply_t0 = now;
        mlog('A', 1, panel.applying, 0);
        task_create("moth_apply", 0x1e, 0x1000, (void *) apply_helper_task, (void *)(intptr_t)panel.applying);
        return;
    }

    if (apply_phase == 2)
    {
        if (apply_helper != 2) return;
        apply_helper = 0;
        mlog('A', 2, apply_err ? 1 : 0, 0);
        if (apply_err)
        {
            panel_apply_result(&panel, 0, apply_err, now);
            apply_phase = 0;
            return;
        }
        apply_phase = 3;
        apply_t0 = now;
        return;
    }

    /* phase 3: Crop Mood refreshes LiveView on its own; wait for the new frame size */
    if (now - apply_t0 < 600) return;
    if (p->mood == MOOD_OFF)
    {
        panel_apply_result(&panel, 1, "Canon H.264 ready", now);
        apply_phase = 0;
        return;
    }
    if (!lv) return;
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
    key_head = key_tail = 0;                /* nothing from a previous session */
    handing_back = 0;
    repaint_until = get_ms_clock() + 300;
    keys_released = 0;
    lv_lost_at = 0;
    last_frame_ms = get_ms_clock();
    hold_gui_state();
    overlay_active = 1;
    return 1;
}

static void overlay_end(int why)
{
    mlog('C', why, 0, 0);
    log_dirty = 1;
    closed_at = get_ms_clock();
    release_gui_state();
    overlay_active = 0;
    panel.state = PS_CLOSED;
    apply_phase = 0;                        /* an apply cut short is not resumed on the next open */
    keys_released = 0;
    if (saved_global_draw > 0) menu_set_value_from_script("Overlay", "Global Draw", saved_global_draw);
    saved_global_draw = -1;
    /* an instant close leaves the panel's pixels behind; ML's menu repaints itself */
    if (!gui_menu_shown()) clrscr();
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
            opening = 1;
            open_request = 0;
            if (can_show_panel() && overlay_begin())
            {
                fill_view();
                panel_open(&panel, &view, get_ms_clock());
                mlog('O', cfg_theme, profile_count, 0);
                was_lv = lv;
            }
            open_request = 0;               /* an INFO that arrived while starting up is not a new request */
            opening = 0;
            continue;
        }

        if (overlay_active && keys_released)
        {
            /* the watchdog gave the buttons back while we were stuck: tidy up quietly */
            overlay_end(CLOSE_WATCHDOG);
            continue;
        }

        if (overlay_active)
        {
            int k, closed = 0;
            while ((k = pop_key()) != PK_NONE)
            {
                /* ML and Canon may redraw around a key (on the EOS M, DOWN goes through ML's
                   long-press check, which redraws the screen): repaint fully for a moment */
                repaint_until = now + 300;
                if (k == PK_CLOSE_NOW) { overlay_end(CLOSE_KEY_PASS); closed = 1; break; }
                int act = panel_key(&panel, &view, k, now);
                if (act == PA_APPLY) apply_phase = 1;
                if (act == PA_THEME_NEXT || act == PA_THEME_PREV)
                {
                    cfg_theme = (cfg_theme + (act == PA_THEME_NEXT ? 1 : THEME_COUNT - 1)) % THEME_COUNT;
                    view.theme = theme();
                    panel_theme_changed(&panel);
                    mlog('T', cfg_theme, 0, 0);
                }
            }
            if (closed) continue;
            if (apply_phase) { apply_poll(now); repaint_until = now + 300; }

            /* Something took the screen (ML menu, recording, photo mode, display off): give it
               back at once, without drawing again. LiveView itself may blink off while Crop Mood
               reconfigures, so only its long absence counts (longer while applying). */
            if (!lv != !was_lv) { mlog('L', lv ? 1 : 0, 0, 0); was_lv = lv; }
            int why = gui_menu_shown() ? CLOSE_MENU : RECORDING ? CLOSE_REC :
                      !is_movie_mode() ? CLOSE_PHOTO : !display_is_on() ? CLOSE_DISPLAY : 0;
            if (!lv)
            {
                if (!lv_lost_at) lv_lost_at = now ? now : 1;
                if (now - lv_lost_at > (apply_phase ? 6000u : 700u)) why = CLOSE_LV;
            }
            else lv_lost_at = 0;
            if (why) { overlay_end(why); continue; }

            struct present out;
            last_frame_ms = now;
            view.active = cfg_active;
            int was_done = panel.state == PS_DONE;
            int fl = panel_frame(&panel, &view, &back, now, &out);
            if (fl & PF_PRESENTED) present(&out);
            if (panel_settled(&panel))
            {
                if ((int32_t)(repaint_until - now) > 0) present_full();
                else if (screen_was_disturbed()) { present_full(); mlog('R', 0, 0, 0); }
            }
            if (fl & PF_CLOSED) overlay_end(was_done ? CLOSE_DONE : CLOSE_KEY);
            msleep(20);                     /* 50 Hz: a foreign redraw is gone within one frame */
            continue;
        }

        if (log_dirty && !RECORDING && apply_helper != 1 && !logging()) log_flush();

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
    if (overlay_active && (keys_released || get_ms_clock() - last_frame_ms > 2000))
    {
        /* The panel task stopped drawing: never hold the camera's buttons hostage. Only flag
           it here (this is the GUI task); the panel task frees and restores when it wakes. */
        if (!keys_released) { release_gui_state(); canon_gui_enable_front_buffer(1); }
        keys_released = 1;
        return 1;
    }
    if (overlay_active && handing_back) return 1;
    if (overlay_active)
    {
        struct key_route r = moth_route_key(key);
        mlog('P', key, r.panel_key, r.pass_to_canon);
        if (r.panel_key == PK_CLOSE_NOW)
        {
            /* REC / PLAY / shutter: ML must see LiveView idle again before the key goes on
               (MLV Lite only starts RAW recording from idle LiveView). Put the state back,
               swallow this event and send the key again through ML. */
            release_gui_state();
            handing_back = 1;
            push_key(PK_CLOSE_NOW);
            if (key == MODULE_KEY_REC || key == MODULE_KEY_LV || key == MODULE_KEY_PLAY ||
                key == MODULE_KEY_TOUCH_2_FINGER)
            {
                module_send_keypress(key);
                return 0;
            }
            return 1;                       /* shutter: Canon focuses / shoots, not ML's state */
        }
        if (r.panel_key != PK_NONE) push_key(r.panel_key);
        return r.pass_to_canon;
    }

    int trigger = cfg_key == 1 && key == MODULE_KEY_INFO;
    if (trigger && (opening || open_request)) return 0;     /* already on its way */
    if (trigger && lv && !is_movie_mode() && !gui_menu_shown())
    {
        /* INFO stays Canon's in photo mode; say once per session why the panel did not come */
        static int hinted;
        if (!hinted) { hinted = 1; NotifyBox(2000, "Mothlight works in movie mode"); }
        return 1;
    }
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
#define EOSM_Q_SET         0x1d   /* SET's real press code; ML turns it into SET after its long-press check */

static unsigned int moth_keypress_raw_cbr(unsigned int raw)
{
    const struct event * e = (const struct event *) raw;
    if (e && e->type == 0 && e->param >= 0 && logging()) mlog('K', e->param, overlay_active, IS_FAKE(e) ? 1 : 0);
    if (!overlay_active || keys_released || handing_back) return 1;
    if (e && e->type == 0)
        switch (e->param)
        {
            case EOSM_UNPRESS_UP: case EOSM_UNPRESS_RIGHT:
            case EOSM_UNPRESS_LEFT: case EOSM_UNPRESS_DOWN:
                return 0;
            case EOSM_Q_SET:
                /* with LiveView not idle for ML, SET arrives as itself, not as ML's fake SET */
                push_key(PK_SET);
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
                .help = "How the panel looks. Takes effect the next time the panel opens.",
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
    MODULE_CBR(CBR_KEYPRESS_RAW, moth_keypress_raw_cbr, 0)    /* first: it logs every event */
    MODULE_CBR(CBR_KEYPRESS, moth_keypress_cbr, 0)
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
