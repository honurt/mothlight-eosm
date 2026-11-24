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
#include <focus.h>

#include "version.h"
#include "panel.h"
#include "qbar.h"
#include "settings.h"
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
static CONFIG_INT("moth.key",     cfg_key,     1);    /* what INFO opens: 0 nothing, 1 quick bar, 2 profiles */
static CONFIG_INT("moth.motion",  cfg_motion,  1);    /* 1 smooth, 0 off */
static CONFIG_INT("moth.card",    cfg_card,    0);    /* index in card_mb[] (0 = auto) */
static CONFIG_INT("moth.tally",   cfg_tally,   1);    /* 0 off, 1 frame, 2 pulsing frame */
static CONFIG_INT("moth.active",  cfg_active,  -1);
static CONFIG_INT("moth.learned", cfg_learned, 0);    /* bit (bits-10): learned from clips */
static CONFIG_INT("moth.bpp10",   cfg_bpp10,   0);
static CONFIG_INT("moth.bpp11",   cfg_bpp11,   0);
static CONFIG_INT("moth.bpp12",   cfg_bpp12,   0);
static CONFIG_INT("moth.bpp14",   cfg_bpp14,   0);
static CONFIG_INT("moth.welcomed", cfg_welcomed, 0); /* the first-start welcome was shown */
static CONFIG_INT("moth.focus",   cfg_focus,   1);
static CONFIG_INT("moth.prevset", cfg_prevset, 0);    /* the recording preview default was set once */
static CONFIG_INT("moth.clips",   cfg_clips,   1);    /* PLAY (RAW on) opens Mothlight's clip list */    /* Crop Mood x5: 1 tap / half-press focus at x10, 0 Canon's */

static const int card_mb[] = { 0, 30, 40, 50, 60, 65, 70, 75, 80, 90 };

/* ---------------------------------------------------------------- state */

static struct rec_profile profiles[MAX_PROFILES];
static int profile_count;
static int profiles_from_file;

static struct bpp_table bpp;
static struct panel panel;
static struct panel_view view;

/* the overlay shows one of two things */
enum { OV_PANEL = 1, OV_BAR = 2, OV_SETTINGS = 3 };
static int ov_mode;
static struct qbar qbar;
static struct qbar_view qview;
static uint32_t qview_read_at;
static struct settings sets;
static struct settings_view sview;
static int settings_from_bar;
static int welcome_mode;                    /* the settings page shows the welcome / setup check */
static int welcome_from_settings;           /* ... opened from Settings: back goes to Settings */
static int welcome_tried;                   /* the automatic first-start welcome was opened this boot */
static int welcome_mark_pending;            /* seen: write the marker file once idle (never while recording) */
static int clips_mode;                      /* the settings page shows the clip list (1), a clip (2), delete? (3) */
static volatile int clips_resent;           /* a PLAY we sent on ourselves: let it through once */
static int clips_play_pending;              /* 1 play the chosen clip, 2 Canon's playback */
static int bar_hint_seen;                   /* after the first time, the bar's strip shows warnings */

/* focus punch-in: x10 from the bar, back to where we were afterwards */
static volatile int zoom_job;               /* a zoom change is running in its helper task */
static int zoom_before;                     /* lv_dispsize before the punch-in; 0 = not zoomed by us */
static int zoom_restore;                    /* the overlay closed while punched in: zoom back when idle */
static volatile uint32_t rec_after_zoom;    /* REC pressed while punched in (time): record once zoomed back */
static int panel_from_bar;                  /* the profile list was opened from the quick bar: go back to it */
static volatile int tap_pending, tap_x, tap_y;   /* a tap on the quick bar (touch position read from RAM) */

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

struct log_ev { uint32_t t; short code; char kind; signed char a, b; uint32_t v; };
#define LOG_N 192
static struct log_ev log_ring[LOG_N];
static volatile unsigned log_n;
static volatile int log_dirty;
static volatile uint32_t closed_at;          /* keep logging buttons for 3 s after the panel closes */
#define LOG_AFTER_CLOSE_MS 3000
static int logging(void) { return overlay_active || get_ms_clock() - closed_at < LOG_AFTER_CLOSE_MS; }

static void mlog_v(char kind, int code, int a, int b, uint32_t v);
static void mlog(char kind, int code, int a, int b) { mlog_v(kind, code, a, b, 0); }
static void mlog_v(char kind, int code, int a, int b, uint32_t v)
{
    unsigned i = log_n++ % LOG_N;
    log_ring[i].v = v;
    log_ring[i].t = get_ms_clock();
    log_ring[i].code = (short)code;
    log_ring[i].kind = kind;
    log_ring[i].a = (signed char)a;
    log_ring[i].b = (signed char)b;
}

static int log_line(char * out, int len, const struct log_ev * e)
{
    static const char * pk[] = { "-", "up", "down", "left", "right", "set", "close", "dial-", "dial+" };
    const char * k = (e->a >= 0 && e->a <= 8) ? pk[(int)e->a] : (e->a == PK_CLOSE_NOW ? "close-now" : "?");
    switch (e->kind)
    {
        case 'K': return snprintf(out, len, "%7d  button 0x%02x%s%s\n", (int)e->t, e->code & 0xffff,
                                  e->b ? " (fake)" : "", e->a ? "" : "   [panel closed]");
        case 'P': return snprintf(out, len, "%7d  portable key %d  -> panel %s, %s\n", (int)e->t, e->code, k,
                                  e->b ? "passed on to Canon" : "kept");
        case 'O': return snprintf(out, len, "%7d  OPEN  %s, theme %d, %d profiles\n", (int)e->t,
                                  e->b == OV_BAR ? "quick bar" : e->b == OV_SETTINGS ? "settings" : "profiles", e->code, e->a);
        case 'C': return snprintf(out, len, "%7d  CLOSE %s\n", (int)e->t,
                                  e->code >= 0 && e->code < (int)COUNT(close_why) ? close_why[e->code] : "?");
        case 'R': return snprintf(out, len, "%7d  repaint (something drew over the panel)\n", (int)e->t);
        case 'T': return snprintf(out, len, "%7d  theme -> %d\n", (int)e->t, e->code);
        case 'A': return snprintf(out, len, "%7d  apply step %d (%d)\n", (int)e->t, e->code, e->a);
        case 'L': return snprintf(out, len, "%7d  LiveView %s\n", (int)e->t, e->code ? "back" : "gone");
        case 'X': return snprintf(out, len, "%7d  touch 0x%02x  pos 0x%08x  -> x %d y %d%s\n", (int)e->t, e->code & 0xffff,
                                  (int)e->v, (int)(e->v & 0xffff), (int)(e->v >> 16), e->a ? "  (used)" : "");
        case 'Y': return snprintf(out, len, "%7d  touch 0x%02x  obj 0x%08x arg 0x%x\n", (int)e->t, e->code & 0xffff,
                                  (int)e->v, e->a);
        case 'F': return snprintf(out, len, "%7d  focus: step %d  %d %d\n", (int)e->t, e->code, e->a, e->b);
        case 'S': return snprintf(out, len, "%7d  settings: item %d%s\n", (int)e->t, e->code, e->a ? " (failed)" : "");
        case 'E': return snprintf(out, len, "%7d  quick bar: setting %d %s, %s\n", (int)e->t, e->code,
                                  e->a > 0 ? "up" : "down", e->b ? "changed" : "unchanged");
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

/* While recording, Crop Mood stops correcting Canon's preview: "Canon (live)" then shows the
   real x5 magnification (tighter than the clip, with vertical lines). ML's framing preview shows
   exactly what is recorded. And MLV Lite's "Preview toggle" swaps the screen on every long
   half-press (the flash on autofocus): off. The preview choice is set once; after that it is
   the user's (Settings > Screen while recording RAW). */
static void recording_preview_defaults(void)
{
    if (menu_get_value_from_script("RAW video", "Preview toggle") > 0)
        menu_set_value_from_script("RAW video", "Preview toggle", 0);
    if (!cfg_prevset && menu_get_value_from_script("RAW video", "Preview") != INT_MIN)
    {
        menu_set_value_from_script("RAW video", "Preview", 2);       /* ML framing */
        cfg_prevset = 1;
    }
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
    recording_preview_defaults();

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

/* ---------------------------------------------------------------- quick bar: the camera side */

static void copy_str(char * dst, int len, const char * src) { snprintf(dst, len, "%s", src ? src : ""); }

static const char * wb_name(int mode)
{
    switch (mode)
    {
        case WB_AUTO:        return "AWB";
        case WB_SUNNY:       return "Daylight";
        case WB_SHADE:       return "Shade";
        case WB_CLOUDY:      return "Cloudy";
        case WB_TUNGSTEN:    return "Tungsten";
        case WB_FLUORESCENT: return "Fluor.";
        case WB_FLASH:       return "Flash";
        case WB_CUSTOM:      return "Custom";
        default:             return "WB";
    }
}

/* what each setting shows; read from ML's lens_info (Canon's own properties) */
struct warning { char text[72]; int bad; };
static int collect_warnings(struct warning * w, int max);
static void start_zoom(int z);

static void qb_fill(struct qbar_view * q)
{
    memset(q->item, 0, sizeof(q->item));
    q->theme = theme();
    q->reduced_motion = !cfg_motion;

    struct qb_item_view * it = &q->item[QB_SHUTTER];
    if (lens_info.raw_shutter)
    {
        int s = get_current_shutter_reciprocal_x1000();
        int fps = fps_get_current_x1000();
        copy_str(it->value, sizeof(it->value), lens_format_shutter_reciprocal(s, 3));
        int deg = s > 0 ? (3600 * (fps / 10) / (s / 10) + 5) / 10 : 0;   /* shutter angle */
        snprintf(it->sub, sizeof(it->sub), "%d deg", deg);   /* green near 180: natural motion */
        it->sub_tone = deg >= 170 && deg <= 190 ? QT_OK : (deg < 45 || deg > 300) ? QT_EDGE : QT_DIM;
    }
    else
    {
        copy_str(it->value, sizeof(it->value), "Auto");
        copy_str(it->sub, sizeof(it->sub), "by Canon");
        it->locked = 1;
    }

    it = &q->item[QB_APERTURE];
    if (!lens_info.lens_exists)
    {
        copy_str(it->value, sizeof(it->value), "Lens");
        copy_str(it->sub, sizeof(it->sub), "manual lens");
        it->locked = 1;
    }
    else if (!lens_info.raw_aperture)
    {
        copy_str(it->value, sizeof(it->value), "Auto");
        copy_str(it->sub, sizeof(it->sub), "by Canon");
        it->locked = 1;
    }
    else
    {
        char lo[12], hi[12];
        copy_str(it->value, sizeof(it->value), lens_format_aperture(lens_info.raw_aperture));
        copy_str(lo, sizeof(lo), lens_format_aperture(lens_info.raw_aperture_min));
        copy_str(hi, sizeof(hi), lens_format_aperture(lens_info.raw_aperture_max));
        snprintf(it->sub, sizeof(it->sub), "%s-%s", lo, hi[0] == 'f' && hi[1] == '/' ? hi + 2 : hi);
    }

    it = &q->item[QB_ISO];
    if (lens_info.raw_iso)
    {
        copy_str(it->value, sizeof(it->value), lens_format_iso(lens_info.raw_iso));
        copy_str(it->sub, sizeof(it->sub), "manual");
    }
    else
    {
        copy_str(it->value, sizeof(it->value), "Auto");
        copy_str(it->sub, sizeof(it->sub), "dial: manual");
        it->sub_tone = QT_ACCENT;
    }

    it = &q->item[QB_WB];
    if (lens_info.wb_mode == WB_KELVIN)
    {
        snprintf(it->value, sizeof(it->value), "%dK", (int)lens_info.kelvin);
        copy_str(it->sub, sizeof(it->sub), "Kelvin");
    }
    else
    {
        copy_str(it->value, sizeof(it->value), wb_name(lens_info.wb_mode));
        copy_str(it->sub, sizeof(it->sub), "dial: Kelvin");
        it->sub_tone = QT_ACCENT;
    }

    it = &q->item[QB_FOCUS];
    snprintf(it->value, sizeof(it->value), "x%d", lv_dispsize > 0 ? lv_dispsize : 1);
    if (lv_dispsize == 10) { copy_str(it->sub, sizeof(it->sub), "DOWN back"); it->sub_tone = QT_ACCENT; }
    else                   { copy_str(it->sub, sizeof(it->sub), "UP x10"); }
    if (zoom_job) { copy_str(it->sub, sizeof(it->sub), "..."); }

    q->battery_bars = battery_level_bars;

    it = &q->item[QB_PROFILE];
    if (cfg_active >= 0 && cfg_active < profile_count)
    {
        const struct rec_profile * p = &profiles[cfg_active];
        copy_str(it->value, sizeof(it->value), p->name);
        if (p->mood == MOOD_OFF) { copy_str(it->sub, sizeof(it->sub), "Canon H.264"); }
        else
        {
            int is_auto;
            int card = card_kBps(&is_auto);
            int need = budget_kBps(live_fmt.width > 0 ? &live_fmt : &p->nominal, &bpp);
            enum budget_verdict vd = budget_verdict(need, card);
            int mins = budget_minutes_on_card(need, free_MB());
            const char * w = vd == BUDGET_CONTINUOUS ? "OK" : vd == BUDGET_MARGINAL ? "EDGE" : "BURST";
            if (mins >= 0) snprintf(it->sub, sizeof(it->sub), "%s  ~%d min", w, mins);
            else           snprintf(it->sub, sizeof(it->sub), "%s", w);
            it->sub_tone = vd == BUDGET_CONTINUOUS ? QT_OK : vd == BUDGET_MARGINAL ? QT_EDGE : QT_BURST;
        }
    }
    else
    {
        copy_str(it->value, sizeof(it->value), "Choose");
        copy_str(it->sub, sizeof(it->sub), "UP/DOWN: list");
        it->sub_tone = QT_ACCENT;
    }
}

/* one dial click. ML's own steppers: Canon's valid steps, the lens's range, no wrap-around
   (priv -1, the "shortcut" mode Crop Mood uses for its ISO / aperture buttons). They wait
   for Canon to confirm, so they run here in the panel's task, never in the GUI task. */
static void qb_adjust(int item, int sign, uint32_t now)
{
    int before, after;
    switch (item)
    {
        case QB_SHUTTER:  before = lens_info.raw_shutter;  shutter_toggle((void *)-1, sign);  after = lens_info.raw_shutter;  break;
        case QB_APERTURE: before = lens_info.raw_aperture; aperture_toggle((void *)-1, sign); after = lens_info.raw_aperture; break;
        case QB_ISO:      before = lens_info.raw_iso;      iso_toggle((void *)-1, sign);      after = lens_info.raw_iso;      break;
        case QB_FOCUS:
            if (zoom_job) return;
            if (sign > 0 && lv_dispsize != 10)
            {
                zoom_before = lv_dispsize > 0 ? lv_dispsize : 1;
                start_zoom(10);
                qbar_toast(&qbar, "x10: focus by hand, or half-press for autofocus", 0, now);
            }
            else if (sign < 0 && lv_dispsize == 10)
            {
                start_zoom(zoom_before ? zoom_before : 5);
                zoom_before = 0;
            }
            else qbar_toast(&qbar, lv_dispsize == 10 ? "Already at x10" : "Not zoomed in", 0, now);
            mlog('E', item, sign, 1);
            return;
        case QB_WB:
            before = lens_info.wb_mode == WB_KELVIN ? (int)lens_info.kelvin : -1;
            kelvin_toggle((void *)-1, sign);
            after = lens_info.wb_mode == WB_KELVIN ? (int)lens_info.kelvin : -1;
            break;
        default: return;
    }
    mlog('E', item, sign, before == after ? 0 : 1);
    if (before == after)
        qbar_toast(&qbar, item == QB_ISO && !lens_info.raw_shutter ? "ISO is automatic: set Movie exposure to Manual (Canon menu)"
                                                                   : "That is the limit", 0, now);
}

static const char * qb_locked_reason(int item)
{
    if (item == QB_APERTURE && !lens_info.lens_exists) return "Manual lens: set the aperture on the lens";
    return "Automatic: in Canon's menu set Movie exposure to Manual";
}

/* ---------------------------------------------------------------- the welcome's marker

   ML writes module settings to the card only on a clean power-off; after a battery pull the
   welcome came back. A tiny file written as soon as it was seen survives that. */

static void welcome_marker_path(char * p, int len) { snprintf(p, len, "%sMOTHWEL.TXT", get_config_dir()); }

static int welcome_marker_exists(void)
{
    char p[64]; uint32_t size = 0;
    welcome_marker_path(p, sizeof(p));
    return FIO_GetFileSize(p, &size) == 0;
}

static void welcome_marker_write(void)
{
    char p[64];
    welcome_marker_path(p, sizeof(p));
    FILE * f = FIO_CreateFile(p);
    if (!f) return;
    static const char text[] = "Mothlight: the welcome page was seen. Delete this file to see it again.\n";
    FIO_WriteFile(f, text, sizeof(text) - 1);
    FIO_CloseFile(f);
}

/* ---------------------------------------------------------------- smart warnings */

static void load_profiles(void);
static void learned_reset_now(void);


/* what could spoil a take, most serious first */
static int collect_warnings(struct warning * w, int max)
{
    int n = 0;
#define WARN(bad_, ...) do { if (n < max) { snprintf(w[n].text, sizeof(w[n].text), __VA_ARGS__); w[n].bad = bad_; n++; } } while (0)
    int is_auto;
    int card = card_kBps(&is_auto);
    int sd = menu_get_value_from_script("Debug", "SD Overclock");
    int sound = menu_get_value_from_script("RAW video", "Sound recording");
    int raw_on = menu_get_value_from_script("Movie", "RAW video");
    const struct rec_profile * p = (cfg_active >= 0 && cfg_active < profile_count) ? &profiles[cfg_active] : 0;
    int raw_profile = p && p->mood != MOOD_OFF;
    enum budget_verdict vd = BUDGET_CONTINUOUS;
    int mins = -1;
    if (raw_profile)
    {
        int need = budget_kBps(live_fmt.width > 0 ? &live_fmt : &p->nominal, &bpp);
        vd = budget_verdict(need, card);
        mins = budget_minutes_on_card(need, free_MB());
    }

    if (battery_level_bars == 0)          WARN(1, "Battery low");
    if (raw_profile && raw_on == 0)       WARN(1, "RAW video is off: apply the profile again");
    if (!lens_info.raw_shutter)           WARN(1, "Movie exposure is Auto: set Manual in Canon's menu");
    if (raw_profile && mins >= 0 && mins < 3) WARN(1, "Card almost full: ~%d min left", mins);
    if (raw_profile && is_auto && sd == 0 && vd != BUDGET_CONTINUOUS)
                                          WARN(0, "SD Overclock is off: the card runs at ~%d MB/s", card / 1000);
    if (raw_profile && sound > 0 && (sd == 3 || vd != BUDGET_CONTINUOUS))
                                          WARN(0, "Sound recording is on: it can stop RAW at this rate");
    if (raw_profile && vd == BUDGET_BURST) WARN(0, "%s records in short bursts on this card", p->name);
#undef WARN
    return n;
}

/* ---------------------------------------------------------------- focus punch-in */

static void zoom_task(void * arg)
{
    int z = (int)(intptr_t)arg;
    if (z == 5) { set_lv_zoom(1); set_lv_zoom(5); }  /* via x1, as Crop Mood does (x10 -> x5 can go black) */
    else set_lv_zoom(z);
    zoom_job = 0;
}

static void start_zoom(int z)
{
    if (zoom_job) return;
    zoom_job = 1;
    task_create("moth_zoom", 0x1e, 0x1000, (void *) zoom_task, (void *)(intptr_t)z);
}

/* once idle: back to the zoom from before the punch-in (also if the x10 is still being set) */
static void zoom_restore_step(void)
{
    if (!zoom_restore || zoom_job) return;
    if (lv_dispsize == 10 && lv) start_zoom(zoom_before ? zoom_before : 5);
    zoom_before = 0;
    zoom_restore = 0;
}

/* ---------------------------------------------------------------- focus at x10

   Crop Mood runs every preset in Canon's x5 mode and hides Canon's layer there; its author
   notes that autofocus in x5 "won't give accurate results" (the modified preview breaks the
   AF data) and that x10 is where focusing works. And a tap in x5 moves Canon's magnified
   area, i.e. the recorded window, instead of a focus point. So in those presets Mothlight
   takes taps and half-presses (never while recording) and does it the reliable way:

     move Canon's AF point to the tapped spot -> x10 there, Canon's layer on (its real focus
     box) -> autofocus (ML's SW1) -> hold to look -> AF point back to the centre -> x5

   The recorded framing ends where it was: the AF point is centred again before x5. */

enum { FJ_IDLE, FJ_ZOOM, FJ_AF, FJ_HOLD, FJ_BACK };
enum { FOCUS_CANON = 0, FOCUS_BOX = 1, FOCUS_X10 = 2 };

/* the focus box: Canon draws one at x1 and lets a tap move it; Crop Mood hides Canon's layer in
   x5, so Mothlight draws it -- where Canon's AF point really is, with the result's colour */
enum { FB_IDLE, FB_FOCUSING, FB_OK, FB_FAIL };
static volatile int fb_result = FB_IDLE;
static volatile uint32_t fb_result_until;
#define FB_W 76
#define FB_H 56
static volatile int fj_state;              /* the focus job's phase (FJ_IDLE: none) */
static volatile int fj_from_half;          /* started by half-press: hold while it is held */
static volatile int fj_mode;               /* FOCUS_BOX (AF where tapped, no zoom) or FOCUS_X10 */
static volatile int fj_half_held;
static volatile int fj_tap_x = -1, fj_tap_y;
static volatile int fj_cancel;             /* a key asked to finish early */
static volatile uint32_t fj_rec_after;     /* REC pressed meanwhile: record once back at x5 */
static volatile int fj_resent;             /* a key we sent on ourselves (it comes back once) */
static int fj_moved_dx, fj_moved_dy;       /* how far the AF point was moved (undone before x5) */
static int fj_focused;                     /* the last AF confirmed focus */

static int focus_takes_over(void)
{
    if (cfg_focus == FOCUS_CANON || !lv || RECORDING || !is_movie_mode() || gui_menu_shown() || overlay_active) return 0;
    if (!lens_info.lens_exists || is_manual_focus()) return 0;      /* MF: Crop Mood's own x10 key */
    int mood = menu_get_value_from_script("Movie", "Crop mood");
    return mood != INT_MIN && mood > 0 && lv_dispsize == 5;
}

/* Where the x5 preview's picture sits on Canon's AF coordinates (the whole sensor): the
   recorded window, centred, across the screen's width; its height follows the frame */
struct af_geo { int W, H, span_x, span_y, disp_h; };

static int af_geometry(struct af_geo * g)
{
    g->W = g->H = 0;
    get_afframe_sensor_res(&g->W, &g->H);
    if (g->W <= 0 || g->H <= 0) return 0;
    const struct rec_profile * p = (cfg_active >= 0 && cfg_active < profile_count) ? &profiles[cfg_active] : 0;
    int w = live_fmt.width > 0 ? live_fmt.width : p ? p->nominal.width : 0;
    int h = live_fmt.height > 0 ? live_fmt.height : p ? p->nominal.height : 0;
    if (w <= 0 || h <= 0) return 0;
    int mood = p ? p->mood : MOOD_1X1;
    g->span_x = w * (mood == MOOD_1X1 ? 1 : 3);         /* 1x3 and 3x3 bin columns by 3 */
    g->span_y = h * (mood == MOOD_3X3 ? 3 : 1);
    if (g->span_x > g->W) g->span_x = g->W;
    if (g->span_y > g->H) g->span_y = g->H;
    g->disp_h = UI_W * g->span_y / g->span_x;             /* the picture's height on screen */
    if (g->disp_h > UI_H) g->disp_h = UI_H;
    if (g->disp_h < 1) g->disp_h = 1;
    return 1;
}

/* screen point -> how far Canon's AF point must move */
static void tap_to_afframe(int x, int y, int * dx, int * dy)
{
    struct af_geo g;
    int cx, cy;
    *dx = *dy = 0;
    if (!af_geometry(&g)) return;
    get_afframe_pos(g.W, g.H, &cx, &cy);
    int tx = g.W / 2 + (x - UI_W / 2) * g.span_x / UI_W;
    int ty = g.H / 2 + (y - UI_H / 2) * g.span_y / g.disp_h;
    *dx = tx - cx;
    *dy = ty - cy;
    mlog_v('F', 9, x, y, (uint32_t)(g.W << 16 | (g.H & 0xffff)));
}

/* Canon's AF point -> screen (the centre of the focus box); 0 if unknown / off the picture */
static int afframe_to_screen(int * x, int * y)
{
    struct af_geo g;
    int cx, cy;
    if (!af_geometry(&g)) return 0;
    get_afframe_pos(g.W, g.H, &cx, &cy);
    *x = UI_W / 2 + (cx - g.W / 2) * UI_W / g.span_x;
    *y = UI_H / 2 + (cy - g.H / 2) * g.disp_h / g.span_y;
    return *x >= 0 && *x < UI_W && *y >= 0 && *y < UI_H;
}

static void focus_task(void * arg)
{
    int start_zoom_level = lv_dispsize;
    /* 1. the AF point to the tapped spot (in x5: Canon moves its magnified area with it) */
    fj_moved_dx = fj_moved_dy = 0;
    if (fj_tap_x >= 0)
    {
        int dx, dy;
        tap_to_afframe(fj_tap_x, fj_tap_y, &dx, &dy);
        if (dx || dy) { move_lv_afframe(dx, dy); fj_moved_dx = dx; fj_moved_dy = dy; }
        mlog('F', 1, dx, dy);
    }
    if (fj_mode == FOCUS_BOX)
    {
        /* Canon's way: autofocus right there, no zoom; the box shows how it went */
        fj_state = FJ_AF;
        fb_result = FB_FOCUSING;
        fj_focused = 0;
        SW1(1, 50);
        uint32_t t0 = get_ms_clock();
        while (!fj_cancel && get_ms_clock() - t0 < 2000 && !RECORDING)
        {
            if (get_focus_confirmation()) { fj_focused = 1; break; }
            msleep(40);
        }
        SW1(0, 50);
        fb_result = fj_focused ? FB_OK : FB_FAIL;
        fb_result_until = get_ms_clock() + 1500;
        mlog('F', 3, fj_focused, 1);
        fj_moved_dx = fj_moved_dy = 0;      /* the AF point stays where tapped, as Canon's does */
        fj_tap_x = -1;
        fj_cancel = 0;
        fj_state = FJ_IDLE;
        return;
    }
    /* 2. x10 there, with Canon's layer on: its real focus box */
    fj_state = FJ_ZOOM;
    if (!fj_cancel) set_lv_zoom(10);
    if (lv_dispsize == 10) canon_gui_enable_front_buffer(0);
    mlog('F', 2, lv_dispsize, 0);
    /* 3. autofocus, the way ML does it (half-shutter switch), until Canon confirms */
    fj_state = FJ_AF;
    fj_focused = 0;
    if (!fj_cancel && lv_dispsize == 10)
    {
        SW1(1, 50);
        uint32_t t0 = get_ms_clock();
        while (!fj_cancel && get_ms_clock() - t0 < 2000)
        {
            if (get_focus_confirmation()) { fj_focused = 1; break; }
            msleep(40);
        }
        msleep(fj_focused ? 150 : 0);       /* let the lens settle */
        SW1(0, 50);
    }
    mlog('F', 3, fj_focused, 0);
    /* 4. look: while the half-press is held, or a moment after a tap */
    fj_state = FJ_HOLD;
    {
        uint32_t t0 = get_ms_clock();
        /* a lost half-press release (ML's menu opened meanwhile, the display went off ...)
           must not leave the camera at x10: those end it too, and 30 s at most */
        while (!fj_cancel && lv && !RECORDING && !gui_menu_shown() && is_movie_mode() && display_is_on())
        {
            if (fj_from_half ? !fj_half_held : get_ms_clock() - t0 > 1200) break;
            if (get_ms_clock() - t0 > 30000) break;
            msleep(30);
        }
    }
    /* 5. AF point back to the centre, then x5 (via x1, as Crop Mood does) */
    fj_state = FJ_BACK;
    if (fj_moved_dx || fj_moved_dy) { move_lv_afframe(-fj_moved_dx, -fj_moved_dy); fj_moved_dx = fj_moved_dy = 0; }
    if (lv_dispsize != start_zoom_level) { set_lv_zoom(1); set_lv_zoom(start_zoom_level == 10 ? 5 : start_zoom_level); }
    mlog('F', 4, lv_dispsize, fj_cancel);
    fj_tap_x = -1;
    fj_cancel = 0;
    if (!overlay_active && fj_mode == FOCUS_X10) release_gui_state();
    fj_state = FJ_IDLE;
}

static void focus_start(int x, int y, int from_half)
{
    if (fj_state != FJ_IDLE || zoom_job) return;
    fj_mode = cfg_focus == FOCUS_X10 ? FOCUS_X10 : FOCUS_BOX;
    fj_tap_x = x; fj_tap_y = y;
    fj_from_half = from_half;
    fj_half_held = from_half;
    fj_cancel = 0;
    fj_state = FJ_ZOOM;
    /* x10: ML's GUI state busy, as with the bar: MLV Lite would otherwise start RAW on REC at x10,
       and ML's own long-press checks would redraw over Canon's x10 view. The box job is short
       and stays in x5: REC and everything else go on as usual. */
    if (fj_mode == FOCUS_X10) hold_gui_state();
    task_create("moth_focus", 0x1e, 0x1000, (void *) focus_task, 0);
}

/* ---------------------------------------------------------------- drawing the focus box */

static struct { int shown, x, y, color, thick; } fb_drawn;

static void fb_outline(uint8_t * bmp, int cx, int cy, int thick, int color, int erase)
{
    int x0 = cx - FB_W / 2, y0 = cy - FB_H / 2;
    for (int t = 0; t < thick; t++)
        for (int side = 0; side < 4; side++)
        {
            int len = side < 2 ? FB_W : FB_H;
            for (int i = 0; i < len; i++)
            {
                int x = side == 0 || side == 1 ? x0 + i : side == 2 ? x0 + t : x0 + FB_W - 1 - t;
                int y = side == 0 ? y0 + t : side == 1 ? y0 + FB_H - 1 - t : y0 + i;
                if (x < 0 || x >= UI_W || y < 0 || y >= UI_H) continue;
                uint8_t * px = bmp + y * BMPPITCH + x;
                if (erase) { if (*px == color) *px = COLOR_EMPTY; }    /* only our own pixels */
                else *px = color;
            }
        }
}

static int fb_intact(uint8_t * bmp)
{
    int x0 = fb_drawn.x - FB_W / 2, y0 = fb_drawn.y - FB_H / 2;
    int probes[4][2] = { { x0 + 3, y0 }, { x0 + FB_W - 4, y0 + FB_H - 1 }, { x0, y0 + FB_H / 2 }, { x0 + FB_W - 1, y0 + 5 } };
    for (int i = 0; i < 4; i++)
    {
        int x = probes[i][0], y = probes[i][1];
        if (x < 0 || x >= UI_W || y < 0 || y >= UI_H) continue;
        if (bmp[y * BMPPITCH + x] != fb_drawn.color) return 0;
    }
    return 1;
}

static void focus_box_erase(void)
{
    uint8_t * bmp = bmp_vram_real();
    if (fb_drawn.shown && bmp) fb_outline(bmp, fb_drawn.x, fb_drawn.y, fb_drawn.thick, fb_drawn.color, 1);
    fb_drawn.shown = 0;
}

static int focus_box_wanted(void)
{
    if (cfg_focus != FOCUS_BOX || overlay_active || !lv || !is_movie_mode() || gui_menu_shown() || !display_is_on()) return 0;
    if (!lens_info.lens_exists || is_manual_focus()) return 0;
    int mood = menu_get_value_from_script("Movie", "Crop mood");
    return mood != INT_MIN && mood > 0 && lv_dispsize == 5;
}

/* every idle loop (~10 Hz): show, move, recolour or repair the box */
static void focus_box_step(uint32_t now)
{
    uint8_t * bmp = bmp_vram_real();
    int x, y;
    if (!bmp || !focus_box_wanted() || !afframe_to_screen(&x, &y)) { focus_box_erase(); return; }
    x = COERCE(x, FB_W / 2, UI_W - FB_W / 2 - 1);
    y = COERCE(y, FB_H / 2 + 2, UI_H - FB_H / 2 - 3);

    int state = fb_result;
    if ((state == FB_OK || state == FB_FAIL) && (int32_t)(now - fb_result_until) > 0) state = fb_result = FB_IDLE;
    if (state == FB_IDLE && get_halfshutter_pressed()) state = get_focus_confirmation() ? FB_OK : FB_FOCUSING;
    int color = state == FB_OK ? COLOR_GREEN1 : state == FB_FAIL ? COLOR_RED : COLOR_WHITE;
    int thick = state == FB_FOCUSING ? 3 : 2;

    if (fb_drawn.shown && fb_drawn.x == x && fb_drawn.y == y && fb_drawn.color == color && fb_drawn.thick == thick)
    {
        if (!fb_intact(bmp)) fb_outline(bmp, x, y, thick, color, 0);    /* something drew over it */
        return;
    }
    focus_box_erase();
    fb_outline(bmp, x, y, thick, color, 0);
    fb_drawn.shown = 1; fb_drawn.x = x; fb_drawn.y = y; fb_drawn.color = color; fb_drawn.thick = thick;
}

/* ---------------------------------------------------------------- saving the current setup */

static int menu_int(const char * menu, const char * entry, int lo, int hi)
{
    int v = menu_get_value_from_script(menu, entry);
    return (v == INT_MIN || v < lo || v > hi) ? -1 : v;
}

static void save_current_profile(char * msg, int len, int * bad)
{
    *bad = 1;
    int mood = menu_int("Movie", "Crop mood", 0, 3);
    if (mood < 0) { snprintf(msg, len, "Load crop_rec in the Modules tab"); return; }
    struct rec_profile p;
    memset(&p, 0, sizeof(p));
    p.mood = mood;
    if (mood == MOOD_OFF) { p.bits = -1; }
    else
    {
        static const char * preset_entry[] = { "", "Preset:", "Preset: ", "Preset:  " };
        static const int preset_max[] = { 0, 5, 2, 1 };
        p.preset = menu_int("Crop mood", preset_entry[mood], 0, preset_max[mood]);
        p.ratio  = menu_int("Crop mood", "Aspect ratio:", 0, 4);
        p.fps    = menu_int("Crop mood", "Framerate:", 0, 2);
        p.bits   = menu_int("Crop mood", "Bit-depth", 0, 3);
        if (p.preset < 0 || p.ratio < 0 || p.fps < 0 || p.bits < 0)
        {
            snprintf(msg, len, "Could not read Crop Mood's settings");
            return;
        }
    }
    profile_fill_nominal(&p);
    for (int i = 0; i < profile_count; i++)
        if (profile_same_setup(&profiles[i], &p))
        {
            cfg_active = i;
            *bad = 0;
            snprintf(msg, len, "Already saved as \"%s\"", profiles[i].name);
            return;
        }
    if (profile_count >= MAX_PROFILES) { snprintf(msg, len, "%d profiles is the most: edit PROFILES.TXT", MAX_PROFILES); return; }
    profile_auto_name(&p, p.name, sizeof(p.name));

    /* the whole list goes to PROFILES.TXT (it replaces the built-in list once it exists) */
    int cap = (profile_count + 1) * 96 + 200;
    char * text = malloc(cap);
    if (!text) { snprintf(msg, len, "Out of memory"); return; }
    int n = snprintf(text, cap, "# Mothlight recording profiles: name | mode | preset | ratio | fps | bits\n"
                                "# Saved from the camera. See docs/PROFILES.md.\n");
    for (int i = 0; i < profile_count && n < cap - 96; i++) n += profile_format_line(&profiles[i], text + n, cap - n);
    n += profile_format_line(&p, text + n, cap - n);
    char path[64];
    snprintf(path, sizeof(path), "%sPROFILES.TXT", get_config_dir());
    FILE * f = FIO_CreateFile(path);
    if (f) { FIO_WriteFile(f, text, n); FIO_CloseFile(f); }
    free(text);
    if (!f) { snprintf(msg, len, "Could not write PROFILES.TXT"); return; }

    load_profiles();
    for (int i = 0; i < profile_count; i++)
        if (profile_same_setup(&profiles[i], &p)) { cfg_active = i; break; }
    *bad = 0;
    snprintf(msg, len, "Saved as \"%s\"", p.name);
}

/* ---------------------------------------------------------------- the settings page */

enum { SI_THEME = 1, SI_MOTION, SI_INFO, SI_TALLY, SI_SDCLOCK, SI_CARD, SI_SOUND, SI_HACKS,
       SI_OVERLAYS, SI_ZEBRAS, SI_PEAK, SI_FALSECOLOR, SI_SAVE, SI_LEARNED, SI_WELCOME, SI_FOCUS, SI_PREVIEW,
       SI_WELCOME_DONE, SI_CHECK_CROP, SI_CHECK_RAW, SI_CHECK_SD, SI_CHECK_EXPO, SI_CHECK_SOUND,
       SI_CANON_PLAY, SI_CLIP_PLAY, SI_CLIP_DELETE, SI_CLIP_BACK, SI_DEL_KEEP, SI_DEL_YES, SI_CLIPS_RESCAN,
       SI_CLIP_BASE = 100 };

static const char * theme_names[] = { "Mothlight", "Stock", "Studio", "Amber", "Signal", "Material",
                                      "Soft", "Glass", "Daylight", "Night", "Mono" };

static void add_row(struct settings_view * v, int kind, int id, int tone, const char * label, const char * value, const char * help)
{
    if (v->count >= SET_MAX_ROWS) return;
    struct set_row * r = &v->row[v->count++];
    r->kind = kind; r->id = id; r->tone = tone; r->help = help;
    snprintf(r->label, sizeof(r->label), "%s", label);
    snprintf(r->value, sizeof(r->value), "%s", value ? value : "");
}

/* an ML menu value as a word, or "module not loaded" */
static void ml_value(char * buf, int len, const char * menu, const char * entry, const char * const * words, int n, int * tone)
{
    int v = menu_get_value_from_script(menu, entry);
    if (v == INT_MIN) { snprintf(buf, len, "not loaded"); *tone = ST_DIM; return; }
    snprintf(buf, len, "%s", (v >= 0 && v < n) ? words[v] : "?");
    *tone = ST_NORMAL;
}

/* ---------------------------------------------------------------- clips

   The EOS M has no clip browser for MLV: PLAY went straight into the last clip, and the MLV
   player then prepared it with no way out. Mothlight lists the clips first (length, size,
   frame) and hands the chosen one to the player, patched so any button cancels. */

#define CLIPS_MAX 40
static int ends_with_mlv(const char * name);
struct clip { char name[16]; uint32_t kB; uint32_t ts; int frames, fps_x1000, w, h, chunks, audio; };
static struct clip clips[CLIPS_MAX];
static int clip_count, clip_cur;

static uint32_t rd32(const uint8_t * p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t * p) { return p[0] | p[1] << 8; }

/* MLVI and RAWI from the start of the file: frames, rate, sound, frame size */
static void clip_read_header(const char * path, struct clip * c)
{
    FILE * f = FIO_OpenFile(path, O_RDONLY | O_SYNC);
    if (!f) return;
    static uint8_t hdr[64];
    int pos = 0;
    for (int b = 0; b < 12; b++)
    {
        if (FIO_SeekSkipFile(f, pos, SEEK_SET) != pos) break;
        if (FIO_ReadFile(f, hdr, 52) < 20) break;
        uint32_t size = rd32(hdr + 4);
        if (size < 16 || size > 64 * 1024 * 1024) break;
        if (!memcmp(hdr, "MLVI", 4))
        {
            c->audio = rd16(hdr + 34) != 0;
            c->frames = rd32(hdr + 36);
            uint32_t nom = rd32(hdr + 44), den = rd32(hdr + 48);
            c->fps_x1000 = den ? (int)((uint64_t)nom * 1000 / den) : 0;
        }
        else if (!memcmp(hdr, "RAWI", 4)) { c->w = rd16(hdr + 16); c->h = rd16(hdr + 18); break; }
        else if (!memcmp(hdr, "VIDF", 4)) break;
        pos += size;
    }
    FIO_CloseFile(f);
}

static void clip_add(const char * name, uint32_t size, uint32_t ts)
{
    if (!ends_with_mlv(name) || strlen(name) >= sizeof(clips[0].name)) return;
    int slot = clip_count < CLIPS_MAX ? clip_count++ : -1;
    if (slot < 0)
    {
        /* full: replace the oldest if this one is newer */
        int old = 0;
        for (int i = 1; i < CLIPS_MAX; i++) if (clips[i].ts < clips[old].ts) old = i;
        if (clips[old].ts >= ts) return;
        slot = old;
    }
    struct clip * c = &clips[slot];
    memset(c, 0, sizeof(*c));
    snprintf(c->name, sizeof(c->name), "%s", name);
    c->kB = size / 1000;
    c->ts = ts;
}

static void clips_scan(void)
{
    clip_count = 0;
    const char * dir = get_dcim_dir();
#ifdef MOTH_ENGINE_UPSTREAM
    struct fio_file * file = alloc_fio_file();
    if (!file) return;
    struct fio_dirent * d = FIO_FindFirstEx(dir, file);
    if (!IS_ERROR(d))
    {
        do
        {
            struct file_info fi = convert_fio_file_info(file);
            if (!(fi.mode & ATTR_DIRECTORY)) clip_add(fi.name, fi.size, (uint32_t)fi.timestamp);
        } while (FIO_FindNextEx(d, file) == 0);
        FIO_FindClose(d);
    }
    free(file);
#else
    struct fio_file file;
    struct fio_dirent * d = FIO_FindFirstEx(dir, &file);
    if (!IS_ERROR(d))
    {
        do { if (!(file.mode & ATTR_DIRECTORY)) clip_add(file.name, file.size, file.timestamp); }
        while (FIO_FindNextEx(d, &file) == 0);
        FIO_FindClose(d);
    }
#endif
    /* newest first */
    for (int i = 1; i < clip_count; i++)
        for (int j = i; j > 0 && clips[j].ts > clips[j - 1].ts; j--)
        { struct clip t = clips[j]; clips[j] = clips[j - 1]; clips[j - 1] = t; }
    char path[96];
    for (int i = 0; i < clip_count; i++)
    {
        struct clip * c = &clips[i];
        snprintf(path, sizeof(path), "%s%s", dir, c->name);
        clip_read_header(path, c);
        c->chunks = 1;
        int len = strlen(path);
        for (int k = 0; k < 100; k++)
        {
            uint32_t size = 0;
            snprintf(path + len - 2, 3, "%02d", k);
            if (FIO_GetFileSize(path, &size) != 0) break;
            c->kB += size / 1000;
            c->chunks++;
        }
    }
}

static void clip_length(const struct clip * c, char * buf, int len)
{
    if (c->frames <= 0 || c->fps_x1000 <= 0) { snprintf(buf, len, "--:--"); return; }
    int s = (int)((int64_t)c->frames * 1000 / c->fps_x1000);
    snprintf(buf, len, "%d:%02d", s / 60, s % 60);
}

static void clip_size(const struct clip * c, char * buf, int len)
{
    if (c->kB >= 1000000) snprintf(buf, len, "%d.%d GB", (int)(c->kB / 1000000), (int)(c->kB / 100000 % 10));
    else snprintf(buf, len, "%d MB", (int)(c->kB / 1000));
}

static void build_clips(struct settings_view * v)
{
    char label[64], val[28], len_s[12], size_s[16];
    memset(v, 0, sizeof(*v));
    v->theme = theme();
    v->reduced_motion = !cfg_motion;
    v->battery_bars = battery_level_bars;
    v->set_word = "Open";
    if (clips_mode == 1)
    {
        snprintf(v->title, sizeof(v->title), "Clips");
        if (!clip_count) add_row(v, SR_NOTE, 0, 0, "No RAW clips on the card yet.", 0, 0);
        else
        {
            snprintf(label, sizeof(label), "%d clip%s, newest first", clip_count, clip_count > 1 ? "s" : "");
            add_row(v, SR_SECTION, 0, 0, label, 0, 0);
        }
        for (int i = 0; i < clip_count; i++)
        {
            const struct clip * c = &clips[i];
            char base[16];
            snprintf(base, sizeof(base), "%s", c->name);
            for (int k = strlen(base) - 1; k > 0; k--) if (base[k] == '.') { base[k] = 0; break; }
            clip_length(c, len_s, sizeof(len_s));
            clip_size(c, size_s, sizeof(size_s));
            snprintf(label, sizeof(label), "%s   %s", base, len_s);
            if (c->w > 0) snprintf(val, sizeof(val), "%dx%d  %s", c->w, c->h, size_s);
            else snprintf(val, sizeof(val), "%s", size_s);
            add_row(v, SR_ITEM, SI_CLIP_BASE + i, 0, label, val,
                    "SET: details, play or delete. The player prepares a clip the first time; any button cancels.");
        }
        add_row(v, SR_SECTION, 0, 0, "MORE", 0, 0);
        add_row(v, SR_ITEM, SI_CANON_PLAY, 0, "Canon playback", "Photos, H.264",
                "Canon's own playback for photos and H.264 movies. PLAY again to come back to LiveView.");
        return;
    }
    const struct clip * c = &clips[(clip_cur >= 0 && clip_cur < clip_count) ? clip_cur : 0];
    clip_length(c, len_s, sizeof(len_s));
    clip_size(c, size_s, sizeof(size_s));
    if (clips_mode == 2)
    {
        snprintf(v->title, sizeof(v->title), "%s", c->name);
        if (c->fps_x1000 > 0) snprintf(label, sizeof(label), "Length %s at %d.%03d fps, %d frames", len_s, c->fps_x1000 / 1000, c->fps_x1000 % 1000, c->frames);
        else snprintf(label, sizeof(label), "Length unknown (recording cut short?)");
        add_row(v, SR_NOTE, 0, 0, label, 0, 0);
        if (c->w > 0) { snprintf(label, sizeof(label), "Frame %d x %d", c->w, c->h); add_row(v, SR_NOTE, 0, 0, label, 0, 0); }
        snprintf(label, sizeof(label), "%s in %d file%s, sound %s", size_s, c->chunks, c->chunks > 1 ? "s" : "", c->audio ? "yes" : "no");
        add_row(v, SR_NOTE, 0, 0, label, 0, 0);
        add_row(v, SR_ITEM, SI_CLIP_PLAY, ST_ACCENT, "Play", "",
                "ML's player. The first time it prepares the clip (a short scan); any button cancels.");
        add_row(v, SR_ITEM, SI_CLIP_DELETE, 0, "Delete", "",
                "Asks first. Deleting removes the clip from the card for good.");
        add_row(v, SR_ITEM, SI_CLIP_BACK, 0, "Back to clips", "", 0);
        return;
    }
    snprintf(v->title, sizeof(v->title), "Delete?");
    snprintf(label, sizeof(label), "%s (%s) will be deleted for good.", c->name, size_s);
    add_row(v, SR_NOTE, 0, 0, label, 0, 0);
    add_row(v, SR_ITEM, SI_DEL_KEEP, 0, "Keep it", "", 0);
    add_row(v, SR_ITEM, SI_DEL_YES, ST_BAD, "Delete", "", "This cannot be undone.");
}

static int clip_delete(const struct clip * c)
{
    char path[96];
    const char * dir = get_dcim_dir();
    snprintf(path, sizeof(path), "%s%s", dir, c->name);
    int ok = FIO_RemoveFile(path) == 0;
    int len = strlen(path);
    for (int k = 0; k < 100; k++)
    {
        snprintf(path + len - 2, 3, "%02d", k);
        if (FIO_RemoveFile(path) != 0) break;
    }
    snprintf(path + len - 3, 4, "IDX");      /* the player's index */
    FIO_RemoveFile(path);
    return ok;
}

static int clip_request_play(const struct clip * c)
{
    char text[112];
    int n = snprintf(text, sizeof(text), "%s%s\n", get_dcim_dir(), c->name);
    FILE * f = FIO_CreateFile("ML/SETTINGS/MOTHPLAY.REQ");
    if (!f) return 0;
    FIO_WriteFile(f, text, n);
    FIO_CloseFile(f);
    return 1;
}

/* the first start: what Mothlight is, how to drive it, and whether the camera is set up for it */
static void build_welcome(struct settings_view * v)
{
    char val[28];
    memset(v, 0, sizeof(*v));
    v->theme = theme();
    v->reduced_motion = !cfg_motion;
    v->battery_bars = battery_level_bars;
    snprintf(v->title, sizeof(v->title), "Welcome");
    v->set_word = "OK";
    {
        char line[48];
        snprintf(line, sizeof(line), "%s %s is installed and running.", MOTH_UI_NAME, MOTH_UI_VERSION);
        add_row(v, SR_NOTE, 0, 0, line, 0, 0);
    }
    add_row(v, SR_ITEM, SI_WELCOME_DONE, ST_ACCENT, "Start shooting", "OK",
            "Closes this page. It comes back from Settings > About > Welcome and setup check.");

    add_row(v, SR_SECTION, 0, 0, "HOW IT WORKS", 0, 0);
    add_row(v, SR_NOTE, 0, 0, "INFO opens the quick bar at the bottom of the screen", 0, 0);
    add_row(v, SR_NOTE, 0, 0, "SET moves between tiles, UP / DOWN changes the value", 0, 0);
    add_row(v, SR_NOTE, 0, 0, "Tap the picture: the focus box moves there and focuses", 0, 0);
    add_row(v, SR_NOTE, 0, 0, "PROFILE: recording presets.  FOCUS: x10 punch-in", 0, 0);
    add_row(v, SR_NOTE, 0, 0, "Two-finger tap: the menu, with Mothlight's settings first", 0, 0);

    add_row(v, SR_SECTION, 0, 0, "SETUP CHECK", 0, 0);
    int crop = menu_get_value_from_script("Movie", "Crop mood") != INT_MIN;
    add_row(v, SR_ITEM, SI_CHECK_CROP, crop ? ST_OK : ST_BAD, "Crop Mood (crop_rec)", crop ? "Ready" : "Not loaded",
            crop ? "The sensor modes behind the 2.5K, 2.8K and 3K profiles."
                 : "Magic Lantern menu > Modules: load crop_rec, then restart the camera.");
    int raw = menu_get_value_from_script("Movie", "RAW video") != INT_MIN;
    add_row(v, SR_ITEM, SI_CHECK_RAW, raw ? ST_OK : ST_BAD, "RAW recording (mlv_lite)", raw ? "Ready" : "Not loaded",
            raw ? "Records the RAW video when you press REC."
                : "Magic Lantern menu > Modules: load mlv_lite, then restart the camera.");
    int sd = menu_get_value_from_script("Debug", "SD Overclock");
    static const char * sdw[] = { "Off", "160 MHz", "192 MHz", "240 MHz" };
    if (sd == INT_MIN) snprintf(val, sizeof(val), "Not loaded");
    else snprintf(val, sizeof(val), "%s", (sd >= 0 && sd < 4) ? sdw[sd] : "?");
    add_row(v, SR_ITEM, SI_CHECK_SD, sd == INT_MIN ? ST_DIM : sd == 0 ? ST_EDGE : ST_OK, "SD card speed (sd_uhs)", val,
            sd == INT_MIN ? "Optional: load sd_uhs in Modules for ~65 MB/s instead of ~38 (more profiles record without stopping)."
                          : "SET here steps through the speeds. 160 MHz suits most cards; restart the camera to use it.");
    int manual = lens_info.raw_shutter != 0;
    add_row(v, SR_ITEM, SI_CHECK_EXPO, manual ? ST_OK : ST_EDGE, "Movie exposure", manual ? "Manual" : "Auto",
            manual ? "Shutter, aperture and ISO are yours to set on the quick bar."
                   : "Canon menu > Movie exposure: Manual. Otherwise the camera picks shutter and ISO itself.");
    int snd = menu_get_value_from_script("RAW video", "Sound recording");
    add_row(v, SR_ITEM, SI_CHECK_SOUND, snd == INT_MIN ? ST_DIM : ST_OK, "Sound with RAW (mlv_snd)",
            snd == INT_MIN ? "Not loaded" : snd ? "On" : "Off",
            "Optional: load mlv_snd in Modules to record a WAV next to each clip.");
}

static void build_settings(struct settings_view * v)
{
    if (welcome_mode) { build_welcome(v); return; }
    if (clips_mode) { build_clips(v); return; }
    static const char * offon[] = { "Off", "On" };
    static const char * motion[] = { "Off", "Smooth" };
    static const char * info[] = { "Nothing", "Quick bar", "Profiles" };
    static const char * tally[] = { "Off", "Red frame", "Pulsing" };
    static const char * sd[] = { "Off", "160 MHz", "192 MHz", "240 MHz" };
    static const char * hacks[] = { "Off", "On", "More", "All" };
    char val[28];
    int tone;

    memset(v, 0, sizeof(*v));
    v->theme = theme();
    v->reduced_motion = !cfg_motion;
    v->battery_bars = battery_level_bars;

    struct warning w[8];
    int nw = collect_warnings(w, 8);
    v->warn_count = nw;
    for (int i = 0; i < nw; i++) add_row(v, SR_WARN, 0, w[i].bad ? ST_BAD : ST_EDGE, w[i].text, 0, 0);

    add_row(v, SR_SECTION, 0, 0, "LOOK", 0, 0);
    add_row(v, SR_ITEM, SI_THEME, 0, "Theme", theme_names[(cfg_theme >= 0 && cfg_theme < THEME_COUNT) ? cfg_theme : 0],
            "How Mothlight looks. SET tries the next one right here.");
    add_row(v, SR_ITEM, SI_MOTION, 0, "Animations", motion[cfg_motion ? 1 : 0],
            "Slides and fades in Mothlight's screens. They never run while recording.");
    add_row(v, SR_ITEM, SI_INFO, 0, "INFO button opens", info[(cfg_key >= 0 && cfg_key < 3) ? cfg_key : 1],
            "What INFO shows in LiveView. 'Nothing' gives the button back to Canon.");
    add_row(v, SR_ITEM, SI_TALLY, 0, "Recording frame", tally[(cfg_tally >= 0 && cfg_tally < 3) ? cfg_tally : 1],
            "A red frame round the screen while recording, so you never wonder.");

    add_row(v, SR_SECTION, 0, 0, "RECORDING", 0, 0);
    ml_value(val, sizeof(val), "Debug", "SD Overclock", sd, 4, &tone);
    add_row(v, SR_ITEM, SI_SDCLOCK, tone, "SD card speed (overclock)", val,
            "Lets the card write faster: ~38 MB/s at Off, ~65 at 160 MHz. 160 MHz suits most cards. Takes effect after restarting the camera.");
    {
        int is_auto, k = card_kBps(&is_auto);
        if (is_auto) snprintf(val, sizeof(val), "Auto (~%d MB/s)", k / 1000);
        else         snprintf(val, sizeof(val), "%d MB/s", k / 1000);
    }
    add_row(v, SR_ITEM, SI_CARD, 0, "Card speed for OK / BURST", val,
            "What the verdicts assume. Auto follows the SD clock; measure yours in Debug > Benchmarks and set it here.");
    ml_value(val, sizeof(val), "RAW video", "Sound recording", offon, 2, &tone);
    add_row(v, SR_ITEM, SI_SOUND, tone, "Sound with RAW video", val,
            "Records a WAV next to the MLV. Turn it off at 240 MHz or for heavy profiles: it can stop RAW recording.");
    {
        static const char * prev[] = { "Auto", "Canon (live)", "Exact framing", "Frozen" };
        ml_value(val, sizeof(val), "RAW video", "Preview", prev, 4, &tone);
        add_row(v, SR_ITEM, SI_PREVIEW, tone, "Screen while recording RAW", val,
                "Exact framing: what is recorded (grey while recording). Canon (live): colour, but in Crop Mood more zoomed in, with lines.");
    }
    ml_value(val, sizeof(val), "RAW video", "Small hacks", hacks, 4, &tone);
    add_row(v, SR_ITEM, SI_HACKS, tone, "Small hacks", val,
            "Pauses some of Canon's background work while recording, leaving more speed for the card.");

    add_row(v, SR_SECTION, 0, 0, "FOCUS AND EXPOSURE AIDS", 0, 0);
    {
        static const char * fm[] = { "Off (Crop Mood's)", "Focus box", "Precise (x10)" };
        add_row(v, SR_ITEM, SI_FOCUS, 0, "Tap to focus", fm[(cfg_focus >= 0 && cfg_focus <= 2) ? cfg_focus : 1],
                "Focus box: tap a spot, the box moves there and focuses, like Canon. Precise: focuses at x10, then back.");
    }
    add_row(v, SR_ITEM, SI_OVERLAYS, saved_global_draw < 0 ? ST_DIM : 0, "ML overlays",
            saved_global_draw < 0 ? "?" : saved_global_draw > 0 ? "On" : "Off",
            "Magic Lantern's drawings over LiveView. The aids below need it on. Hidden while Mothlight is open.");
    ml_value(val, sizeof(val), "Overlay", "Zebras", offon, 2, &tone);
    add_row(v, SR_ITEM, SI_ZEBRAS, tone, "Zebras", val, "Stripes over clipped highlights: lower ISO or close the aperture until they go.");
    ml_value(val, sizeof(val), "Overlay", "Focus Peak", offon, 2, &tone);
    add_row(v, SR_ITEM, SI_PEAK, tone, "Focus peaking", val, "Outlines what is in focus. Or punch in: FOCUS on the quick bar.");
    ml_value(val, sizeof(val), "Overlay", "False color", offon, 2, &tone);
    add_row(v, SR_ITEM, SI_FALSECOLOR, tone, "False color", val, "Paints exposure levels in colours, like a cinema monitor.");

    add_row(v, SR_SECTION, 0, 0, "PROFILES", 0, 0);
    add_row(v, SR_ITEM, SI_SAVE, ST_ACCENT, "Save Crop Mood's settings as a profile", "Save",
            "Adds what Crop Mood is set to now to your profile list (PROFILES.TXT), with a name like 'My 2.5K 2.35 10-bit'.");
    add_row(v, SR_ITEM, SI_LEARNED, cfg_learned ? ST_OK : ST_DIM, "Compression estimate",
            cfg_learned ? "Learned" : "Typical",
            "Mothlight learns how well your clips compress after each take. SET forgets it and starts over.");

    add_row(v, SR_SECTION, 0, 0, "ABOUT", 0, 0);
    {
        char about[48];
        snprintf(about, sizeof(about), "%s %s, %s engine, by Onur", MOTH_UI_NAME, MOTH_UI_VERSION,
                 MOTH_UI_ENGINE[0] == 'c' ? "Crop Mood" : "ML dev");
        add_row(v, SR_NOTE, 0, 0, about, 0, 0);
    }
    add_row(v, SR_ITEM, SI_WELCOME, 0, "Welcome and setup check", "Show",
            "The first-start page again: how Mothlight works and whether the camera is set up for it.");
    add_row(v, SR_NOTE, 0, 0, "Everything else: Magic Lantern's menu, two-finger tap", 0, 0);
}

/* refresh the rows; if they moved (a warning came or went), keep the selected item */
static void settings_refresh(const struct settings_view * fresh, uint32_t now)
{
    int prev_id = (sets.sel >= 0 && sets.sel < sview.count) ? sview.row[sets.sel].id : -1;
    int moved = fresh->count != sview.count || fresh->warn_count != sview.warn_count ||
                fresh->theme != sview.theme;
    sview = *fresh;
    if (moved) settings_rebuilt(&sets, &sview, prev_id, now);
    else settings_changed(&sets);
}

static void settings_activate(int id, uint32_t now)
{
    char msg[72] = "";
    int bad = 0, v;
#define CYCLE(menu, entry, n, missing) \
    do { v = menu_get_value_from_script(menu, entry); \
         if (v == INT_MIN) { snprintf(msg, sizeof(msg), missing); bad = 1; } \
         else menu_set_value_from_script(menu, entry, (v + 1) % (n)); } while (0)
    if (id >= SI_CLIP_BASE && id < SI_CLIP_BASE + CLIPS_MAX)
    {
        clip_cur = id - SI_CLIP_BASE;
        clips_mode = 2;
        static struct settings_view fresh;
        build_settings(&fresh);
        sview = fresh;
        settings_rebuilt(&sets, &sview, SI_CLIP_PLAY, now);
        return;
    }
    switch (id)
    {
        case SI_CLIP_BACK: case SI_DEL_KEEP:
        {
            int back_to = id == SI_DEL_KEEP ? SI_CLIP_DELETE : SI_CLIP_BASE + clip_cur;
            clips_mode = id == SI_DEL_KEEP ? 2 : 1;
            static struct settings_view fresh;
            build_settings(&fresh);
            sview = fresh;
            settings_rebuilt(&sets, &sview, back_to, now);
            return;
        }
        case SI_CLIP_DELETE:
        {
            clips_mode = 3;
            static struct settings_view fresh;
            build_settings(&fresh);
            sview = fresh;
            settings_rebuilt(&sets, &sview, SI_DEL_KEEP, now);
            return;
        }
        case SI_DEL_YES:
        {
            if (RECORDING) { snprintf(msg, sizeof(msg), "Stop recording first"); bad = 1; break; }
            int ok = clip_delete(&clips[clip_cur]);
            mlog('S', id, !ok, 0);
            clips_scan();
            clips_mode = 1;
            if (clip_cur >= clip_count) clip_cur = clip_count - 1;
            static struct settings_view fresh;
            build_settings(&fresh);
            sview = fresh;
            settings_rebuilt(&sets, &sview, clip_cur >= 0 ? SI_CLIP_BASE + clip_cur : SI_CANON_PLAY, now);
            settings_toast(&sets, ok ? "Deleted" : "Could not delete it", !ok, now);
            return;
        }
        case SI_CLIP_PLAY: case SI_CANON_PLAY:
            /* the camera task closes the page and sends PLAY on (to the player / to Canon) */
            clips_play_pending = id == SI_CLIP_PLAY ? 1 : 2;
            return;
        case SI_THEME:   cfg_theme = (cfg_theme + 1) % THEME_COUNT; break;
        case SI_MOTION:  cfg_motion = !cfg_motion; break;
        case SI_FOCUS:   cfg_focus = (cfg_focus + 1) % 3; break;
        case SI_INFO:    cfg_key = (cfg_key + 1) % 3; break;
        case SI_TALLY:   cfg_tally = (cfg_tally + 1) % 3; break;
        case SI_SDCLOCK:
            CYCLE("Debug", "SD Overclock", 4, "Load sd_uhs in Magic Lantern's Modules tab");
            if (!bad) snprintf(msg, sizeof(msg), "Restart the camera to use the new SD speed");
            break;
        case SI_CARD:    cfg_card = (cfg_card + 1) % (int)COUNT(card_mb); break;
        case SI_SOUND:   CYCLE("RAW video", "Sound recording", 2, "Load mlv_snd in Magic Lantern's Modules tab"); break;
        case SI_HACKS:   CYCLE("RAW video", "Small hacks", 3, "Small hacks need mlv_lite"); break;
        case SI_PREVIEW: CYCLE("RAW video", "Preview", 4, "The preview setting needs mlv_lite"); cfg_prevset = 1; break;
        case SI_OVERLAYS:
            /* Global Draw is off while Mothlight is open; this sets what comes back */
            if (saved_global_draw < 0) { snprintf(msg, sizeof(msg), "ML overlays are not available"); bad = 1; }
            else saved_global_draw = saved_global_draw > 0 ? 0 : 1;
            break;
        case SI_ZEBRAS:      CYCLE("Overlay", "Zebras", 2, "Zebras are not available"); break;
        case SI_PEAK:        CYCLE("Overlay", "Focus Peak", 2, "Focus peaking is not available"); break;
        case SI_FALSECOLOR:  CYCLE("Overlay", "False color", 2, "False color is not available"); break;
        case SI_SAVE:        save_current_profile(msg, sizeof(msg), &bad); break;
        case SI_LEARNED:     learned_reset_now(); snprintf(msg, sizeof(msg), "Compression estimate reset to typical"); break;
        case SI_WELCOME:     welcome_mode = 1; welcome_from_settings = 1; break;
        case SI_CHECK_SD:
            CYCLE("Debug", "SD Overclock", 4, "Load sd_uhs in Magic Lantern's Modules tab");
            if (!bad) snprintf(msg, sizeof(msg), "Restart the camera to use the new SD speed");
            break;
        case SI_CHECK_SOUND: CYCLE("RAW video", "Sound recording", 2, "Load mlv_snd in Magic Lantern's Modules tab"); break;
        case SI_CHECK_CROP: case SI_CHECK_RAW: case SI_CHECK_EXPO:
        {
            /* nothing to switch here: the help line says what to do */
            static struct settings_view chk;
            build_settings(&chk);
            for (int i = 0; i < chk.count; i++)
                if (chk.row[i].id == id && chk.row[i].tone == ST_OK) { snprintf(msg, sizeof(msg), "All set"); break; }
            if (!msg[0]) { snprintf(msg, sizeof(msg), "See the line below for how to fix it"); bad = 1; }
            break;
        }
    }
#undef CYCLE
    mlog('S', id, bad, 0);
    static struct settings_view fresh;
    build_settings(&fresh);
    settings_refresh(&fresh, now);
    if (msg[0]) settings_toast(&sets, msg, bad, now);
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

static void present_rows(int y0, int y1)
{
    uint8_t * front = bmp_vram_real();
    if (!front || !back_buf) return;
    for (int y = y0; y < y1; y++)
        memcpy(front + y * BMPPITCH, back.px + y * back.pitch, UI_W);
}

static int rows_disturbed(int y0, int y1)
{
    uint8_t * front = bmp_vram_real();
    if (!front || !back_buf) return 0;
    for (int y = y0 + 1; y < y1; y += 4)
        if (memcmp(front + y * BMPPITCH, back.px + y * back.pitch, UI_W)) return 1;
    return 0;
}

/* Canon's overlay layer back on after Mothlight -- except where Crop Mood keeps it off (its x5
   modes, Canon's clean display), or its icons would flash up until crop_rec hides them again */
static int crop_mood_hides_canon(void)
{
    int mood = menu_get_value_from_script("Movie", "Crop mood");
    return mood != INT_MIN && mood > 0 && lv_dispsize == 5 && !RECORDING && lv_disp_mode == 0;
}

static void canon_layer_back(void)
{
    if (!crop_mood_hides_canon()) canon_gui_enable_front_buffer(1);
}

static int rect_disturbed(int x0, int x1, int y0, int y1)
{
    uint8_t * front = bmp_vram_real();
    if (!front || !back_buf) return 0;
    for (int y = y0 + 1; y < y1; y += 3)
        if (memcmp(front + y * BMPPITCH + x0, back.px + y * back.pitch + x0, x1 - x0)) return 1;
    return 0;
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
    memset(back_buf, 0, UI_W * UI_H);       /* transparent: the quick bar only draws its rows */
    zoom_restore = 0;                       /* opened again before zooming back: still punched in */
    focus_box_erase();                      /* the bar would leave it floating above */

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
    ov_mode = 0;
    panel_from_bar = 0;
    settings_from_bar = 0;
    if (zoom_before) zoom_restore = 1;      /* the task zooms back once idle (then records, for REC) */
    if (welcome_mode) { cfg_welcomed = 1; welcome_mark_pending = 1; }  /* seen, however it was closed */
    welcome_mode = welcome_from_settings = 0;
    clips_mode = 0;
    tap_pending = 0;
    panel.state = PS_CLOSED;
    qbar.state = QB_CLOSED;
    apply_phase = 0;                        /* an apply cut short is not resumed on the next open */
    keys_released = 0;
    if (saved_global_draw > 0) menu_set_value_from_script("Overlay", "Global Draw", saved_global_draw);
    saved_global_draw = -1;
    /* an instant close leaves the panel's pixels behind; ML's menu repaints itself */
    if (!gui_menu_shown()) clrscr();
    canon_layer_back();
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

        if (open_request && !overlay_active && fj_state != FJ_IDLE) { open_request = 0; }
        if (open_request && !overlay_active)
        {
            int mode = open_request;
            opening = 1;
            open_request = 0;
            if (can_show_panel() && overlay_begin())
            {
                ov_mode = mode;
                if (mode == OV_BAR)
                {
                    qb_fill(&qview);
                    qbar_open(&qbar, &qview, get_ms_clock());
                    qview_read_at = get_ms_clock();
                    if (bar_hint_seen)
                    {
                        /* after the first time: the most serious warning instead of the controls */
                        struct warning w[1];
                        if (collect_warnings(w, 1)) qbar_toast(&qbar, w[0].text, w[0].bad, get_ms_clock() + 200);
                    }
                    bar_hint_seen = 1;
                }
                else if (mode == OV_SETTINGS)
                {
                    if (clips_mode) { clips_scan(); clips_mode = 1; }
                    build_settings(&sview);
                    settings_open(&sets, &sview, get_ms_clock());
                }
                else
                {
                    fill_view();
                    panel_open(&panel, &view, get_ms_clock());
                }
                mlog('O', cfg_theme, profile_count, mode);
                was_lv = lv;
            }
            open_request = 0;               /* an INFO that arrived while starting up is not a new request */
            opening = 0;
            if (!overlay_active) welcome_mode = welcome_from_settings = clips_mode = 0;  /* could not open: next boot */
            continue;
        }

        if (overlay_active && keys_released)
        {
            /* the watchdog gave the buttons back while we were stuck: tidy up quietly */
            overlay_end(CLOSE_WATCHDOG);
            continue;
        }

        if (overlay_active && ov_mode == OV_SETTINGS)
        {
            int k, closed = 0, go_back = 0;
            int pending_tap = tap_pending;
            tap_pending = 0;
            while ((k = pop_key()) != PK_NONE || pending_tap)
            {
                repaint_until = now + 300;
                if (k == PK_CLOSE_NOW) { overlay_end(CLOSE_KEY_PASS); closed = 1; break; }
                int act;
                if (k == PK_NONE) { act = settings_tap(&sets, &sview, tap_x, tap_y, now); pending_tap = 0; }
                else act = settings_key(&sets, &sview, k, now);
                if (act == SA_ACTIVATE && sets.act_id == SI_WELCOME_DONE) { go_back = 1; break; }
                if (act == SA_ACTIVATE) { settings_activate(sets.act_id, now); now = get_ms_clock(); }
                if (clips_play_pending) break;
                if (act == SA_CLOSE && clips_mode > 1)
                {
                    /* INFO in a clip / delete question: one step back, not out */
                    sets.open = 1;
                    settings_activate(clips_mode == 3 ? SI_DEL_KEEP : SI_CLIP_BACK, now);
                    act = SA_NONE;
                    continue;
                }
                if (act == SA_CLOSE) { go_back = 1; break; }
            }
            if (closed) continue;
            if (clips_play_pending)
            {
                int what = clips_play_pending;
                clips_play_pending = 0;
                int ok = what == 2 || clip_request_play(&clips[clip_cur]);
                mlog('S', what == 1 ? SI_CLIP_PLAY : SI_CANON_PLAY, !ok, 0);
                overlay_end(CLOSE_KEY);
                clips_resent = what == 2 || !ok;      /* Canon's playback: Mothlight lets that PLAY through */
                module_send_keypress(MODULE_KEY_PLAY);
                continue;
            }
            if (go_back && welcome_mode)
            {
                cfg_welcomed = 1;
                welcome_mark_pending = 1;
                welcome_mode = 0;
                if (welcome_from_settings)
                {
                    /* back to the settings page it came from */
                    welcome_from_settings = 0;
                    static struct settings_view fresh;
                    build_settings(&fresh);
                    sview = fresh;
                    sets.open = 1;          /* INFO closed the welcome's view of the page */
                    settings_rebuilt(&sets, &sview, SI_WELCOME, now);
                    continue;
                }
            }
            if (go_back)
            {
                if (settings_from_bar)
                {
                    settings_from_bar = 0;
                    memset(back_buf, 0, UI_W * UI_H);
                    present_rows(0, UI_H);
                    ov_mode = OV_BAR;
                    qb_fill(&qview);
                    qbar_open(&qbar, &qview, now);
                    qview_read_at = now;
                    mlog('O', cfg_theme, profile_count, OV_BAR);
                }
                else overlay_end(CLOSE_KEY);
                continue;
            }

            /* values Canon or ML change meanwhile (battery, a module's state): twice a second */
            if (now - qview_read_at > 500)
            {
                static struct settings_view fresh;
                build_settings(&fresh);
                if (memcmp(&fresh, &sview, sizeof(fresh))) settings_refresh(&fresh, now);
                qview_read_at = now;
            }

            if (!lv != !was_lv) { mlog('L', lv ? 1 : 0, 0, 0); was_lv = lv; }
            int why = gui_menu_shown() ? CLOSE_MENU : RECORDING ? CLOSE_REC :
                      !is_movie_mode() ? CLOSE_PHOTO : !display_is_on() ? CLOSE_DISPLAY : 0;
            if (!lv)
            {
                if (!lv_lost_at) lv_lost_at = now ? now : 1;
                if (now - lv_lost_at > (zoom_job ? 4000u : 700u)) why = CLOSE_LV;
            }
            else lv_lost_at = 0;
            if (why) { overlay_end(why); continue; }

            struct present out;
            last_frame_ms = now;
            int fl = settings_frame(&sets, &sview, &back, now, &out);
            if (fl & PF_PRESENTED) present(&out);
            if ((int32_t)(repaint_until - now) > 0) present_full();
            else if (screen_was_disturbed()) { present_full(); mlog('R', 0, 0, 0); }
            msleep(20);
            continue;
        }

        if (overlay_active && ov_mode == OV_BAR)
        {
            int k, closed = 0, to_panel = 0;
            int pending_tap = tap_pending;
            tap_pending = 0;
            while ((k = pop_key()) != PK_NONE || pending_tap)
            {
                repaint_until = now + 300;
                if (k == PK_CLOSE_NOW) { overlay_end(CLOSE_KEY_PASS); closed = 1; break; }
                int act;
                if (k == PK_NONE) { act = qbar_tap(&qbar, &qview, tap_x, tap_y, now); pending_tap = 0; }
                else act = qbar_key(&qbar, &qview, k, now);
                if (act == QA_ADJUST)
                {
                    qb_adjust(qbar.adj_item, qbar.adj_sign, now);
                    now = get_ms_clock();       /* Canon took its time */
                    qb_fill(&qview);
                    qbar_changed(&qbar);
                    qview_read_at = now;
                }
                if (act == QA_LOCKED) qbar_toast(&qbar, qb_locked_reason(qbar.adj_item), 0, now);
                if (act == QA_OPEN_PANEL) { to_panel = 1; break; }
            }
            if (closed) continue;

            if (to_panel)
            {
                /* the profile list, in place: same overlay, the bar's rows cleared first */
                memset(back_buf, 0, UI_W * UI_H);
                present_rows(QB_TOP, UI_H);
                qbar.state = QB_CLOSED;
                ov_mode = OV_PANEL;
                panel_from_bar = 1;
                fill_view();
                panel_open(&panel, &view, now);
                mlog('O', cfg_theme, profile_count, OV_PANEL);
                continue;
            }

            /* Canon changes values itself: the dial is its shutter, auto ISO, a lens swap.
               Look 10 times a second (a few property reads). */
            if (now - qview_read_at > 100)
            {
                static struct qbar_view fresh;
                qb_fill(&fresh);
                if (memcmp(fresh.item, qview.item, sizeof(fresh.item)) || fresh.battery_bars != qview.battery_bars)
                    { qview = fresh; qbar_changed(&qbar); }
                qview_read_at = now;
            }

            if (!lv != !was_lv) { mlog('L', lv ? 1 : 0, 0, 0); was_lv = lv; }
            int why = gui_menu_shown() ? CLOSE_MENU : RECORDING ? CLOSE_REC :
                      !is_movie_mode() ? CLOSE_PHOTO : !display_is_on() ? CLOSE_DISPLAY : 0;
            if (!lv)
            {
                if (!lv_lost_at) lv_lost_at = now ? now : 1;
                if (now - lv_lost_at > (zoom_job ? 4000u : 700u)) why = CLOSE_LV;
            }
            else lv_lost_at = 0;
            if (why) { overlay_end(why); continue; }

            struct present out, bat;
            last_frame_ms = now;
            if (qbar_battery_frame(&qbar, &qview, &back, now, &bat) & PF_PRESENTED) present(&bat);
            int fl = qbar_frame(&qbar, &qview, &back, now, &out);
            if (fl & PF_PRESENTED) present(&out);
            if (qbar_settled(&qbar))
            {
                /* only the bar's own rows (and its strip) are ours; LiveView shows above */
                int y0 = qbar.strip_shown ? QB_STRIP_Y : QB_Y;
                if ((int32_t)(repaint_until - now) > 0) present_rows(y0, UI_H);
                else if (rows_disturbed(y0, UI_H)) { present_rows(y0, UI_H); mlog('R', 0, 0, 0); }
                /* the battery chip: only its own rectangle (Canon's icons may sit beside it) */
                if (qbar.bat_shown && rect_disturbed(QB_BAT_X0, QB_BAT_X1, QB_BAT_Y0, QB_BAT_Y1))
                {
                    struct present r = { QB_BAT_X0, QB_BAT_X1, 1, { { QB_BAT_Y0, QB_BAT_Y1, 0 } } };
                    present(&r);
                }
            }
            if (fl & PF_CLOSED) overlay_end(CLOSE_KEY);
            msleep(20);
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
            if ((fl & PF_CLOSED) && panel_from_bar)
            {
                /* back to the quick bar, which now shows the profile in use */
                panel_from_bar = 0;
                memset(back_buf, 0, UI_W * UI_H);
                present_rows(0, UI_H);
                ov_mode = OV_BAR;
                qb_fill(&qview);
                qbar_open(&qbar, &qview, get_ms_clock());
                qview_read_at = get_ms_clock();
                mlog('O', cfg_theme, profile_count, OV_BAR);
                msleep(20);
                continue;
            }
            if (fl & PF_CLOSED) overlay_end(was_done ? CLOSE_DONE : CLOSE_KEY);
            msleep(20);                     /* 50 Hz: a foreign redraw is gone within one frame */
            continue;
        }

        if (!overlay_active) zoom_restore_step();
        focus_box_step(now);
        if (welcome_mark_pending && !RECORDING && !overlay_active) { welcome_mark_pending = 0; welcome_marker_write(); }
        {
            /* RAW already on from before: the preview fixes apply without re-applying a profile */
            static int preview_checked;
            if (!preview_checked && now > 3000 && !RECORDING)
            {
                preview_checked = 1;
                if (menu_get_value_from_script("Movie", "RAW video") == 1) recording_preview_defaults();
            }
        }
        if (fj_rec_after && fj_state == FJ_IDLE && !overlay_active)
        {
            if (lv && lv_dispsize != 10 && !RECORDING) { fj_rec_after = 0; mlog('F', 5, 0, 0); module_send_keypress(MODULE_KEY_REC); }
            else if (now - fj_rec_after > 5000) fj_rec_after = 0;
        }

        /* the first start after installing: the welcome page, once LiveView has settled */
        {
            static uint32_t calm_since;
            int calm = !overlay_active && lv && is_movie_mode() && !RECORDING && !gui_menu_shown() &&
                       display_is_on() && apply_helper != 1 && !open_request;
            if (!calm) calm_since = 0;
            else if (!calm_since) calm_since = now | 1;
            if (!cfg_welcomed && !welcome_tried && calm && now - calm_since > 2500)
            {
                welcome_tried = 1;
                welcome_mode = 1;
                open_request = OV_SETTINGS;
            }
        }
        if (rec_after_zoom && !overlay_active)
        {
            if (!zoom_job && !zoom_restore && lv_dispsize != 10 && lv && !RECORDING)
            {
                rec_after_zoom = 0;
                mlog('E', QB_FOCUS, 0, 2);
                module_send_keypress(MODULE_KEY_REC);
            }
            else if (now - rec_after_zoom > 4000) { rec_after_zoom = 0; mlog('E', QB_FOCUS, 0, 3); }
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
    if (!overlay_active && fj_state != FJ_IDLE && fj_mode == FOCUS_X10)
    {
        /* the focus job is showing x10 */
        if (key == MODULE_KEY_UNPRESS_HALFSHUTTER) { fj_half_held = 0; return 0; }
        if (key == MODULE_KEY_PRESS_HALFSHUTTER) return 0;
        if (key == MODULE_KEY_REC) { fj_rec_after = get_ms_clock() | 1; fj_cancel = 1; return 0; }
        if (key == MODULE_KEY_TOUCH_2_FINGER || key == MODULE_KEY_PLAY)
        {
            /* ML's menu / playback: let them through now (ML needs its idle state for that).
               The key we send comes back through here: that copy goes on (no loop). */
            if (fj_resent == (int)key) { fj_resent = 0; return 1; }
            fj_cancel = 1;
            release_gui_state();
            fj_resent = (int)key;
            module_send_keypress(key);
            return 0;
        }
        if (key == MODULE_KEY_TOUCH_1_FINGER || key == MODULE_KEY_UNTOUCH_1_FINGER) return 0;
        if (key == MODULE_KEY_INFO || key == MODULE_KEY_PRESS_SET || key == MODULE_KEY_PRESS_UP ||
            key == MODULE_KEY_PRESS_DOWN || key == MODULE_KEY_MENU || key == MODULE_KEY_TRASH)
            { fj_cancel = 1; return 0; }
        return 1;
    }
    if (!overlay_active && key == MODULE_KEY_PRESS_HALFSHUTTER && cfg_focus == FOCUS_X10 && focus_takes_over())
    {
        focus_start(-1, 0, 1);
        return 0;
    }
    if (!overlay_active && (key == MODULE_KEY_TOUCH_1_FINGER || key == MODULE_KEY_UNTOUCH_1_FINGER) &&
        focus_takes_over())
        return 0;                           /* the raw handler took this tap */
    if (overlay_active && (keys_released || get_ms_clock() - last_frame_ms > 2000))
    {
        /* The panel task stopped drawing: never hold the camera's buttons hostage. Only flag
           it here (this is the GUI task); the panel task frees and restores when it wakes. */
        if (!keys_released) { release_gui_state(); canon_layer_back(); }
        keys_released = 1;
        return 1;
    }
    if (overlay_active && handing_back) return 1;
    if (overlay_active && clips_mode && key == MODULE_KEY_PLAY) { push_key(PK_CLOSE); return 0; }
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
            if ((key == MODULE_KEY_REC || key == MODULE_KEY_LV) && zoom_before && lv_dispsize == 10)
            {
                /* RAW cannot record at x10: zoom back first (the task does it), then record */
                rec_after_zoom = get_ms_clock() | 1;
                return 0;
            }
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

    if (key == MODULE_KEY_PLAY && !overlay_active)
    {
        if (clips_resent) { clips_resent = 0; return 1; }          /* ours, for Canon's playback */
        if (cfg_clips && lv && is_movie_mode() && !RECORDING && !gui_menu_shown() &&
            menu_get_value_from_script("Movie", "RAW video") == 1 && !opening && !open_request)
        {
            clips_mode = 1;
            open_request = OV_SETTINGS;
            return 0;
        }
    }

    int trigger = cfg_key && key == MODULE_KEY_INFO;
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
        open_request = cfg_key == 2 ? OV_PANEL : OV_BAR;
        return 0;
    }
    return 1;
}

/* Touch position, experimental. ML gives modules only "touched" / "released" events, but its
   EOS M notes (platform/EOSM.202/consts.h) document where Canon keeps the position:
   TOUCH_XY_RAW1 = 0x4D868, found with memspy, Y in the upper half-word and X in the lower
   (0..719, 0..479). Reading RAM is harmless; a position is used only when it is plausible,
   and every read is logged so it can be checked against the camera. */
#define EOSM_TOUCH_XY_ADDR 0x4D868
#ifndef moth_peek32
#define moth_peek32(addr) (*(volatile uint32_t *)(addr))
#endif
#define EOSM_TOUCH_1     0x6f
#define EOSM_UNTOUCH_1   0x70

static int touch_pos(int * x, int * y, uint32_t * raw)
{
    uint32_t v = moth_peek32(EOSM_TOUCH_XY_ADDR);
    *raw = v;
    *x = (int)(v & 0xffff);
    *y = (int)(v >> 16);
    return *x < 720 && *y < 480 && v != 0xffffffff;
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
    if (e && e->type == 0 && !overlay_active && !IS_FAKE(e))
    {
        /* focus at x10: a tap picks the spot (Canon would move the recorded window instead) */
        int p = e->param;
        if ((p == EOSM_TOUCH_1 || p == EOSM_UNTOUCH_1) && (fj_state != FJ_IDLE || focus_takes_over()))
        {
            int x, y; uint32_t v;
            int ok = touch_pos(&x, &y, &v);
            mlog_v('X', p, ok, 2, v);
            if (p == EOSM_UNTOUCH_1)
            {
                if (fj_state != FJ_IDLE) { if (fj_mode == FOCUS_X10) fj_cancel = 1; }   /* x10: done looking */
                else focus_start(ok ? x : UI_W / 2, ok ? y : UI_H / 2, 0);
            }
            return 0;
        }
    }
    if (!overlay_active || keys_released || handing_back) return 1;
    if (e && e->type == 0 && (e->param == EOSM_TOUCH_1 || e->param == EOSM_UNTOUCH_1 || e->param == 0x0f || e->param == 0x11))
    {
        int x, y; uint32_t v;
        int ok = touch_pos(&x, &y, &v);
        mlog_v('Y', e->param, e->arg, 0, (uint32_t)(uintptr_t)e->obj);
        if (e->param == EOSM_UNTOUCH_1 && ok && (ov_mode == OV_BAR || ov_mode == OV_SETTINGS))
        {
            tap_x = x; tap_y = y; tap_pending = 1;
        }
        mlog_v('X', e->param, e->param == EOSM_UNTOUCH_1 && ok && (ov_mode == OV_BAR || ov_mode == OV_SETTINGS), 0, v);
    }
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
    open_request = OV_PANEL;
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

static void learned_reset_now(void)
{
    cfg_learned = cfg_bpp10 = cfg_bpp11 = cfg_bpp12 = cfg_bpp14 = 0;
    bpp_from_config();
}

static MENU_SELECT_FUNC(learned_reset) { learned_reset_now(); }

static MENU_SELECT_FUNC(open_settings_from_menu)
{
    open_request = OV_SETTINGS;
    gui_stop_menu();
}

static MENU_UPDATE_FUNC(about_update)
{
    MENU_SET_VALUE("v%s", MOTH_UI_VERSION);
    MENU_SET_HELP("%s by Onur. Engine: %s. %d profiles%s.", MOTH_UI_NAME,
                  MOTH_UI_ENGINE, profile_count, profiles_from_file ? " from PROFILES.TXT" : " (built in)");
}

/* Mothlight's own tab, first in Magic Lantern's menu (the menu patch creates it there; on an
   engine without the patch ML adds it at the end) */
static struct menu_entry moth_menu[] =
{
    {
        .name = "Settings", .select = open_settings_from_menu,
        .help = "Mothlight's settings page: look, focus, card, sound, aids, profiles.",
        .help2 = "Plain words and a help line for each; everything else stays in these tabs.",
    },
    {
        .name    = "Profiles",
        .select  = open_from_menu,
        .update  = main_update,
        .help    = "Choose and apply a recording profile (Crop Mood presets).",
        .help2   = "Shortcut in LiveView: INFO opens the quick bar, PROFILE opens the list.",
        .depends_on = DEP_MOVIE_MODE,
    },
    {
        .name = "Theme", .priv = &cfg_theme, .max = THEME_COUNT - 1,
        .choices = CHOICES("Mothlight", "Stock", "Studio", "Amber", "Signal", "Material",
                           "Soft", "Glass", "Daylight", "Night", "Mono"),
        .help = "How Mothlight looks. Takes effect the next time it opens.",
    },
    {
        .name = "INFO opens", .priv = &cfg_key, .max = 2,
        .choices = CHOICES("nothing", "Quick bar", "Profiles"),
        .help = "Quick bar: shutter, aperture, ISO, WB, focus and profile over LiveView.",
        .help2 = "Profiles: the profile list directly. 'nothing' gives INFO back to Canon.",
    },
    {
        .name = "Tap to focus", .priv = &cfg_focus, .max = 2,
        .choices = CHOICES("OFF", "Focus box", "Precise (x10)"),
        .help = "Crop Mood presets: Focus box = tap a spot, Canon-style box focuses there.",
        .help2 = "Precise: half-press or tap focuses at x10 (most accurate), then back to x5.",
    },
    {
        .name = "Animations", .priv = &cfg_motion, .max = 1,
        .choices = CHOICES("OFF", "Smooth"),
        .help = "Slides and fades. They never run while recording.",
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
    {
        .name = "Clips on PLAY", .priv = &cfg_clips, .max = 1,
        .choices = CHOICES("OFF", "ON"),
        .help = "With RAW on, PLAY shows Mothlight's clip list first (length, size, delete).",
        .help2 = "OFF: PLAY goes straight into ML's player, as before.",
    },
    { .name = "About Mothlight", .update = about_update, .icon_type = IT_ALWAYS_ON },
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
    if (welcome_marker_exists()) cfg_welcomed = 1;
    /* a damaged config must not reach ML's menu as an out-of-range choice */
    if (cfg_theme < 0 || cfg_theme >= THEME_COUNT) cfg_theme = THEME_MOTHLIGHT;
    if (cfg_key < 0 || cfg_key > 2) cfg_key = 1;
    if (cfg_motion < 0 || cfg_motion > 1) cfg_motion = 1;
    if (cfg_focus < 0 || cfg_focus > 2) cfg_focus = FOCUS_BOX;
    if (cfg_clips < 0 || cfg_clips > 1) cfg_clips = 1;
    if (cfg_tally < 0 || cfg_tally > 2) cfg_tally = 1;
    if (cfg_card < 0 || cfg_card >= (int)COUNT(card_mb)) cfg_card = 0;
    menu_add("Mothlight", moth_menu, COUNT(moth_menu));
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
