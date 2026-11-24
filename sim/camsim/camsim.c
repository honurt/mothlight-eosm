/*
 * camsim: runs the real modules/moth_ui/moth_ui.c against a fake EOS M.
 *
 * The panel simulator (sim/e2e.c) drives panel.c directly and so never saw the camera
 * glue -- which is where both bugs found on the camera were. This harness compiles
 * moth_ui.c itself and models the parts of the camera it talks to, pessimistically:
 *
 *   - buttons reach the module the way ML's module.c delivers them: portable key to
 *     CBR_KEYPRESS (0 when ML has no name for it), then the raw event to CBR_KEYPRESS_RAW,
 *     and only if both pass does Canon get it
 *   - Canon redraws its LiveView screen over the BMP layer on every button it receives,
 *     whether or not the front buffer is "disabled" (what the camera actually did)
 *   - applying a profile pauses LiveView and set_lv_zoom blocks, like Crop Mood does
 *   - the module's task and the GUI task alternate deterministically (pthread hand-off),
 *     the task's msleep() is where time passes
 *
 * After every frame the task draws, invariants are checked (screen == panel when settled,
 * no Canon redraw caused by navigation, resources restored after close, no leaks ...).
 * Each scenario runs in its own process so moth_ui.c's statics start fresh.
 *
 *   camsim [fonts dir] [frames dir]     runs every scenario; frames dir: dump PPMs
 */
#include <pthread.h>
#include <unistd.h>
#include <sys/wait.h>
#include "fake/fake_ml.h"

/* ---------------------------------------------------------------- the module under test */
#include "../../modules/moth_ui/moth_ui.c"
#undef malloc
#undef free

/* ---------------------------------------------------------------- world */

struct cam_state cam;
int gui_state;
static int raw_recordings, h264_recordings, rec_at_x10;
static char mlv_played[128];
static int mlv_play_runs;
static const int shutters[] = { 33, 40, 50, 60, 80, 100, 125, 160, 200, 250, 320, 400, 500, 640, 800, 1000, 1250, 1600, 2000, 2500, 3200, 4000 };
static const int fstops10[] = { 14, 16, 18, 20, 22, 25, 28, 32, 35, 40, 45, 50, 56, 63, 71, 80, 90, 100, 110, 130, 140, 160, 180, 200, 220 };
static const int isos[] = { 100, 125, 160, 200, 250, 320, 400, 500, 640, 800, 1000, 1250, 1600, 2000, 2500, 3200, 4000, 5000, 6400 };
static int sh_i = 2, ap_i = 6, iso_i = 9, auto_expo, lens_min_i = 0, lens_max_i = 21;
static int lens_changes;
static uint32_t now_ms;
static uint8_t * bmp;                       /* 960 x 480, like the EOS M's BMP VRAM */
struct font font_dynamic[UF_COUNT];

static const char * scenario = "?";
static int failures;
#define FAIL(...) do { fprintf(stderr, "  FAIL [%s @%ums] ", scenario, now_ms); \
                       fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); failures++; } while (0)
#define EXPECT(c, ...) do { if (!(c)) FAIL(__VA_ARGS__); } while (0)

/* counters the scenarios look at */
static int canon_got[256];                  /* Canon button codes received */
static int canon_redraws, canon_redraws_open, canon_key_redraws_open;
static long heap_live, heap_peak;
static int fail_big_malloc;                 /* fail the next allocation >= 100 kB */
static int notify_count;
static char notify_last[96];
static int stall_zoom_ms;                   /* set_lv_zoom blocks this long */
static int lv_pause_ms = 350;               /* LiveView off while Crop Mood reconfigures */
static int canon_periodic_ms;               /* Canon refreshes its screen on its own */
static int frames_checked, settled_mismatch;
static const char * dump_dir;
static int dump_n;
static int global_draw_at_start = 1;
static int soak_settings_frames, soak_x10_frames;
static int crop_wants_hidden(void);
static int begin_pending;                  /* overlay_begin() is between its first call and overlay_active = 1 */

/* menus: (menu, entry) -> value; missing entries model unloaded modules */
struct mval { const char * menu, * entry; int value; int exists; };
static struct mval menus[] = {
    { "Movie", "RAW video", 0, 1 },
    { "Movie", "Crop mood", 0, 1 },
    { "RAW video", "Data format", 0, 1 },
    { "RAW video", "Resolution", 0, 1 },
    { "RAW video", "Aspect ratio", 0, 1 },
    { "Crop mood", "Preset:", 0, 1 },
    { "Crop mood", "Preset: ", 0, 1 },
    { "Crop mood", "Preset:  ", 0, 1 },
    { "Crop mood", "Aspect ratio:", 0, 1 },
    { "Crop mood", "Framerate:", 0, 1 },
    { "Crop mood", "Bit-depth", 0, 1 },
    { "Overlay", "Global Draw", 1, 1 },
    { "Debug", "SD Overclock", 2, 1 },
    { "RAW video", "Sound recording", 0, 1 },
    { "RAW video", "Small hacks", 1, 1 },
    { "RAW video", "Preview", 1, 1 },
    { "RAW video", "Preview toggle", 1, 1 },
    { "Overlay", "Zebras", 0, 1 },
    { "Overlay", "Focus Peak", 0, 1 },
    { "Overlay", "False color", 0, 1 },
};
static struct mval * find_menu(const char * m, const char * e)
{
    for (int i = 0; i < COUNT(menus); i++)
        if (!strcmp(menus[i].menu, m) && !strcmp(menus[i].entry, e)) return &menus[i];
    return 0;
}
static void set_exists(const char * m, const char * e, int x) { find_menu(m, e)->exists = x; }
static int menu_val(const char * m, const char * e) { return find_menu(m, e)->value; }

/* RAW frame the recorder would produce right now (set when LiveView settles after apply) */
static int raw_w, raw_h;

/* ---------------------------------------------------------------- scheduling: task <-> GUI */

static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
#define MAX_TASKS 8
struct ctask { void (*fn)(void *); void * arg; uint32_t wake; int done, blocked; const char * name; };
static struct ctask tasks[MAX_TASKS];
static int ntasks;
static int turn = -1;                       /* -1: world/GUI; else the task running */
static __thread int my_task = -1;
static int task_running;

static void post_frame_checks(void);

static void yield_to_world(uint32_t wake, int blocked)
{
    int me = my_task;
    pthread_mutex_lock(&mx);
    tasks[me].wake = wake; tasks[me].blocked = blocked;
    turn = -1; pthread_cond_broadcast(&cv);
    while (turn != me) pthread_cond_wait(&cv, &mx);
    pthread_mutex_unlock(&mx);
}

static void cam_block(int ms);
static int ui_stall_once;                   /* the UI task hangs this long, once (watchdog tests) */
void msleep(int ms)
{
    if (my_task == 0) post_frame_checks();  /* the UI task just finished a frame */
    if (my_task == 0 && ui_stall_once) { int st = ui_stall_once; ui_stall_once = 0; cam_block(st); }
    yield_to_world(now_ms + (ms > 0 ? ms : 1), 0);
}

/* a task is inside a slow camera call: time passes, the GUI and other tasks keep running */
static void cam_block(int ms) { yield_to_world(now_ms + ms, 1); }

struct start { int id; };
static void * task_thread(void * arg)
{
    int id = (int)(intptr_t)arg;
    my_task = id;
    pthread_mutex_lock(&mx);
    while (turn != id) pthread_cond_wait(&cv, &mx);
    pthread_mutex_unlock(&mx);
    tasks[id].fn(tasks[id].arg);
    pthread_mutex_lock(&mx);
    tasks[id].done = 1; turn = -1; pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mx);
    return 0;
}

void task_create(const char * name, int prio, int stack, void * fn, void * arg)
{
    int id = -1;
    for (int i = 1; i < ntasks; i++) if (tasks[i].done) { id = i; break; }   /* finished tasks free their slot */
    if (id < 0)
    {
        if (ntasks >= MAX_TASKS) { fprintf(stderr, "too many tasks\n"); abort(); }
        id = ntasks++;
    }
    memset(&tasks[id], 0, sizeof(tasks[id]));
    tasks[id].fn = (void (*)(void *)) fn;
    tasks[id].arg = arg;
    tasks[id].wake = now_ms;                /* runs at the next tick */
    tasks[id].name = name;
    pthread_t th;
    pthread_create(&th, 0, task_thread, (void *)(intptr_t)id);
    pthread_detach(th);
    task_running = 1;
}

static void world_tick(void);

static void check_visible(void);

/* advance the world; give each task its turns */
static void run_for(int ms)
{
    uint32_t end = now_ms + ms;
    while (now_ms < end)
    {
        now_ms += 5;
        world_tick();
        for (int i = 0; i < ntasks; i++)
        {
            if (tasks[i].done || now_ms < tasks[i].wake) continue;
            pthread_mutex_lock(&mx);
            turn = i; pthread_cond_broadcast(&cv);
            while (turn != -1) pthread_cond_wait(&cv, &mx);
            pthread_mutex_unlock(&mx);
        }
        check_visible();
    }
}

/* ML's long-press ring (x 655-685, y 328-358) is ML's own UI while DOWN is held: allowed there */
static int ring_active;
/* what part of the screen the overlay owns, and whether it is fully up */
static int ov_owned_top(void)
{
    if (ov_mode == OV_BAR) return qbar.strip_shown ? QB_STRIP_Y : QB_Y;
    return 0;
}
static int ov_settled(void)
{
    if (!overlay_active || !back_buf) return 0;
    if (ov_mode == OV_BAR) return qbar_settled(&qbar);
    if (ov_mode == OV_SETTINGS) return settings_settled(&sets);
    return panel_settled(&panel);
}

static int row_differs(int y)
{
    const uint8_t * a = bmp + y * BMPPITCH, * b = back.px + y * back.pitch;
    if (ring_active && y >= 328 && y < 358)
        return memcmp(a, b, 655) || memcmp(a + 685, b + 685, UI_W - 685);
    return memcmp(a, b, UI_W) != 0;
}

/* What the user sees: while the panel is fully open, anything else on the screen may last
   at most one or two LCD frames. This is the "black flash" / "goes to shooting and back". */
#define MAX_OVERDRAW_MS 45
static uint32_t overdrawn_since;
static int worst_overdraw;
static void check_visible(void)
{
    /* a stalled panel task cannot repaint (that is what the watchdog is for) */
    if (!ov_settled() || tasks[0].blocked || keys_released)
    { overdrawn_since = 0; return; }
    int diff = 0;
    for (int y = ov_owned_top() + 1; y < UI_H && !diff; y += 6) diff = row_differs(y);
    if (!diff) { overdrawn_since = 0; return; }
    if (!overdrawn_since) overdrawn_since = now_ms;
    int d = now_ms - overdrawn_since;
    if (d > worst_overdraw) worst_overdraw = d;
    if (d == MAX_OVERDRAW_MS + 5) FAIL("the panel was overdrawn for more than %d ms (visible flash)", MAX_OVERDRAW_MS);
}

/* the card: files the module writes */
static char logfile[65536];
static int logfile_len, logfile_writes, logfile_writes_recording;
static struct { int open, profiles; } the_file;
static int welcome_marker_on_card;
static struct { int open, idx, pos; } reader;
static char play_req[128];
static int play_req_len;
static char profiles_written[16384];        /* PROFILES.TXT as the module wrote it */
static int profiles_written_len, profiles_writes, fail_profiles_write;
static const char * profiles_txt;           /* NULL: no PROFILES.TXT on the card */
FILE * FIO_CreateFile(const char * name)
{
    if (!strcmp(name, "ML/SETTINGS/MOTHPLAY.REQ"))
    {
        play_req_len = 0; play_req[0] = 0; the_file.profiles = 3;
        return (FILE *) &the_file;
    }
    if (!strcmp(name, "ML/SETTINGS/MOTHWEL.TXT"))
    {
        if (cam.recording) FAIL("welcome marker written while recording");
        welcome_marker_on_card = 1; the_file.profiles = 2;
        return (FILE *) &the_file;
    }
    if (!strcmp(name, "ML/SETTINGS/PROFILES.TXT"))
    {
        if (cam.recording) FAIL("PROFILES.TXT written while recording");
        if (fail_profiles_write) return 0;
        profiles_written_len = 0; profiles_written[0] = 0; profiles_writes++;
        the_file.open = 1; the_file.profiles = 1;
        return (FILE *) &the_file;
    }
    if (strcmp(name, "ML/LOGS/MOTHLOG.TXT")) { FAIL("unexpected file %s", name); return 0; }
    the_file.profiles = 0;
    logfile_len = 0; logfile_writes++;
    if (cam.recording) logfile_writes_recording++;
    the_file.open = 1;
    return (FILE *) &the_file;
}
int FIO_WriteFile(FILE * f, const void * p, size_t n)
{
    if (the_file.profiles == 2) return n;
    if (the_file.profiles == 3)
    {
        if (n > sizeof(play_req) - 1 - play_req_len) n = sizeof(play_req) - 1 - play_req_len;
        memcpy(play_req + play_req_len, p, n); play_req_len += n; play_req[play_req_len] = 0;
        return n;
    }
    if (the_file.profiles)
    {
        if (n > sizeof(profiles_written) - 1 - profiles_written_len) n = sizeof(profiles_written) - 1 - profiles_written_len;
        memcpy(profiles_written + profiles_written_len, p, n); profiles_written_len += n;
        profiles_written[profiles_written_len] = 0;
        return n;
    }
    if (n > sizeof(logfile) - logfile_len) n = sizeof(logfile) - logfile_len;
    memcpy(logfile + logfile_len, p, n); logfile_len += n; logfile[logfile_len < (int)sizeof(logfile) ? logfile_len : (int)sizeof(logfile) - 1] = 0;
    return n;
}
void FIO_CloseFile(FILE * f) { if (f == (FILE *) &reader) { reader.open = 0; return; } if (the_file.profiles == 1) profiles_txt = profiles_written; the_file.open = 0; the_file.profiles = 0; }

/* ---------------------------------------------------------------- Canon */

static void canon_redraw_screen(void)
{
    /* Canon's LiveView overlay: wipes the BMP layer, draws its top and bottom info */
    memset(bmp, 0, BMPPITCH * 480);
    for (int y = 0; y < 40; y++) memset(bmp + y * BMPPITCH, 0x01, 720);
    for (int y = 440; y < 480; y++) memset(bmp + y * BMPPITCH, 0x01, 720);
    canon_redraws++;
    if (overlay_active) canon_redraws_open++;
}

static int canon_af_in_x5, canon_moved_window;
static uint32_t sw1_since;   /* what Crop Mood users suffer without Mothlight */
static int crop_mood_x5_idle(void);
static void canon_receive(int code)
{
    canon_got[code & 0xff]++;
    if (code == 0x50 && crop_mood_x5_idle() && !cam.manual_focus && cam.lens.lens_exists) canon_af_in_x5++;
    if (code == 0x70 && crop_mood_x5_idle()) canon_moved_window++;
    /* housekeeping: card door, power checks, "info changed", "press button something" (0x54
       follows every press on the real camera, and UP stayed smooth there) -- no redraw */
    if (code == 93 || code == 91 || code == 97 || code == 105 || code == 0x54) return;
    if (overlay_active) canon_key_redraws_open++;
    if (!cam.lv_on || cam.playback) return;
    switch (code)
    {
        case 0x1E:  /* movie button: Canon's own H.264 */
            if (cam.movie) { cam.recording = !cam.recording; cam.raw = 0; if (cam.recording) h264_recordings++; }
            break;
        case 0x0B:  /* PLAY */
            cam.playback = 1; cam.lv_on = 0;
            return;
        case 0x06:  /* MENU */
            cam.canon_menu = 1;
            break;
        case 93: case 91: case 97:  /* card door, lock, power checks: housekeeping only */
            return;
    }
    canon_redraw_screen();
}

/* ---------------------------------------------------------------- the GUI task: buttons */

struct keymap_entry { int canon, portable; };
static const struct keymap_entry eosm_keys[] = {
    { 0x02, MODULE_KEY_WHEEL_LEFT }, { 0x03, MODULE_KEY_WHEEL_RIGHT },
    { 0x04, MODULE_KEY_PRESS_SET }, { 0x05, MODULE_KEY_UNPRESS_SET },
    { 0x06, MODULE_KEY_MENU }, { 0x07, MODULE_KEY_INFO }, { 0x0B, MODULE_KEY_PLAY },
    { 0x0D, MODULE_KEY_TRASH }, { 0x1E, MODULE_KEY_REC },
    { 0x2a, MODULE_KEY_PRESS_UP }, { 0x26, MODULE_KEY_PRESS_RIGHT },
    { 0x28, MODULE_KEY_PRESS_LEFT }, { 0x2c, MODULE_KEY_PRESS_DOWN },
    { 0x50, MODULE_KEY_PRESS_HALFSHUTTER }, { 0x51, MODULE_KEY_UNPRESS_HALFSHUTTER },
    { 0x6f, MODULE_KEY_TOUCH_1_FINGER }, { 0x70, MODULE_KEY_UNTOUCH_1_FINGER },
    { 0x76, MODULE_KEY_TOUCH_2_FINGER }, { 0x77, MODULE_KEY_UNTOUCH_2_FINGER },
};
enum {
    K_WL = 0x02, K_WR = 0x03, K_SET = 0x04, K_SET_UP = 0x05, K_MENU = 0x06, K_INFO = 0x07,
    K_PLAY = 0x0B, K_REC = 0x1E, K_UP = 0x2a, K_UP_UP = 0x2b, K_RIGHT = 0x26, K_RIGHT_UP = 0x27,
    K_LEFT = 0x28, K_LEFT_UP = 0x29, K_DOWN = 0x2c, K_DOWN_UP = 0x2d, K_HALF = 0x50, K_HALF_UP = 0x51,
    K_TOUCH = 0x6f, K_UNTOUCH = 0x70, K_TOUCH2 = 0x76, K_DOOR = 93, K_OLC = 105, K_UNHANDLED = 0x54,
    K_QSET = 0x1d,
};

static int canon_code(int portable_key)
{
    for (int i = 0; i < COUNT(eosm_keys); i++)
        if (eosm_keys[i].portable == portable_key) return eosm_keys[i].canon;
    return -1;
}

static int portable(int canon)
{
    for (int i = 0; i < COUNT(eosm_keys); i++)
        if (eosm_keys[i].canon == canon) return eosm_keys[i].portable;
    return 0;                               /* what module.c hands modules for unknown codes */
}

/* one button event through ML: core first (two-finger tap opens ML's menu), then modules */
static void gui_event_x(int canon, int fake)
{
    if (canon == 0x50 && !fake) { cam.half = 1; sw1_since = now_ms; }
    if (canon == 0x51 && !fake) cam.half = 0;
    /* ML core: two-finger tap opens ML's menu, but only from idle LiveView (handle_ml_menu_erase) */
    if (canon == K_TOUCH2 && cam.lv_on && !cam.menu_shown && gui_state == 0) { cam.menu_shown = 1; return; }
    if (cam.menu_shown) return;             /* ML's menu eats everything while it is open */

    /* MLV Play (patched, loaded before moth_ui): PLAY in idle movie LiveView with RAW on */
    if (canon == K_PLAY && cam.lv_on && cam.movie && gui_state == 0 && menu_val("Movie", "RAW video") == 1)
    {
        if (play_req_len)
        {
            char * nl = strchr(play_req, '\n'); if (nl) *nl = 0;
            snprintf(mlv_played, sizeof(mlv_played), "%s", play_req);
            play_req_len = 0; mlv_play_runs++;
            return;
        }
        if (!cfg_clips) { mlv_played[0] = 0; mlv_play_runs++; return; }
        /* Mothlight's clip list: go on to the modules */
    }

    /* MLV Lite (loaded before moth_ui): takes REC only in idle LiveView, with RAW video on */
    if (canon == K_REC && cam.lv_on && cam.movie && gui_state == 0 && menu_val("Movie", "RAW video") == 1)
    {
        cam.recording = !cam.recording; cam.raw = 1;
        if (cam.recording) raw_recordings++;
        if (cam.recording && cam.dispsize == 10) rec_at_x10++;
        return;
    }

    static struct event ev;                 /* static: low address, like the camera's 32-bit pointers */
    ev.type = 0; ev.param = canon; ev.obj = 0; ev.arg = fake ? FAKE_BTN : 0;
    /* module.c calls a module's handlers in the order the module declares them:
       moth_ui declares the raw one first (it logs every event) */
    if (!cam_CBR_KEYPRESS_RAW((unsigned int)(uintptr_t)&ev)) return;
    if (!cam_CBR_KEYPRESS(portable(canon))) return;
    canon_receive(canon);
}
static void gui_event(int canon) { gui_event_x(canon, 0); }

/* EOS M: ML's core takes DOWN before any module (a 0.5 s hold opens ML's menu; the camera has
   no trash key). It draws a progress ring while held; on a short press it calls redraw() --
   Canon repaints its screen -- and then sends a fake DOWN press and release to the modules. */
static int ml_longpress_down = 1;
static int ml_redraws;
static void press_down_eosm(int hold_ms)
{
    if (!ml_longpress_down || !cam.lv_on || cam.menu_shown || cam.playback || gui_state != 0)
    {
        gui_event(K_DOWN); run_for(hold_ms); gui_event(K_DOWN_UP); return;
    }
    int t = 0;
    ring_active = 1;
    do
    {
        for (int y = 328; y < 358; y++) memset(bmp + y * BMPPITCH + 655, 0x0E, 30);   /* the ring */
        run_for(20); t += 20;
    } while (t < hold_ms && t < 500);
    if (t >= 500) { cam.menu_shown = 1; ring_active = 0; return; }  /* long press: ML menu */
    run_for(10);
    canon_redraw_screen(); ml_redraws++;                            /* redraw() erases the ring */
    ring_active = 0;
    gui_event_x(K_DOWN, 1);
    gui_event_x(K_DOWN_UP, 1);
}

/* A press and its release, the way the EOS M sends them -- taken from MOTHLOG.TXT written
   by the real camera:
     - every button press is followed by Canon's 0x54 ("press button something")
     - DOWN and SET go through ML's long-press check (redraw, then a fake press + release)
     - RIGHT never reaches ML in LiveView: Canon acts on it and only "info changed" (0x69)
       and the release (0x27) arrive */
static int step(int * i, int n, int sign, int lo, int hi);
static void lens_sync(void);
static void press_set_eosm(int hold_ms)
{
    if (gui_state != 0)
    {
        /* LiveView not idle for ML: no long-press check, SET arrives as its real code */
        gui_event(K_QSET); gui_event(K_UNHANDLED); run_for(hold_ms); gui_event(K_SET_UP);
        return;
    }
    run_for(hold_ms);
    gui_event(K_SET_UP);                    /* the real release arrives first ... */
    gui_event(K_UNHANDLED);
    canon_redraw_screen(); ml_redraws++;    /* ... ML's long-press check redraws ... */
    gui_event_x(K_SET, 1);                  /* ... and sends a fake SET */
    gui_event_x(K_SET_UP, 1);
}

static void tap(int canon, int hold_ms)
{
    if ((canon == K_WR || canon == K_WL) && cam.lv_on && !cam.menu_shown)
    {
        /* Canon keeps the rear dial in LiveView: manual movie exposure -> shutter */
        if (cam.lens.raw_shutter) { step(&sh_i, COUNT(shutters), canon == K_WR ? 1 : -1, 0, COUNT(shutters) - 1); lens_sync(); }
        gui_event(K_UNHANDLED); gui_event(K_OLC);
        run_for(hold_ms);
        return;
    }
    if (canon == K_DOWN) { press_down_eosm(hold_ms); gui_event(K_UNHANDLED); return; }
    if (canon == K_SET && cam.lv_on && !cam.menu_shown) { press_set_eosm(hold_ms); return; }
    if (canon == K_RIGHT && cam.lv_on && !cam.menu_shown)
    {
        gui_event(K_UNHANDLED); gui_event(K_OLC); run_for(hold_ms); gui_event(K_RIGHT_UP);
        return;
    }
    gui_event(canon);
    if (canon == K_UP || canon == K_LEFT) gui_event(K_UNHANDLED);
    run_for(hold_ms);
    switch (canon)
    {
        case K_UP: gui_event(K_UP_UP); break;
        case K_LEFT: gui_event(K_LEFT_UP); break;
        case K_RIGHT: gui_event(K_RIGHT_UP); break;
        case K_SET: gui_event(K_SET_UP); break;
        case K_TOUCH: gui_event(K_UNTOUCH); break;
        case K_HALF: gui_event(K_HALF_UP); break;
    }
}

/* ---------------------------------------------------------------- world ticking */

static uint32_t touch_raw = 0xffffffff;     /* what Canon keeps at 0x4D868 */
static int peeks_bad_addr;
uint32_t camsim_peek32(uint32_t addr) { if (addr != 0x4D868) peeks_bad_addr++; return touch_raw; }
static void touch_at(int x, int y)
{
    touch_raw = ((uint32_t)y << 16) | (uint32_t)x;
    gui_event(K_TOUCH); run_for(60); gui_event(K_UNTOUCH); run_for(60);
}

static int sent_q[16], sent_n;
int module_send_keypress(int module_key)
{
    int c = canon_code(module_key);
    if (c >= 0 && sent_n < 16) sent_q[sent_n++] = c;
    return 0;
}

static uint32_t lv_back_at, last_periodic;
static void world_tick(void)
{
    /* keys a module sent with module_send_keypress arrive as fake events, after the current one */
    while (sent_n)
    {
        int c = sent_q[0];
        memmove(sent_q, sent_q + 1, --sent_n * sizeof(int));
        gui_event_x(c, 1);
    }
    if (lv_back_at && now_ms >= lv_back_at)
    {
        cam.lv_on = 1; lv_back_at = 0;
        canon_redraw_screen();              /* LiveView comes back with Canon's screen */
        /* the recorder now produces the frame of whatever profile the menus describe */
        raw_w = raw_h = 0;
        for (int i = 0; i < profile_count; i++)
        {
            const struct rec_profile * p = &profiles[i];
            static const char * pe[] = { "", "Preset:", "Preset: ", "Preset:  " };
            if (p->mood == menu_val("Movie", "Crop mood") && p->mood > 0
                && p->preset == menu_val("Crop mood", pe[(int)p->mood])
                && p->ratio == menu_val("Crop mood", "Aspect ratio:")
                && p->fps == menu_val("Crop mood", "Framerate:")
                && p->bits == menu_val("Crop mood", "Bit-depth"))
            { raw_w = p->nominal.width; raw_h = p->nominal.height; break; }
        }
    }
    /* crop_rec's own task, ~10 Hz: hides Canon's layer in x5 (with a clrscr) */
    static uint32_t last_crop_check;
    if (now_ms - last_crop_check >= 100)
    {
        last_crop_check = now_ms;
        if (crop_wants_hidden() && cam.front_enabled) { cam.front_enabled = 0; memset(bmp, 0, BMPPITCH * 480); }
    }
    if (canon_periodic_ms && cam.lv_on && now_ms - last_periodic >= (uint32_t)canon_periodic_ms)
    {
        last_periodic = now_ms;
        canon_redraw_screen();
    }
}

static void lv_refresh(void)
{
    if (!cam.lv_on) return;
    canon_redraw_screen();                  /* LiveView going down redraws Canon's screen */
    cam.lv_on = 0;
    lv_back_at = now_ms + lv_pause_ms;
}

/* ---------------------------------------------------------------- fake ML API */

int is_movie_mode(void) { return cam.movie; }
int gui_menu_shown(void) { return cam.menu_shown; }
int display_is_on(void) { return cam.display_on; }
void set_lv_zoom(int z) { cam.dispsize = z; lv_refresh(); cam_block(stall_zoom_ms ? stall_zoom_ms : 600); }
int fps_get_current_x1000(void) { return 23976; }

/* Canon's AF point, in its own coordinates (the whole sensor) */
#define AF_W 5184
#define AF_H 3456
static int af_cx = AF_W / 2, af_cy = AF_H / 2, af_moves, af_moves_rec;
static int sw1_on, af_runs, af_at_x10, af_at_x5;
static int af_never_confirms;
static int crop_mood_x5_idle(void) { return menu_val("Movie", "Crop mood") > 0 && cam.dispsize == 5 && !cam.recording && cam.lv_on; }
void get_afframe_sensor_res(int * W, int * H) { if (W) *W = AF_W; if (H) *H = AF_H; }
void get_afframe_pos(int W, int H, int * x, int * y) { *x = af_cx * W / AF_W; *y = af_cy * H / AF_H; }
void move_lv_afframe(int dx, int dy)
{
    if (cam.recording) af_moves_rec++;
    af_cx = af_cx + dx; af_cy = af_cy + dy;
    af_cx = af_cx < 200 ? 200 : af_cx > AF_W - 200 ? AF_W - 200 : af_cx;
    af_cy = af_cy < 150 ? 150 : af_cy > AF_H - 150 ? AF_H - 150 : af_cy;
    af_moves++;
    cam_block(40);
}
void SW1(int v, int wait)
{
    if (v && !sw1_on) { sw1_since = now_ms; af_runs++; if (cam.dispsize == 10) af_at_x10++; else af_at_x5++; }
    sw1_on = v;
    cam_block(wait);
}
int is_manual_focus(void) { return cam.manual_focus; }
int get_halfshutter_pressed(void) { return cam.half || sw1_on; }
int get_focus_confirmation(void) { return (sw1_on || cam.half) && !af_never_confirms && now_ms - sw1_since > 400; }
void gui_stop_menu(void) { cam.menu_shown = 0; }

int menu_get_value_from_script(const char * m, const char * e)
{
    struct mval * v = find_menu(m, e);
    return v && v->exists ? v->value : INT_MIN;
}
int menu_set_value_from_script(const char * m, const char * e, int value)
{
    struct mval * v = find_menu(m, e);
    if (!v || !v->exists) return INT_MIN;
    int changed = v->value != value;
    v->value = value;
    if (changed && !strcmp(m, "Crop mood")) lv_refresh();
    return value;
}
const char * menu_get_str_value_from_script(const char * m, const char * e, struct menu_display_info * info)
{
    if (!strcmp(e, "RAW video"))
        snprintf(info->value, sizeof(info->value), "%s, %dx%d", menu_val(m, e) ? "ON" : "OFF", raw_w, raw_h);
    else snprintf(info->value, sizeof(info->value), "%d", menu_get_value_from_script(m, e));
    return info->value;
}

uint8_t * bmp_vram_real(void) { return bmp; }
/* Crop Mood keeps Canon's overlay layer off in its x5 modes (crop_rec.c: "disable Canon
   overlays in x5 mode for cleaner preview"); turning it on there makes Canon's icons flash
   up until crop_rec hides them again */
static int canon_layer_flashes;
static int crop_wants_hidden(void)
{
    return menu_val("Movie", "Crop mood") > 0 && cam.dispsize == 5 && !cam.recording && cam.lv_on;
}
int canon_gui_front_buffer_disabled(void) { return !cam.front_enabled; }
void canon_gui_disable_front_buffer(void) { cam.front_enabled = 0; begin_pending = 1; }
void canon_gui_enable_front_buffer(int x)
{
    if (crop_wants_hidden() && !cam.front_enabled) canon_layer_flashes++;
    cam.front_enabled = 1;
}
void clrscr(void) { memset(bmp, 0, BMPPITCH * 480); }
uint32_t get_ms_clock(void) { return now_ms; }
void redraw(void) { if (cam.lv_on && !cam.playback) canon_redraw_screen(); }
void bmp_fill(uint8_t col, int x, int y, int w, int h)
{
    if (x < 0 || y < 0 || x + w > 720 || y + h > 480) { FAIL("bmp_fill out of the screen"); return; }
    for (int r = y; r < y + h; r++) memset(bmp + r * BMPPITCH + x, col, w);
}

/* the lens and exposure, Canon-like: third stops, limits, auto exposure, manual lenses.
   Like ML's steppers, each change waits for Canon to confirm (~40 ms). */

static void lens_sync(void)
{
    cam.lens.raw_shutter = auto_expo ? 0 : 56 + sh_i * 3;
    cam.lens.raw_aperture = auto_expo || !cam.lens.lens_exists ? 0 : 10 + ap_i * 3;
    cam.lens.aperture = cam.lens.raw_aperture ? fstops10[ap_i] : 0;
    cam.lens.raw_aperture_min = 10 + lens_min_i * 3;
    cam.lens.raw_aperture_max = 10 + lens_max_i * 3;
}
static int step(int * i, int n, int sign, int lo, int hi)
{
    int j = *i + sign;
    if (j < lo || j > hi || j >= n) return 0;
    *i = j; lens_changes++; return 1;
}
void shutter_toggle(void * priv, int sign) { if (!cam.lens.raw_shutter) return; cam_block(40); step(&sh_i, COUNT(shutters), sign, 0, COUNT(shutters) - 1); lens_sync(); }
void aperture_toggle(void * priv, int sign) { if (!cam.lens.lens_exists || !cam.lens.raw_aperture) return; cam_block(40); step(&ap_i, COUNT(fstops10), sign, lens_min_i, lens_max_i); lens_sync(); }
void iso_toggle(void * priv, int sign)
{
    if (auto_expo) return;                  /* Canon keeps ISO automatic in auto movie exposure */
    cam_block(40);
    if (!cam.lens.raw_iso) { iso_i = 0; cam.lens.raw_iso = 72; lens_changes++; return; }
    if (step(&iso_i, COUNT(isos), sign, 0, COUNT(isos) - 1)) cam.lens.raw_iso = 72 + iso_i * 3;
}
void kelvin_toggle(void * priv, int sign)
{
    int k = cam.lens.wb_mode == WB_KELVIN ? (int)cam.lens.kelvin : 5200;
    int st = k + sign * 100 > 7000 ? 500 : 100;
    k = (k / st) * st + sign * st;
    if (k < 1500) k = 1500;
    if (k > 15000) k = 15000;
    cam_block(40);
    if (cam.lens.wb_mode != WB_KELVIN || (int)cam.lens.kelvin != k) lens_changes++;
    cam.lens.wb_mode = WB_KELVIN; cam.lens.kelvin = k;
}
int get_current_shutter_reciprocal_x1000(void) { return shutters[sh_i] * 1000; }
const char * lens_format_shutter_reciprocal(int x1000, int digits) { static char b[16]; snprintf(b, sizeof(b), "1/%d", x1000 / 1000); return b; }
const char * lens_format_aperture(int raw)
{
    static char b[16];
    int i = (raw - 10) / 3;
    if (i < 0) i = 0;
    if (i >= COUNT(fstops10)) i = COUNT(fstops10) - 1;
    snprintf(b, sizeof(b), "f/%d.%d", fstops10[i] / 10, fstops10[i] % 10);
    return b;
}
const char * lens_format_iso(int raw) { static char b[16]; if (!raw) return "Auto"; snprintf(b, sizeof(b), "%d", isos[(raw - 72) / 3]); return b; }

/* the heap: sizes are kept in a header so leaks can be counted */
void * cam_malloc(size_t n)
{
    if (fail_big_malloc && n >= 100000) { fail_big_malloc = 0; return 0; }
    size_t * p = malloc(n + 16);
    if (!p) return 0;
    p[0] = n; heap_live += n; if (heap_live > heap_peak) heap_peak = heap_live;
    return (char *)p + 16;
}
void cam_free(void * q)
{
    if (!q) return;
    size_t * p = (size_t *)((char *)q - 16);
    heap_live -= p[0];
    free(p);
}

/* the card: a DCIM folder with whatever MLV files the scenario puts there */
struct fake_file { char name[64]; uint32_t size, ts; int frames, w, h, audio; };
static struct fake_file dcim[16];
static int dcim_n;
const char * get_dcim_dir(void) { return "B:/DCIM/100CANON/"; }
static struct fio_dirent the_dirent;
struct fio_dirent * FIO_FindFirstEx(const char * dir, struct fio_file * f)
{
    if (!dcim_n) return 0;
    the_dirent.i = 0;
    snprintf(f->name, sizeof(f->name), "%s", dcim[0].name); f->mode = 0; f->timestamp = dcim[0].ts; f->size = dcim[0].size;
    return &the_dirent;
}
int FIO_FindNextEx(struct fio_dirent * d, struct fio_file * f)
{
    if (++d->i >= dcim_n) return 1;
    snprintf(f->name, sizeof(f->name), "%s", dcim[d->i].name); f->mode = 0; f->timestamp = dcim[d->i].ts; f->size = dcim[d->i].size;
    return 0;
}
void FIO_FindClose(struct fio_dirent * d) {}
/* reading clip headers: a made-up MLVI + RAWI for each .MLV in the folder */
static uint8_t mlv_hdr[256];
static void make_mlv_header(const struct fake_file * c)
{
    memset(mlv_hdr, 0, sizeof(mlv_hdr));
    uint8_t * p = mlv_hdr;
    #define W32(o, v) do { uint32_t _v = (v); p[o] = _v; p[o+1] = _v >> 8; p[o+2] = _v >> 16; p[o+3] = _v >> 24; } while (0)
    #define W16(o, v) do { uint32_t _v = (v); p[o] = _v; p[o+1] = _v >> 8; } while (0)
    memcpy(p, "MLVI", 4); W32(4, 52); memcpy(p + 8, "v2.0", 4);
    W16(32, 0x21); W16(34, c->audio ? 1 : 0); W32(36, c->frames); W32(44, 23976); W32(48, 1000);
    p += 52;
    memcpy(p, "RAWI", 4); W32(4, 180); W16(16, c->w); W16(18, c->h);
    #undef W32
    #undef W16
}
FILE * FIO_OpenFile(const char * name, unsigned mode)
{
    const char * base = strrchr(name, '/'); base = base ? base + 1 : name;
    for (int i = 0; i < dcim_n; i++)
        if (!strcmp(dcim[i].name, base)) { reader.open = 1; reader.idx = i; reader.pos = 0; make_mlv_header(&dcim[i]); return (FILE *) &reader; }
    return 0;
}
int FIO_SeekSkipFile(FILE * f, int pos, int whence) { reader.pos = pos; return pos; }
int FIO_ReadFile(FILE * f, void * p, size_t n)
{
    if (reader.pos >= (int)sizeof(mlv_hdr)) return 0;
    if (n > sizeof(mlv_hdr) - reader.pos) n = sizeof(mlv_hdr) - reader.pos;
    memcpy(p, mlv_hdr + reader.pos, n); reader.pos += n;
    return n;
}
static char removed[256];
static int removals;
int FIO_RemoveFile(const char * name)
{
    if (cam.recording) FAIL("a file was deleted while recording");
    const char * base = strrchr(name, '/'); base = base ? base + 1 : name;
    for (int i = 0; i < dcim_n; i++)
        if (!strcmp(dcim[i].name, base))
        {
            strncat(removed, base, sizeof(removed) - strlen(removed) - 2); strcat(removed, " ");
            dcim[i] = dcim[--dcim_n]; removals++;
            return 0;
        }
    return -1;
}

int FIO_GetFileSize(const char * path, uint32_t * size)
{
    if (!strcmp(path, "ML/SETTINGS/MOTHWEL.TXT")) { *size = 70; return welcome_marker_on_card ? 0 : -1; }
    if (!strcmp(path, "ML/SETTINGS/MOTHPLAY.REQ")) { *size = play_req_len; return play_req_len ? 0 : -1; }
    const char * base = strrchr(path, '/');
    base = base ? base + 1 : path;
    for (int i = 0; i < dcim_n; i++)
        if (!strcmp(dcim[i].name, base)) { *size = dcim[i].size; return 0; }
    return -1;
}
void NotifyBox(int ms, const char * fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vsnprintf(notify_last, sizeof(notify_last), fmt, ap);
    va_end(ap);
    notify_count++;
}
static struct card_info the_card;
struct card_info * get_shooting_card(void) { return &the_card; }
int get_free_space_32k(struct card_info * c) { return 32 * 1000 * 30; }

const char * get_config_dir(void) { return "ML/SETTINGS/"; }
void * read_entire_file(const char * path, int * size)
{
    if (!profiles_txt) { *size = 0; return 0; }
    int n = strlen(profiles_txt);
    char * b = cam_malloc(n);
    memcpy(b, profiles_txt, n);
    *size = n;
    return b;
}
void fio_free(void * p) { cam_free(p); }
int is_camera(const char * m, const char * fw) { return !strcmp(m, "EOSM") && !strcmp(fw, "2.0.2"); }

static struct menu_entry * menu_root;
static struct menu_entry * menu_child(const char * name);
static int menu_n;
void menu_add(const char * parent, struct menu_entry * e, int n) { menu_root = e; menu_n = n; }
static struct lvinfo_item * lv_items;
void lvinfo_add_items(struct lvinfo_item * items, int n) { lv_items = items; }

/* ---------------------------------------------------------------- invariants after every frame */

extern void camsim_dump(const uint8_t * bmp960, const char * dir, int n);

static int was_overlay;
static void post_frame_checks(void)
{
    frames_checked++;
    int settled = ov_settled();
    if (settled && back_buf)
    {
        for (int y = ov_owned_top(); y < UI_H; y++)
            if (row_differs(y))
            {
                settled_mismatch++;
                int x = 0; while (x < UI_W && bmp[y * BMPPITCH + x] == back.px[y * back.pitch + x]) x++;
                int rows_bad = 0; for (int yy = 0; yy < UI_H; yy++) rows_bad += !!memcmp(bmp + yy * BMPPITCH, back.px + yy * back.pitch, UI_W);
                FAIL("screen differs from the panel on row %d (x %d: screen %02x panel %02x; %d rows differ) state %d half %d prev %d-%d lv %d",
                     y, x, bmp[y * BMPPITCH + x], back.px[y * back.pitch + x], rows_bad, panel.state,
                     tween_value(&panel.reveal, now_ms), panel.prev_y0, panel.prev_y1, cam.lv_on);
                break;
            }
    }
    if (getenv("TRACE2") && now_ms > (uint32_t)atoi(getenv("TRACE2"))) fprintf(stderr, "    f @%u ov %d mode %d qb %d pfb %d st %d half %d lv %d movie %d menu %d disp %d apply %d hb %d kr %d ring %d/%d bp %d\n", now_ms, overlay_active, ov_mode, qbar.state, panel_from_bar, panel.state, tween_value(&panel.reveal, now_ms), cam.lv_on, cam.movie, cam.menu_shown, cam.display_on, apply_phase, handing_back, keys_released, key_head, key_tail, begin_pending);
    if (overlay_active) begin_pending = 0;
    /* what Global Draw must be once closed: as it was, or as Settings > ML overlays chose */
    if (overlay_active && saved_global_draw >= 0) global_draw_at_start = saved_global_draw;
    if (overlay_active) { if (ov_mode == OV_SETTINGS) soak_settings_frames++; if (cam.dispsize == 10) soak_x10_frames++; }
    if (!overlay_active && !begin_pending && fj_state == FJ_IDLE && gui_state == MOTH_GUI_STATE)
        FAIL("ML's gui_state left at Mothlight's value with the panel closed");
    if (was_overlay && !overlay_active && !begin_pending)
    {
        /* the panel just handed the screen back */
        EXPECT(!cam.lv_on || cam.recording || cam.front_enabled == !crop_wants_hidden(), "Canon's layer left %s (Crop Mood x5 wants it %s)",
               cam.front_enabled ? "on" : "off", crop_wants_hidden() ? "off" : "on");
        EXPECT(canon_layer_flashes == 0, "Canon's overlay layer turned on in Crop Mood x5 (%d times): its icons flash up",
               canon_layer_flashes);
        EXPECT(back_buf == 0, "panel buffer (338 KB) not freed after close");
        EXPECT(menu_val("Overlay", "Global Draw") == global_draw_at_start,
               "Global Draw left at %d (was %d)", menu_val("Overlay", "Global Draw"), global_draw_at_start);
    }
    if (getenv("TRACE") && was_overlay != overlay_active) fprintf(stderr, "  trace @%u overlay %d lv %d apply %d state %d keys_released %d menu %d rec %d\n", now_ms, overlay_active, cam.lv_on, apply_phase, panel.state, keys_released, cam.menu_shown, cam.recording);
    was_overlay = overlay_active;
    if (dump_dir) camsim_dump(bmp, dump_dir, dump_n++);
}

/* ---------------------------------------------------------------- scenario helpers */

static int want_welcome;
static void boot(void)
{
    memset(&cam, 0, sizeof(cam));
    cam.lv_on = 1; cam.movie = 1; cam.display_on = 1; cam.front_enabled = 1; cam.dispsize = 1; cam.battery = 2;
    cam.lens.focal_len = 22;
    cam.lens.lens_exists = 1;
    cam.lens.raw_iso = 72 + iso_i * 3;
    cam.lens.wb_mode = WB_KELVIN; cam.lens.kelvin = 5600;
    lens_sync();
    if (!getenv("KEEP_KEY")) cfg_key = 2;   /* most scenarios test the profile panel; bar ones set 1 */
    if (!want_welcome) cfg_welcomed = 1;    /* the first-start page has its own scenario */
    bmp = cam_malloc(BMPPITCH * 480);
    heap_live = 0;                          /* the screen is not the module's */
    memset(bmp, 0, BMPPITCH * 480);
    EXPECT(cam_mod_init() == 0, "module init failed");
    run_for(300);
}

static int panel_open_now(void) { return overlay_active && panel.state != PS_CLOSED; }

static void open_panel(void)
{
    tap(K_INFO, 0);
    run_for(800);
    EXPECT(panel_open_now() && panel.state == PS_OPEN, "INFO did not open the panel (state %d)", panel.state);
}

static void close_panel(void)
{
    /* INFO on a list opened from the quick bar goes back to the bar; INFO again closes */
    for (int i = 0; i < 3 && overlay_active; i++) { tap(K_INFO, 0); run_for(800); }
    EXPECT(!overlay_active, "panel did not close");
}

static void finish(void)
{
    if (overlay_active) close_panel();
    run_for(300);
    EXPECT(heap_live == 0, "heap: %ld bytes still allocated (leak)", heap_live);
}

/* ---------------------------------------------------------------- scenarios */

static void s_basic_navigation(void)
{
    boot(); open_panel();
    int sel0 = panel.sel;
    int r0 = canon_key_redraws_open;
    for (int i = 0; i < 3; i++) { tap(K_DOWN, 80); run_for(120); }
    EXPECT(panel.sel == sel0 + 3, "DOWN x3 moved selection %d -> %d", sel0, panel.sel);
    tap(K_UP, 80); run_for(200);
    EXPECT(panel.sel == sel0 + 2, "UP did not move selection back");
    tap(K_WR, 0); tap(K_WR, 0); run_for(200);
    EXPECT(panel.sel == sel0 + 2 && panel_open_now(), "Canon's dial (shutter) moved the list or closed the panel");
    EXPECT(canon_key_redraws_open == r0, "%d navigation keys leaked to Canon, which redraws on them (the black flash)",
           canon_key_redraws_open - r0);
    EXPECT(!canon_got[K_DOWN] && !canon_got[K_DOWN_UP] && !canon_got[K_UP_UP], "navigation keys leaked to Canon");
    EXPECT(panel_open_now(), "panel closed while navigating");
    finish();
}

static void s_double_info(void)
{
    boot();
    tap(K_INFO, 0);
    for (int i = 0; i < 100 && !(begin_pending && !overlay_active); i++) run_for(5);
    EXPECT(begin_pending && !overlay_active, "could not catch the start-up window");
    tap(K_INFO, 0);                         /* a second INFO lands mid start-up */
    run_for(900);
    EXPECT(panel_open_now(), "double INFO: panel not open");
    close_panel();
    run_for(1500);
    EXPECT(!overlay_active, "double INFO: the panel reopened by itself after closing");
    finish();
}


static int find_profile(int mood);
static void select_profile(int idx);

static int screen_says(const char * text)
{
    ui_text_drawn_reset();
    if (ov_mode == OV_BAR) qbar.full_redraw = 1;
    else if (ov_mode == OV_SETTINGS) sets.full_redraw = 1;
    else panel.full_redraw = 1;
    run_for(60);
    return strstr(ui_text_drawn, text) != 0;
}

static void s_down_eosm(void)
{
    boot(); open_panel();
    int s0 = panel.sel;
    for (int i = 0; i < 5; i++) { tap(K_DOWN, 60 + i * 40); run_for(150); }
    EXPECT(ml_redraws == 0, "ML's long-press check ran %d times with the panel open (the black flash)", ml_redraws);
    EXPECT(panel.sel == (s0 + 5) % profile_count, "DOWN x5 moved %d -> %d", s0, panel.sel);
    EXPECT(panel_open_now(), "panel closed after DOWN");
    printf("    worst overdraw after DOWN: %d ms\n", worst_overdraw);
    /* a long hold is just a long press of DOWN while the panel is open */
    tap(K_DOWN, 700); run_for(100);
    EXPECT(panel_open_now() && !cam.menu_shown, "a long DOWN left the panel");
    close_panel();
    /* with the panel closed, ML's long-press is back: a short DOWN redraws, a long one opens ML's menu */
    tap(K_DOWN, 60); run_for(100);
    EXPECT(ml_redraws == 1, "ML's long-press check did not come back after closing");
    tap(K_DOWN, 700); run_for(100);
    EXPECT(cam.menu_shown, "long DOWN with the panel closed did not open ML's menu");
    cam.menu_shown = 0;
    finish();
}

static void s_apply_no_flash(void)
{
    boot(); open_panel();
    int idx = find_profile(MOOD_1X1);
    select_profile(idx);
    worst_overdraw = 0;
    tap(K_SET, 40);
    run_for(1500);
    EXPECT(overlay_active, "panel closed during apply");
    EXPECT(strstr(panel.toast, "Ready") != 0, "toast: \"%s\"", panel.toast);
    printf("    worst overdraw while applying: %d ms\n", worst_overdraw);
    finish();
}

static void s_close_mid_apply(void)
{
    boot(); open_panel();
    int a = find_profile(MOOD_1X1);
    select_profile(a);
    tap(K_SET, 30); run_for(100);
    tap(K_INFO, 0); run_for(1500);          /* closed while the helper is still applying */
    EXPECT(!overlay_active, "did not close mid-apply");
    EXPECT(cfg_active == a, "an apply that finished after closing was not recorded (active %d)", cfg_active);
    open_panel();
    int b = (a + 1) % profile_count;
    select_profile(b);
    tap(K_SET, 30); run_for(1800);
    EXPECT(cfg_active == b, "second apply after closing mid-apply did not run (active %d)", cfg_active);
    finish();
}

static void s_sd_clock_hint(void)
{
    boot();
    find_menu("Debug", "SD Overclock")->value = 0;
    open_panel();
    select_profile(find_profile(MOOD_1X1));
    EXPECT(screen_says("Debug > SD Overclock"), "no SD Overclock hint with the stock clock:\n%s", ui_text_drawn);
    close_panel();
    find_menu("Debug", "SD Overclock")->value = 1;
    open_panel();
    select_profile(find_profile(MOOD_1X1));
    EXPECT(!screen_says("SD Overclock"), "SD Overclock hint shown although it is on");
    finish();
}

static void s_event_log(void)
{
    boot(); open_panel();
    tap(K_LEFT, 40); tap(K_RIGHT, 40); tap(K_DOWN, 60); tap(K_UP, 40);
    close_panel();
    tap(K_UP, 40);                          /* after closing: logged as "[panel closed]" */
    run_for(3500);
    EXPECT(strstr(logfile, "[panel closed]"), "keys after closing not logged:\n%s", logfile);
    EXPECT(logfile_writes == 1, "log written %d times", logfile_writes);
    run_for(3500);                          /* it keeps listening 3 s after closing, then writes */
    EXPECT(strstr(logfile, "OPEN") && strstr(logfile, "button 0x28") && strstr(logfile, "button 0x69")
           && strstr(logfile, "button 0x2c") && strstr(logfile, "CLOSE closed by a key"), "log content:\n%s", logfile);
    EXPECT(!strstr(logfile, "  u  "), "log times did not print");
    /* never written while recording */
    open_panel();
    tap(K_REC, 0);
    run_for(2000);
    EXPECT(logfile_writes_recording == 0, "log written while recording");
    cam.recording = 0;
    run_for(3500);
    EXPECT(logfile_writes == 2 && strstr(logfile, "CLOSE closed by REC/PLAY/shutter"), "log after REC:\n%s", logfile);
    finish();
}

static void s_gui_state(void)
{
    boot(); open_panel();
    EXPECT(gui_state == MOTH_GUI_STATE, "gui_state not held while open (%d)", gui_state);
    int r0 = ml_redraws, s0 = panel.sel;
    for (int i = 0; i < 4; i++) { tap(K_DOWN, 80); run_for(120); }
    EXPECT(ml_redraws == r0, "ML's long-press check still ran on DOWN (%d redraws)", ml_redraws - r0);
    EXPECT(panel.sel == (s0 + 4) % profile_count, "DOWN x4 with the state held: sel %d -> %d", s0, panel.sel);
    select_profile(find_profile(MOOD_1X1));
    tap(K_SET, 60); run_for(2000);
    EXPECT(ml_redraws == r0, "ML's long-press check ran on SET");
    EXPECT(cfg_active == find_profile(MOOD_1X1), "SET (real 0x1d) did not apply (active %d)", cfg_active);
    run_for(3000);
    EXPECT(!overlay_active && gui_state == 0, "after closing: overlay %d, gui_state %d", overlay_active, gui_state);
    /* Canon changes state while the panel is up: our close must not overwrite it */
    open_panel();
    gui_state = 2;
    close_panel();
    EXPECT(gui_state == 2, "closing overwrote a state Canon set meanwhile (%d)", gui_state);
    gui_state = 0;
    finish();
}

static void s_rec_raw_while_open(void)
{
    boot(); open_panel();
    select_profile(find_profile(MOOD_1X1));
    tap(K_SET, 40); run_for(1200);          /* RAW video on */
    tap(K_DOWN, 40); run_for(200);          /* keep the panel open */
    EXPECT(panel_open_now(), "panel not open");
    tap(K_REC, 0); run_for(400);
    EXPECT(raw_recordings == 1 && h264_recordings == 0, "REC with the panel open: %d RAW, %d H.264 recordings",
           raw_recordings, h264_recordings);
    EXPECT(!overlay_active, "panel still up while recording");
    cam.recording = 0;
    run_for(3500);
    finish();
}

static void s_two_finger_while_open(void)
{
    boot(); open_panel();
    tap(K_TOUCH2, 0); run_for(200);
    EXPECT(!overlay_active, "panel still up after a two-finger tap");
    EXPECT(cam.menu_shown, "two-finger tap with the panel open did not open ML's menu");
    cam.menu_shown = 0;
    finish();
}

/* ---------------------------------------------------------------- quick bar */

static void open_bar(void)
{
    cfg_key = 1;
    tap(K_INFO, 0);
    run_for(600);
    EXPECT(overlay_active && ov_mode == OV_BAR && qbar.state == QB_OPEN, "INFO did not open the quick bar (mode %d state %d)", ov_mode, qbar.state);
}

static void bar_select(int item)
{
    for (int i = 0; i < QB_COUNT && qbar.sel != item; i++) { tap(K_SET, 30); run_for(60); }
    EXPECT(qbar.sel == item, "SET did not get to bar item %d (sel %d)", item, qbar.sel);
}

static void s_qbar_basic(void)
{
    boot(); open_bar();
    EXPECT(screen_says("SHUTTER") && screen_says("1/50") && screen_says("f/2.8") && screen_says("800") && screen_says("5600K"),
           "bar does not show the camera's values:\n%s", ui_text_drawn);
    EXPECT(screen_says("SET next"), "no hint after opening");
    /* LiveView above the bar stays untouched (transparent) */
    int dirty_above = 0;
    for (int y = 0; y < QB_STRIP_Y && !dirty_above; y += 3)
        for (int x = 0; x < UI_W; x += 7)
        {
            if (x >= QB_BAT_X0 && x < QB_BAT_X1 && y >= QB_BAT_Y0 && y < QB_BAT_Y1) continue;   /* the battery */
            if (bmp[y * BMPPITCH + x]) { dirty_above = 1; break; }
        }
    EXPECT(!dirty_above, "the bar drew above its area (LiveView should show there)");
    int chip = 0;
    for (int y = QB_BAT_Y0; y < QB_BAT_Y1; y++) for (int x = QB_BAT_X0; x < QB_BAT_X1; x++) chip += bmp[y * BMPPITCH + x] != 0;
    EXPECT(chip > 300, "no battery at the top-left (%d pixels)", chip);
    /* low battery: the chip follows within a refresh, and goes away with the bar */
    cam.battery = 0; run_for(300);
    EXPECT(qbar.bat_drawn == 0, "battery chip did not follow the battery level");
    cam.battery = 2; run_for(300);

    bar_select(QB_SHUTTER);
    tap(K_UP, 30); run_for(200);
    EXPECT(sh_i == 3 && screen_says("1/60"), "UP on SHUTTER: index %d\n%s", sh_i, ui_text_drawn);
    bar_select(QB_APERTURE);
    tap(K_DOWN, 30); tap(K_DOWN, 30); run_for(300);
    EXPECT(ap_i == 4 && screen_says("f/2.2"), "DOWN x2 on APERTURE: index %d", ap_i);
    bar_select(QB_ISO);
    tap(K_UP, 30); run_for(200);
    EXPECT(iso_i == 10 && screen_says("1000"), "UP on ISO: index %d", iso_i);
    bar_select(QB_WB);
    tap(K_DOWN, 30); run_for(200);
    EXPECT(cam.lens.kelvin == 5500 && screen_says("5500K"), "DOWN on WB: %uK", cam.lens.kelvin);
    for (int i = QB_WB; i < QB_COUNT; i++) tap(K_SET, 30);
    run_for(200);
    EXPECT(qbar.sel == QB_SHUTTER, "SET does not wrap around (sel %d)", qbar.sel);
    EXPECT(!canon_got[K_DOWN] && !canon_got[K_UP] && !canon_got[K_QSET], "bar keys leaked to Canon");
    printf("    worst overdraw with the bar: %d ms\n", worst_overdraw);
    tap(K_INFO, 0); run_for(400);
    EXPECT(!overlay_active && gui_state == 0, "INFO did not close the bar cleanly");
    chip = 0;   /* (0x01 is Canon's own overlay, redrawn after closing) */
    for (int y = QB_BAT_Y0; y < QB_BAT_Y1; y++) for (int x = QB_BAT_X0; x < QB_BAT_X1; x++) chip += bmp[y * BMPPITCH + x] > 1;
    EXPECT(chip == 0, "battery chip left on screen after closing (%d pixels)", chip);
    finish();
}

static void s_qbar_dial(void)
{
    boot(); open_bar();
    bar_select(QB_ISO);                     /* whatever is selected, the dial is Canon's shutter */
    tap(K_WR, 0); run_for(150);
    EXPECT(sh_i == 3 && iso_i == 9, "dial: shutter index %d, iso index %d", sh_i, iso_i);
    EXPECT(screen_says("1/60"), "bar did not follow Canon's shutter within 150 ms\n%s", ui_text_drawn);
    tap(K_WL, 0); tap(K_WL, 0); run_for(150);
    EXPECT(screen_says("1/40"), "bar did not follow the dial back\n%s", ui_text_drawn);
    finish();
}

static void s_qbar_locked(void)
{
    auto_expo = 1;
    boot(); open_bar();
    EXPECT(screen_says("Auto") && screen_says("by Canon"), "auto exposure not shown:\n%s", ui_text_drawn);
    bar_select(QB_SHUTTER);
    tap(K_UP, 30); run_for(200);
    EXPECT(lens_changes == 0 && screen_says("Movie exposure to Manual"), "UP on automatic shutter: no explanation\n%s", ui_text_drawn);
    bar_select(QB_ISO);
    tap(K_UP, 30); run_for(200);
    EXPECT(screen_says("ISO is automatic"), "UP on automatic ISO: no explanation\n%s", ui_text_drawn);
    tap(K_INFO, 0); run_for(400);
    auto_expo = 0; lens_sync();
    cam.lens.lens_exists = 0; lens_sync();  /* a manual (adapted) lens */
    open_bar();
    bar_select(QB_APERTURE);
    tap(K_UP, 30); run_for(200);
    EXPECT(screen_says("Manual lens"), "UP on a manual lens: no explanation\n%s", ui_text_drawn);
    finish();
}

static void s_qbar_limits(void)
{
    boot(); open_bar();
    bar_select(QB_APERTURE);
    for (int i = 0; i < 12; i++) { tap(K_DOWN, 20); run_for(60); }
    EXPECT(ap_i == lens_min_i, "aperture went past the lens (index %d)", ap_i);
    EXPECT(screen_says("That is the limit"), "no message at the lens limit\n%s", ui_text_drawn);
    finish();
}

static void s_qbar_to_panel(void)
{
    boot(); open_bar();
    bar_select(QB_PROFILE);
    tap(K_DOWN, 40); run_for(800);
    EXPECT(overlay_active && ov_mode == OV_PANEL && panel.state == PS_OPEN, "DOWN on PROFILE did not open the list (mode %d)", ov_mode);
    tap(K_INFO, 0); run_for(800);
    EXPECT(overlay_active && ov_mode == OV_BAR && qbar.state == QB_OPEN, "closing the list did not go back to the bar (mode %d)", ov_mode);
    tap(K_UP, 40); run_for(800);
    EXPECT(ov_mode == OV_PANEL, "UP on PROFILE did not open the list");
    int idx = find_profile(MOOD_1X1);
    select_profile(idx);
    tap(K_SET, 40); run_for(5000);         /* apply, "Ready", the list closes itself ... */
    EXPECT(cfg_active == idx, "apply from the list failed (active %d)", cfg_active);
    EXPECT(overlay_active && ov_mode == OV_BAR, "after applying, not back on the bar (overlay %d mode %d)", overlay_active, ov_mode);
    EXPECT(screen_says(profiles[idx].name), "bar does not show the new profile\n%s", ui_text_drawn);
    tap(K_INFO, 0); run_for(500);
    EXPECT(!overlay_active, "INFO on the bar after the list did not close");
    finish();
}

static void s_qbar_touch(void)
{
    boot(); open_bar();
    touch_at(qb_tile_x[QB_WB] + 30, 450);      /* WB tile */
    EXPECT(qbar.sel == QB_WB, "tap on WB did not select it (sel %d)", qbar.sel);
    touch_at(qb_tile_x[QB_PROFILE] + 60, 440); /* PROFILE: select ... */
    EXPECT(qbar.sel == QB_PROFILE && ov_mode == OV_BAR, "tap on PROFILE: sel %d mode %d", qbar.sel, ov_mode);
    touch_at(qb_tile_x[QB_PROFILE] + 60, 440); /* ... and a second tap opens the list */
    run_for(600);
    EXPECT(ov_mode == OV_PANEL, "second tap on PROFILE did not open the list");
    tap(K_INFO, 0); run_for(800);
    /* a position that cannot be right, or a tap above the bar: nothing happens */
    int s0 = qbar.sel;
    touch_raw = 0xdeadbeef; gui_event(K_TOUCH); run_for(40); gui_event(K_UNTOUCH); run_for(100);
    touch_at(300, 100);
    EXPECT(qbar.sel == s0 && ov_mode == OV_BAR && overlay_active, "implausible / off-bar tap changed something");
    EXPECT(peeks_bad_addr == 0, "read RAM somewhere other than Canon's touch position");
    run_for(300); tap(K_INFO, 0); run_for(3500);
    EXPECT(strstr(logfile, "touch 0x70") && strstr(logfile, "(used)"), "touch reads not in the log:\n%s", logfile);
    finish();
}

static void s_qbar_rec(void)
{
    boot();
    select_profile(0);                      /* (panel closed: no-op) */
    open_panel(); select_profile(find_profile(MOOD_1X1)); tap(K_SET, 40); run_for(4000);
    open_bar();
    tap(K_REC, 0); run_for(300);
    EXPECT(raw_recordings == 1 && !overlay_active, "REC with the bar open: %d RAW, overlay %d", raw_recordings, overlay_active);
    cam.recording = 0; run_for(3500);
    finish();
}

static void s_qbar_external(void)
{
    boot(); open_bar();
    iso_i = 14; cam.lens.raw_iso = 72 + iso_i * 3;     /* Canon changed it (auto ISO, a remote ...) */
    run_for(400);
    EXPECT(screen_says("2500"), "bar did not follow a value Canon changed\n%s", ui_text_drawn);
    finish();
}

static void s_qbar_themes(void)
{
    boot();
    for (int t = 0; t < THEME_COUNT; t++)
    {
        cfg_theme = t;
        open_bar();
        ui_text_overflows = 0;
        qbar.full_redraw = 1; run_for(60);
        EXPECT(ui_text_overflows == 0, "theme %s: bar text does not fit: %s", moth_themes[t].name, ui_text_overflow_last);
        tap(K_INFO, 0); run_for(400);
    }
    finish();
}

static void s_themes_all(void)
{
    boot(); open_panel();
    int t0 = cfg_theme, s0 = panel.sel;
    tap(K_LEFT, 40); tap(K_RIGHT, 40); run_for(300);
    EXPECT(cfg_theme == t0 && panel.sel == s0 && panel_open_now(), "LEFT/RIGHT changed something (theme %d, sel %d)", cfg_theme, panel.sel);
    close_panel();
    /* the theme is set in Movie > Mothlight > Theme; every theme opens and draws */
    for (int t = 0; t < THEME_COUNT; t++)
    {
        cfg_theme = t;
        open_panel();
        EXPECT(screen_says(moth_themes[t].name), "theme %s not shown after choosing it in the menu", moth_themes[t].name);
        close_panel();
    }
    finish();
}

static void s_key_mash(void)
{
    boot(); open_panel();
    static const int keys[] = { K_UP, K_DOWN, K_LEFT, K_RIGHT, K_WL, K_WR, K_TOUCH, K_UNHANDLED, K_OLC };
    srand(7);
    for (int i = 0; i < 600; i++) { tap(keys[rand() % COUNT(keys)], rand() % 30); run_for(rand() % 25); }
    run_for(600);
    EXPECT(panel_open_now(), "600 random navigation keys closed the panel");
    EXPECT(panel.sel >= 0 && panel.sel < profile_count, "selection out of range: %d", panel.sel);
    finish();
}

static void s_keys_during_animations(void)
{
    boot();
    tap(K_INFO, 0); run_for(40);            /* iris still opening */
    tap(K_DOWN, 30); tap(K_DOWN, 30);
    run_for(800);
    EXPECT(panel.state == PS_OPEN, "keys during the open animation broke the panel");
    tap(K_INFO, 0); run_for(20);            /* closing ... */
    tap(K_SET, 20);                         /* ... and SET while it closes: must not apply */
    run_for(1500);
    EXPECT(!overlay_active, "panel did not close");
    EXPECT(menu_val("Movie", "Crop mood") == 0, "a SET pressed while closing applied a profile");
    finish();
}

static void s_rapid_toggle(void)
{
    boot();
    for (int i = 0; i < 40; i++) { tap(K_INFO, 0); run_for(15 + (i % 7) * 20); }
    run_for(1500);
    if (overlay_active) close_panel();
    run_for(500);
    EXPECT(!overlay_active, "rapid INFO toggling left the panel open");
    EXPECT(cam.front_enabled, "front buffer left disabled");
    finish();
}

static void s_rec_while_open(void)
{
    boot(); open_panel();
    tap(K_REC, 0); run_for(400);
    EXPECT(cam.recording, "REC pressed with the panel open did not start recording");
    EXPECT(!overlay_active, "panel still up while recording");
    cam.recording = 0;
    finish();
}

static void s_play_while_open(void)
{
    boot(); open_panel();
    tap(K_PLAY, 0); run_for(400);
    EXPECT(cam.playback, "PLAY with the panel open did not open playback");
    EXPECT(!overlay_active, "panel still up in playback");
    cam.playback = 0; cam.lv_on = 1;
    finish();
}

static void s_menu_while_open(void)
{
    boot(); open_panel();
    tap(K_TOUCH2, 0);                       /* two-finger tap: ML's menu */
    run_for(100);
    EXPECT(!overlay_active, "panel still drawing over ML's menu 100 ms after it opened");
    EXPECT(cam.menu_shown, "ML's menu did not open");
    cam.menu_shown = 0;
    finish();
}

static void s_mode_switch_while_open(void)
{
    boot(); open_panel();
    cam.movie = 0;                          /* mode switch to photo; LiveView stays */
    run_for(300);
    EXPECT(!overlay_active, "panel stayed open after switching to photo mode");
    cam.movie = 1;
    finish();
}

static void s_display_off_while_open(void)
{
    boot(); open_panel();
    cam.display_on = 0;
    run_for(300);
    EXPECT(!overlay_active, "panel stayed open with the display off");
    cam.display_on = 1;
    finish();
}

static void s_recording_starts_elsewhere(void)
{
    boot(); open_panel();
    cam.recording = 1; cam.raw = 1;         /* e.g. a remote, or ML's own trigger */
    run_for(200);
    EXPECT(!overlay_active, "panel up while recording");
    run_for(500);
    cam.recording = 0;
    run_for(1500);
    finish();
}

static int find_profile(int mood)
{
    for (int i = 0; i < profile_count; i++) if (profiles[i].mood == mood) return i;
    return -1;
}

static void select_profile(int idx)
{
    for (int i = 0; i < 40 && panel.sel != idx && panel_open_now(); i++)
    {
        tap(panel.sel < idx ? K_DOWN : K_UP, 30);
        run_for(60);
    }
    EXPECT(panel.sel == idx, "could not select profile %d (sel %d, panel %s)", idx, panel.sel, panel_open_now() ? "open" : "closed");
    run_for(300);
}

static void s_apply_with_lv_refresh(void)
{
    boot(); open_panel();
    int idx = find_profile(MOOD_1X1);
    select_profile(idx);
    tap(K_SET, 40);
    run_for(2000);                          /* LiveView restarted meanwhile */
    EXPECT(overlay_active, "the LiveView refresh during apply closed the panel");
    EXPECT(cfg_active == idx, "active profile %d, expected %d", cfg_active, idx);
    EXPECT(menu_val("Movie", "Crop mood") == profiles[idx].mood, "Crop mood not set");
    EXPECT(menu_val("Movie", "RAW video") == 1, "RAW video not enabled");
    EXPECT(cam.dispsize == 5, "x5 zoom not set");
    EXPECT(strstr(panel.toast, "Ready") != 0, "toast after apply: \"%s\"", panel.toast);
    run_for(4000);
    EXPECT(!overlay_active, "panel did not close itself after a successful apply");

    /* apply again, then keep browsing: a key cancels the auto-close */
    open_panel();
    select_profile(idx);
    tap(K_SET, 40);
    run_for(1200);
    tap(K_DOWN, 30);
    run_for(4000);
    EXPECT(overlay_active && panel.state == PS_OPEN, "a key after the result did not keep the panel open");
    EXPECT(panel.sel == idx + 1 || idx == profile_count - 1, "the key that dismissed the result was lost");
    finish();
}

static void s_apply_off(void)
{
    boot(); open_panel();
    int idx = find_profile(MOOD_OFF);
    if (idx < 0) { finish(); return; }
    select_profile(idx);
    tap(K_SET, 40);
    run_for(2500);
    EXPECT(menu_val("Movie", "RAW video") == 0 && menu_val("Movie", "Crop mood") == 0, "H.264 profile left RAW on");
    EXPECT(strstr(panel.toast, "H.264") != 0, "toast: \"%s\"", panel.toast);
    finish();
}

static void s_apply_modules_missing(void)
{
    boot();
    set_exists("Movie", "Crop mood", 0);    /* crop_rec not loaded */
    open_panel();
    select_profile(find_profile(MOOD_1X1));
    tap(K_SET, 40);
    run_for(1500);
    EXPECT(strstr(panel.toast, "crop_rec") != 0, "toast without crop_rec: \"%s\"", panel.toast);
    EXPECT(menu_val("Movie", "RAW video") == 0, "RAW video turned on although the apply failed (half-applied)");
    EXPECT(cfg_active == -1, "failed apply changed the active profile");
    set_exists("Movie", "Crop mood", 1);
    set_exists("Movie", "RAW video", 0);    /* mlv_lite not loaded */
    select_profile(find_profile(MOOD_1X1));
    tap(K_SET, 40);
    run_for(1500);
    EXPECT(strstr(panel.toast, "mlv_lite") != 0, "toast without mlv_lite: \"%s\"", panel.toast);
    finish();
}

static void s_stalled_task_watchdog(void)
{
    boot(); open_panel();
    select_profile(find_profile(MOOD_1X1));
    ui_stall_once = 3500;                   /* the panel's task hangs */
    run_for(2600);
    int before = canon_got[K_HALF];
    tap(K_HALF, 0);
    EXPECT(canon_got[K_HALF] == before + 1, "buttons still held by a stalled panel after 2.6 s");
    stall_zoom_ms = 0;
    run_for(3000);
    EXPECT(!overlay_active || panel_open_now(), "inconsistent state after the stall");
    if (overlay_active) close_panel();
    run_for(500);
    EXPECT(cam.front_enabled, "front buffer disabled after the watchdog");
    EXPECT(menu_val("Overlay", "Global Draw") == 1, "Global Draw stayed OFF after the watchdog");
    open_panel();
    finish();
}

static void s_malloc_failure(void)
{
    boot();
    fail_big_malloc = 1;
    tap(K_INFO, 0); run_for(500);
    EXPECT(!overlay_active, "panel opened without its buffer");
    EXPECT(cam.front_enabled, "front buffer disabled after a failed open");
    open_panel();                           /* works again once memory is there */
    finish();
}

static void s_stale_keys_on_menu_open(void)
{
    boot(); open_panel();
    tap(K_INFO, 0);                         /* close ... */
    tap(K_SET, 10);                         /* ... with keys arriving during the close */
    tap(K_DOWN, 10);
    run_for(1200);
    EXPECT(!overlay_active, "did not close");
    /* now open from the ML menu (does not go through the INFO path) */
    cam.menu_shown = 1;
    menu_child("Profiles")->select(0, 1);
    run_for(1200);
    EXPECT(panel_open_now(), "Mothlight > Profiles did not open the panel");
    EXPECT(menu_val("Movie", "Crop mood") == 0, "stale SET from the last session applied a profile");
    finish();
}

static void s_info_in_photo_mode(void)
{
    boot();
    cam.movie = 0;
    int before = canon_got[K_INFO];
    tap(K_INFO, 0); run_for(400);
    EXPECT(!overlay_active, "panel opened in photo mode");
    EXPECT(canon_got[K_INFO] == before + 1, "INFO in photo mode did not reach Canon");
    EXPECT(notify_count == 1 && strstr(notify_last, "movie mode"), "no hint in photo mode: \"%s\"", notify_last);
    tap(K_INFO, 0); run_for(300);
    EXPECT(notify_count == 1, "the photo-mode hint repeats on every INFO");
    EXPECT(canon_got[K_INFO] == before + 2, "second INFO in photo mode did not reach Canon");
    finish();
}

static void s_housekeeping_events_pass(void)
{
    boot(); open_panel();
    int d = canon_got[K_DOOR], o = canon_got[K_OLC];
    gui_event(K_DOOR); gui_event(K_OLC);
    run_for(100);
    EXPECT(canon_got[K_DOOR] == d + 1, "card door event swallowed while the panel is open");
    EXPECT(canon_got[K_OLC] == o + 1, "Canon info-changed event swallowed while the panel is open");
    finish();
}

static void s_halfshutter(void)
{
    boot(); open_panel();
    int h = canon_got[K_HALF];
    tap(K_HALF, 0); run_for(500);
    EXPECT(canon_got[K_HALF] == h + 1, "half-shutter did not reach Canon (no focus)");
    EXPECT(!overlay_active, "half-shutter did not close the panel");
    finish();
}

static void s_canon_periodic_redraw(void)
{
    boot();
    canon_periodic_ms = 700;                /* Canon refreshes its screen by itself */
    open_panel();
    run_for(3000);                          /* post-frame check fails if not repaired */
    canon_periodic_ms = 0;
    finish();
}

static void s_profiles_variants(void)
{
    static char many[4096];
    char * p = many;
    for (int i = 0; i < 20; i++)
        p += sprintf(p, "Profile number %d with a very long name indeed | 1:1 | 3K | 2.39 | 23.976 | 10\n", i);
    const char * cases[] = {
        "",
        "# only a comment\n",
        "garbage line\n\x01\x02\xff\n| | | |\n",
        "Solo | 1x3 | highest | 2.39 | 23.976 | 12\n",
        many,
        "No newline at end | 3x3 | 1080p | 16:9 | 25 | 14",
        "A | 1:1 | 2.8K | 2.35 | 30 | 11\r\nB | off | | | | \r\n",
    };
    for (int c = 0; c < COUNT(cases); c++)
    {
        pid_t pid = fork();
        if (pid == 0)
        {
            profiles_txt = cases[c];
            boot();
            EXPECT(profile_count > 0 && profile_count <= MAX_PROFILES, "case %d: %d profiles", c, profile_count);
            open_panel();
            int s0 = panel.sel;
            for (int i = 0; i < profile_count + 3; i++) { tap(K_DOWN, 20); run_for(60); }
            EXPECT(panel.sel == (s0 + profile_count + 3) % profile_count, "case %d: DOWN past the end should wrap: sel %d of %d", c, panel.sel, profile_count);
            for (int i = 0; i < profile_count + 3; i++) { tap(K_UP, 20); run_for(60); }
            EXPECT(panel.sel == s0, "case %d: UP past the top should wrap back: sel %d", c, panel.sel);
            run_for(400);
            finish();
            _exit(failures ? 1 : 0);
        }
        int st; waitpid(pid, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st)) { failures++; fprintf(stderr, "  FAIL [%s] PROFILES.TXT case %d\n", scenario, c); }
    }
}

static void s_parser_fuzz(void)
{
    static struct rec_profile out[MAX_PROFILES];
    char buf[512];
    srand(11);
    const char * parts[] = { "|", " ", "\n", "1:1", "1x3", "3x3", "off", "3K", "2.5K", "highest", "1080p",
                             "2.39", "16:9", "23.976", "25", "10", "12", "14", "#", "x", "\r", "99999999999" };
    for (int i = 0; i < 20000; i++)
    {
        int n = 0, len = rand() % 60;
        for (int k = 0; k < len && n < 480; k++)
        {
            if (rand() % 5 == 0) buf[n++] = (char)(rand() % 255 + 1);
            else { const char * s = parts[rand() % COUNT(parts)]; int l = strlen(s); memcpy(buf + n, s, l); n += l; }
        }
        buf[n] = 0;
        int bad = 0;
        int c = profiles_parse(buf, out, MAX_PROFILES, &bad);
        EXPECT(c >= 0 && c <= MAX_PROFILES, "parser returned %d", c);
        for (int j = 0; j < c; j++)
        {
            char s[96];
            profile_summary(&out[j], s, sizeof(s));
            EXPECT(out[j].nominal.width > 0 || out[j].mood == MOOD_OFF, "profile with no frame size");
        }
    }
}

static void s_bad_config_at_boot(void)
{
    cfg_theme = 99; cfg_active = 40; cfg_card = 77; cfg_tally = 9; cfg_bpp10 = -5; cfg_focus = 7; cfg_key = -3; cfg_motion = 4;
    boot();
    open_panel();
    tap(K_DOWN, 30); run_for(300);
    EXPECT(panel_open_now(), "panel not usable after a corrupt config");
    struct menu_display_info info = { 0 };
    for (struct menu_entry * e = menu_root; e < menu_root + menu_n; e++)
    {
        if (e->update) e->update(e, &info);
        for (struct menu_entry * c = e->children; c && c->name; c++) if (c->update) c->update(c, &info);
    }
    lv_items[0].update(&lv_items[0], 1);
    EXPECT(cfg_theme < THEME_COUNT && cfg_card < 10 && cfg_tally <= 2 && cfg_focus <= 1 && cfg_key >= 0 && cfg_key <= 2 && cfg_motion <= 1,
           "corrupt settings not repaired at start");
    finish();
}

static void s_reduced_motion(void)
{
    cfg_motion = 0;
    boot();
    tap(K_INFO, 0); run_for(150);
    EXPECT(panel.state == PS_OPEN, "with animations off the panel is not open after 150 ms");
    tap(K_DOWN, 20); run_for(100);
    tap(K_INFO, 0); run_for(150);
    EXPECT(!overlay_active, "with animations off the panel is not closed after 150 ms");
    finish();
}

static void s_learning(void)
{
    boot();
    /* a 10-bit profile is live; record 4 s */
    int idx = -1;
    for (int i = 0; i < profile_count; i++) if (profiles[i].mood == MOOD_1X1 && profile_bits(&profiles[i]) == 10) { idx = i; break; }
    open_panel(); select_profile(idx); tap(K_SET, 30); run_for(5000);
    EXPECT(!overlay_active, "panel did not close itself after applying");
    run_for(2500);                          /* the task reads the live format every 2 s */
    cam.recording = 1; cam.raw = 1;
    run_for(4000);
    /* 4 s at ~5.7 bit/px */
    const struct rec_format * f = &profiles[idx].nominal;
    uint32_t kB = (uint32_t)((long long)f->width * f->height * f->fps_x1000 / 1000 * 4 * 570 / 800 / 1000);
    snprintf(dcim[0].name, 64, "M23-1200.MLV"); dcim[0].size = kB * 1000 / 2; dcim[0].ts = 100;
    snprintf(dcim[1].name, 64, "M23-1200.M00"); dcim[1].size = kB * 1000 / 2; dcim[1].ts = 100;
    dcim_n = 2;
    cam.recording = 0;
    run_for(1500);
    EXPECT(notify_count == 1 && strstr(notify_last, "Learned: 10-bit"), "after a clip: \"%s\" (%d notes)", notify_last, notify_count);
    EXPECT(cfg_learned & 1, "10-bit not marked as learned");
    /* a clip with no files on the card must not learn garbage */
    cam.recording = 1; run_for(1000); dcim_n = 0; cam.recording = 0; run_for(1500);
    EXPECT(notify_count == 1, "learned from a clip that is not on the card");
    finish();
}

static void s_soak(void)
{
    const char * seed_s = getenv("SEED");
    unsigned seed = seed_s ? (unsigned)atoi(seed_s) : 1;
    srand(seed);
    boot();
    static const int keys[] = { K_UP, K_DOWN, K_LEFT, K_RIGHT, K_WL, K_WR, K_SET, K_INFO, K_INFO, K_INFO,
                                K_HALF, K_TOUCH, K_UNHANDLED, K_OLC, K_DOOR, K_REC, K_MENU, K_PLAY,
                                K_SET, K_UP, K_UP, K_TOUCH2 };
    int opens = 0, focus_steps = 0;
    for (int step = 0; step < 4000; step++)
    {
        if (getenv("SOAKDBG")) fprintf(stderr, "step %d t %u ov %d mode %d fj %d zoom %d lv %d rec %d menu %d gs %d\n", step, now_ms, overlay_active, ov_mode, fj_state, cam.dispsize, cam.lv_on, cam.recording, cam.menu_shown, gui_state);
        int r = rand() % 100;
        if (r < 70) tap(keys[rand() % COUNT(keys)], rand() % 60);
        else if (r < 74) { cam.movie = !cam.movie; }
        else if (r < 77) { cam.menu_shown = 1; run_for(200 + rand() % 400); cam.menu_shown = 0; }
        else if (r < 80) { cam.display_on = 0; run_for(300); cam.display_on = 1; }
        else if (r < 83) lv_refresh();
        else if (r < 85) { cam.recording = 0; }
        else if (r < 87) { cam.playback = 0; cam.lv_on = 1; cam.canon_menu = 0; }
        else if (r < 88) { stall_zoom_ms = 2500; }
        else if (r < 89) { stall_zoom_ms = 0; }
        else if (r < 90) { ui_stall_once = 1000 + rand() % 2500; }
        else if (r < 91) { canon_periodic_ms = canon_periodic_ms ? 0 : 900; }
        else if (r < 93) { cfg_key = 1 + rand() % 2; }   /* INFO opens the bar or the profiles */
        else if (r < 94) { cam.battery = rand() % 3; }
        else if (r < 95) { touch_raw = rand() % 3 ? (uint32_t)((rand() % 480) << 16 | (rand() % 720)) : (uint32_t)rand();
                           gui_event(K_TOUCH); run_for(rand() % 80); gui_event(K_UNTOUCH); }
        run_for(rand() % 150);
        if (overlay_active) opens++;
        if (fj_state != FJ_IDLE) focus_steps++;
        /* keep the world able to show the panel most of the time */
        if (rand() % 10 == 0) { cam.movie = 1; cam.playback = 0; cam.lv_on = 1; cam.recording = 0; cam.menu_shown = 0; }
    }
    stall_zoom_ms = 0; canon_periodic_ms = 0; cfg_key = 2;
    cam.movie = 1; cam.playback = 0; cam.lv_on = 1; cam.recording = 0; cam.menu_shown = 0; cam.display_on = 1;
    run_for(7000);
    cam.recording = 0;                      /* a REC pressed during a focus job records once it is back */
    if (overlay_active) close_panel();
    run_for(1000);
    EXPECT(!overlay_active && cam.front_enabled == !crop_wants_hidden(), "soak: did not end closed and clean (overlay %d, front %d, zoom %d, focus job %d, mood %d, menu %d, rec %d, lv %d, play %d)",
           overlay_active, cam.front_enabled, cam.dispsize, fj_state, menu_val("Movie", "Crop mood"), cam.menu_shown, cam.recording, cam.lv_on, cam.playback);
    EXPECT(heap_live == 0, "soak: %ld bytes leaked", heap_live);
    EXPECT(menu_val("Overlay", "Global Draw") == global_draw_at_start, "soak: Global Draw left at %d (chosen %d)",
           menu_val("Overlay", "Global Draw"), global_draw_at_start);
    EXPECT(cam.dispsize != 10 || zoom_before == 0, "soak: left punched in at x10");
    EXPECT(opens + focus_steps > 100, "soak: panel barely open (%d steps) and no focus jobs (%d), test not meaningful", opens, focus_steps);
    printf("    soak seed %u: %d steps open, %d in focus jobs (%d frames on Settings, %d at x10), %d frames checked, %u s simulated\n",
           seed, opens, focus_steps, soak_settings_frames, soak_x10_frames, frames_checked, now_ms / 1000);
    open_panel();                           /* and it still works */
    finish();
}

static void check_text_fits(const char * what)
{
    run_for(1500);                          /* every animation done */
    panel.full_redraw = 1;
    ui_text_overflows = 0;
    run_for(60);
    EXPECT(ui_text_overflows == 0, "%s (theme %s, profile \"%s\"): text does not fit: %s", what,
           moth_themes[cfg_theme].name, profiles[panel.sel].name, ui_text_overflow_last);
}

static void s_text_fits(void)
{
    static const char * longnames =
        "Twenty three characters | 1:1 | 3K | 2.39 | 23.976 | 10\n"
        "WWWWWWWWWWWWWWWWWWWWWWW | 1x3 | highest | 2.39 | 23.976 | 12\n"
        "Slow 3x3 long name here | 3x3 | hfr | 16:9 | 25 | 14\n"
        "H264 with a long name x | off | | | |\n";
    for (int variant = 0; variant < 2; variant++)
    {
        pid_t pid = fork();
        if (pid == 0)
        {
            profiles_txt = variant ? longnames : 0;
            boot();
            open_panel();
            for (int t = 0; t < THEME_COUNT; t++)
            {
                for (int i = 0; i < profile_count; i++)
                {
                    check_text_fits("browsing");
                    tap(K_DOWN, 20);
                }
                close_panel(); cfg_theme = (t + 1) % THEME_COUNT; open_panel();
            }
            /* the applying line and the result with the longest name, as the active profile */
            select_profile(variant ? 1 : 0);
            tap(K_SET, 20); run_for(30);
            ui_text_overflows = 0; panel.full_redraw = 1; run_for(40);
            EXPECT(ui_text_overflows == 0, "applying line: %s", ui_text_overflow_last);
            run_for(1000);
            tap(K_DOWN, 20);
            check_text_fits("after applying (In use: ...)");
            finish();
            fflush(stdout);
            _exit(failures ? 1 : 0);
        }
        int st; waitpid(pid, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st)) { failures++; fprintf(stderr, "  FAIL [%s] variant %d\n", scenario, variant); }
    }
}


/* ---------------------------------------------------------------- settings page */

static struct menu_entry * menu_child(const char * name);
static void open_settings(void)
{
    cam.menu_shown = 1;
    struct menu_entry * e = menu_child("Settings");
    if (e) e->select(0, 1);
    run_for(1200);
    EXPECT(overlay_active && ov_mode == OV_SETTINGS, "Mothlight > Settings did not open the page (mode %d)", ov_mode);
}

static int sel_id(void) { return sets.sel >= 0 && sets.sel < sview.count ? sview.row[sets.sel].id : -1; }

static void settings_goto(int id)
{
    for (int i = 0; i < SET_MAX_ROWS + 2 && sel_id() != id; i++) { tap(K_DOWN, 30); run_for(60); }
    EXPECT(sel_id() == id, "could not reach settings item %d (on %d)", id, sel_id());
}

static void settings_set(int id) { settings_goto(id); tap(K_SET, 30); run_for(250); }

static struct menu_entry * menu_child(const char * name)
{
    for (int i = 0; i < menu_n; i++) if (!strcmp(menu_root[i].name, name)) return &menu_root[i];
    return 0;
}

static void s_settings_basic(void)
{
    boot(); open_settings();
    EXPECT(screen_says("Settings") && screen_says("Theme") && screen_says("LOOK"), "Settings page not drawn:\n%s", ui_text_drawn);
    EXPECT(gui_state != 0, "gui_state not held on the settings page");
    /* every row, both ways: nothing leaves the screen, every help line fits */
    ui_text_overflows = 0;
    int n = 0;
    for (int i = 0; i < SET_MAX_ROWS; i++) { tap(K_DOWN, 30); run_for(80); n++; }
    for (int i = 0; i < SET_MAX_ROWS; i++) { tap(K_UP, 30); run_for(80); }
    EXPECT(ui_text_overflows == 0, "%d strings overflowed on the settings page (last: %s)", ui_text_overflows, ui_text_overflow_last);
    EXPECT(!canon_got[K_DOWN] && !canon_got[K_UP] && !canon_got[K_QSET], "settings keys leaked to Canon");
    EXPECT(ml_redraws == 0, "ML's long-press redraw ran on the settings page (black flash)");
    /* theme: SET tries the next one right here */
    int t0 = cfg_theme;
    settings_set(SI_THEME);
    EXPECT(cfg_theme == (t0 + 1) % THEME_COUNT && sview.theme == theme(), "SET on Theme: %d -> %d", t0, cfg_theme);
    EXPECT(screen_says(theme_names[cfg_theme]), "new theme name not shown");
    EXPECT(sel_id() == SI_THEME, "selection moved after changing the theme");
    settings_set(SI_MOTION);
    EXPECT(cfg_motion == 0, "Animations not switched off");
    settings_set(SI_MOTION);
    /* INFO: closed (opened from ML's menu) */
    tap(K_INFO, 0); run_for(600);
    EXPECT(!overlay_active && gui_state == 0, "INFO did not close Settings");
    finish();
}

static void s_settings_ml_items(void)
{
    boot(); open_settings();
    find_menu("Debug", "SD Overclock")->value = 0;
    run_for(600);
    settings_set(SI_SDCLOCK);
    EXPECT(menu_val("Debug", "SD Overclock") == 1 && screen_says("160 MHz") && screen_says("Restart the camera"),
           "SD speed: %d\n%s", menu_val("Debug", "SD Overclock"), ui_text_drawn);
    for (int i = 0; i < 3; i++) { tap(K_SET, 30); run_for(150); }
    EXPECT(menu_val("Debug", "SD Overclock") == 0, "SD speed does not wrap around to Off");
    settings_set(SI_SOUND);
    EXPECT(menu_val("RAW video", "Sound recording") == 1, "Sound not switched on");
    settings_set(SI_SOUND);
    settings_set(SI_PREVIEW);
    EXPECT(menu_val("RAW video", "Preview") == 2 && screen_says("Exact framing"), "preview setting: %d", menu_val("RAW video", "Preview"));
    settings_set(SI_HACKS);
    EXPECT(menu_val("RAW video", "Small hacks") == 2, "Small hacks: %d", menu_val("RAW video", "Small hacks"));
    settings_set(SI_ZEBRAS);
    settings_set(SI_PEAK);
    settings_set(SI_FALSECOLOR);
    EXPECT(menu_val("Overlay", "Zebras") == 1 && menu_val("Overlay", "Focus Peak") == 1 && menu_val("Overlay", "False color") == 1,
           "aids not switched on");
    int card0 = cfg_card;
    settings_set(SI_CARD);
    EXPECT(cfg_card != card0, "card speed did not change");
    /* ML overlays: Global Draw stays off while open, the choice applies on closing */
    EXPECT(menu_val("Overlay", "Global Draw") == 0, "Global Draw on while Mothlight is open");
    settings_set(SI_OVERLAYS);
    EXPECT(screen_says("Off"), "ML overlays not shown as Off");
    EXPECT(menu_val("Overlay", "Global Draw") == 0, "Global Draw switched on while Mothlight is open");
    close_panel(); run_for(300);
    EXPECT(menu_val("Overlay", "Global Draw") == 0, "ML overlays Off was not kept after closing");
    find_menu("Overlay", "Global Draw")->value = 1;
    global_draw_at_start = 1;
    /* a module that is not loaded: shown, and explained */
    set_exists("RAW video", "Sound recording", 0);
    open_settings();
    EXPECT(screen_says("not loaded"), "missing mlv_snd not shown");
    settings_set(SI_SOUND);
    EXPECT(screen_says("Load mlv_snd"), "SET on a missing module: no explanation\n%s", ui_text_drawn);
    finish();
}

static void s_settings_from_ml_menu(void)
{
    boot();
    cam.menu_shown = 1;
    struct menu_entry * e = menu_child("Settings");
    EXPECT(e && e->select, "no Mothlight > Settings in ML's menu");
    if (e) e->select(0, 1);
    run_for(1200);
    EXPECT(overlay_active && ov_mode == OV_SETTINGS, "Mothlight > Settings did not open the page (mode %d)", ov_mode);
    tap(K_INFO, 0); run_for(600);
    EXPECT(!overlay_active, "INFO on Settings opened from the menu did not close it");
    /* REC and two-finger tap leave the page like they leave the bar */
    open_panel(); close_panel();
    open_settings();
    tap(K_TOUCH2, 0); run_for(300);
    EXPECT(!overlay_active && cam.menu_shown, "two-finger tap on Settings did not open ML's menu");
    cam.menu_shown = 0; run_for(200);
    open_settings();
    menu_set_value_from_script("Movie", "RAW video", 1);
    tap(K_REC, 0); run_for(300);
    EXPECT(!overlay_active && raw_recordings == 1, "REC on Settings: overlay %d, %d RAW", overlay_active, raw_recordings);
    cam.recording = 0; run_for(3500);
    finish();
}

static void s_settings_touch(void)
{
    boot(); open_settings();
    int y = 0, target = -1;
    /* the third item row on screen: tap selects, a second tap activates */
    for (int i = 0, items = 0; i < sview.count; i++)
        if (sview.row[i].kind == SR_ITEM && ++items == 3) { target = sview.row[i].id; break; }
    for (int k = 0; k < 400 && sel_id() != target; k += 4)
    {
        y = 60 + k;
        touch_at(300, y);
        if (sel_id() == target) break;
    }
    EXPECT(sel_id() == target, "tapping did not select item %d (on %d)", target, sel_id());
    int key0 = cfg_key;
    touch_at(300, y); run_for(200);
    EXPECT(target != SI_INFO || cfg_key != key0, "a second tap did not change the item");
    if (target == SI_INFO) { while (cfg_key != key0) { touch_at(300, y); run_for(150); } }
    finish();
}

static void s_warnings(void)
{
    auto_expo = 1;
    boot();
    open_panel(); select_profile(find_profile(MOOD_3X3)); tap(K_SET, 40); run_for(4000);
    if (overlay_active) close_panel();
    cam.battery = 0;
    find_menu("Debug", "SD Overclock")->value = 0;
    find_menu("RAW video", "Sound recording")->value = 1;
    open_bar(); close_panel();
    open_bar();                             /* second time: the worst warning in the strip */
    EXPECT(screen_says("Battery low"), "low battery not in the bar's strip:\n%s", ui_text_drawn);
    close_panel();
    open_settings();
    EXPECT(screen_says("Battery low") && screen_says("Movie exposure is Auto") && screen_says("SD Overclock is off"),
           "warnings missing on the settings page:\n%s", ui_text_drawn);
    int w0 = sview.warn_count;
    EXPECT(w0 >= 3, "only %d warnings", w0);
    /* fixing one from the page: the selection stays on its item although the rows move up */
    settings_goto(SI_SOUND);
    settings_set(SI_SOUND);
    run_for(700);
    EXPECT(sel_id() == SI_SOUND, "selection jumped after a warning went away (on %d)", sel_id());
    cam.battery = 2; run_for(700);
    EXPECT(sview.warn_count < w0 && sel_id() == SI_SOUND, "battery warning still shown, or selection moved (%d warnings, on %d)",
           sview.warn_count, sel_id());
    finish();
}

static void s_save_profile(void)
{
    boot();
    int n0 = profile_count;
    /* Crop Mood set by hand to something no profile has */
    find_menu("Movie", "Crop mood")->value = MOOD_1X1;
    find_menu("Crop mood", "Preset:")->value = 3;
    find_menu("Crop mood", "Aspect ratio:")->value = 1;
    find_menu("Crop mood", "Framerate:")->value = 0;
    find_menu("Crop mood", "Bit-depth")->value = 2;
    open_settings();
    settings_set(SI_SAVE);
    EXPECT(profiles_writes == 1, "PROFILES.TXT written %d times", profiles_writes);
    EXPECT(profile_count == n0 + 1, "profile count %d -> %d", n0, profile_count);
    EXPECT(cfg_active == profile_count - 1 && !strncmp(profiles[cfg_active].name, "My ", 3),
           "new profile not active (%d, %s)", cfg_active, cfg_active >= 0 ? profiles[cfg_active].name : "-");
    EXPECT(screen_says("Saved as"), "no confirmation\n%s", ui_text_drawn);
    EXPECT(strstr(profiles_written, "2.5K Cinema |") && strstr(profiles_written, profiles[cfg_active].name),
           "PROFILES.TXT lacks the built-ins or the new one:\n%s", profiles_written);
    /* the same again: not a duplicate */
    tap(K_SET, 30); run_for(250);
    EXPECT(profiles_writes == 1 && profile_count == n0 + 1 && screen_says("Already saved"), "saved a duplicate");
    /* it is a real profile: it shows in the list and applies */
    close_panel();
    cfg_key = 2; open_panel();
    EXPECT(screen_says(profiles[profile_count - 1].name), "saved profile not in the list");
    close_panel();
    /* crop_rec missing: nothing written, and said why */
    set_exists("Movie", "Crop mood", 0);
    open_settings(); settings_set(SI_SAVE);
    EXPECT(profiles_writes == 1 && screen_says("Load crop_rec"), "save without crop_rec: %d writes", profiles_writes);
    set_exists("Movie", "Crop mood", 1);
    /* the card refuses the file */
    find_menu("Crop mood", "Bit-depth")->value = 0;
    fail_profiles_write = 1;
    tap(K_SET, 30); run_for(250);
    EXPECT(profile_count == n0 + 1 && screen_says("Could not write"), "failed write not reported, or list changed");
    fail_profiles_write = 0;
    finish();
}

static void s_save_profile_full(void)
{
    static char many[4096];
    int n = 0;
    for (int i = 0; i < MAX_PROFILES; i++)
        n += snprintf(many + n, sizeof(many) - n, "P%d | 1:1 | 2.5K | 2.35 | 23.976 | %d\n", i, 10 + (i % 3));
    profiles_txt = many;
    boot();
    EXPECT(profile_count == MAX_PROFILES, "%d profiles loaded", profile_count);
    find_menu("Movie", "Crop mood")->value = MOOD_3X3;
    open_settings(); settings_set(SI_SAVE);
    EXPECT(profiles_writes == 0 && screen_says("profiles is the most"), "a 17th profile: %d writes\n%s", profiles_writes, ui_text_drawn);
    finish();
}

static void s_focus_punch_in(void)
{
    boot();
    open_panel(); select_profile(find_profile(MOOD_1X1)); tap(K_SET, 40); run_for(4000);
    if (overlay_active) close_panel();
    int z0 = cam.dispsize;
    open_bar();
    bar_select(QB_FOCUS);
    EXPECT(screen_says("x"), "FOCUS tile shows no zoom");
    tap(K_UP, 30); run_for(1500);
    EXPECT(cam.dispsize == 10 && overlay_active && ov_mode == OV_BAR, "UP on FOCUS: zoom %d, overlay %d", cam.dispsize, overlay_active);
    EXPECT(screen_says("x10") && screen_says("DOWN back"), "FOCUS tile not updated\n%s", ui_text_drawn);
    tap(K_UP, 30); run_for(300);
    EXPECT(screen_says("Already at x10"), "UP at x10: no message");
    tap(K_DOWN, 30); run_for(1800);
    EXPECT(cam.dispsize == z0, "DOWN on FOCUS: zoom %d (was %d)", cam.dispsize, z0);
    /* closing while punched in: back to where we were */
    tap(K_UP, 30); run_for(1500);
    tap(K_INFO, 0); run_for(2000);
    EXPECT(!overlay_active && cam.dispsize == z0, "closing at x10 left zoom %d", cam.dispsize);
    /* closing while the zoom is still being set */
    open_bar(); bar_select(QB_FOCUS);
    tap(K_UP, 30); run_for(150);
    tap(K_INFO, 0); run_for(3000);
    EXPECT(!overlay_active && cam.dispsize == z0, "closing during the zoom left zoom %d", cam.dispsize);
    /* REC while punched in: zoom back first, then record RAW */
    open_bar(); bar_select(QB_FOCUS);
    tap(K_UP, 30); run_for(1500);
    EXPECT(cam.dispsize == 10, "no x10");
    tap(K_REC, 0); run_for(3000);
    EXPECT(raw_recordings == 1 && rec_at_x10 == 0 && cam.dispsize == z0,
           "REC at x10: %d RAW, %d at x10, zoom %d", raw_recordings, rec_at_x10, cam.dispsize);
    cam.recording = 0; run_for(3500);
    /* zoomed by Canon (not by us): closing leaves it alone */
    cam.dispsize = 10;
    open_bar(); close_panel(); run_for(1500);
    EXPECT(cam.dispsize == 10, "closing changed a zoom Mothlight did not set");
    cam.dispsize = z0;
    finish();
}

static void s_welcome(void)
{
    want_welcome = 1;
    boot();
    run_for(4000);
    EXPECT(overlay_active && ov_mode == OV_SETTINGS && welcome_mode, "no welcome on the first start (overlay %d mode %d)", overlay_active, ov_mode);
    EXPECT(screen_says("Welcome") && screen_says("is installed") && screen_says("SETUP CHECK"),
           "welcome not drawn:\n%s", ui_text_drawn);
    EXPECT(sel_id() == SI_WELCOME_DONE, "welcome does not start on 'Start shooting'");
    settings_goto(SI_CHECK_RAW);
    EXPECT(screen_says("Ready"), "setup check not drawn:\n%s", ui_text_drawn);
    ui_text_overflows = 0;
    for (int i = 0; i < 12; i++) { tap(K_DOWN, 30); run_for(80); }
    EXPECT(ui_text_overflows == 0, "%d strings overflowed on the welcome page (last: %s)", ui_text_overflows, ui_text_overflow_last);
    settings_goto(SI_WELCOME_DONE);
    tap(K_SET, 30); run_for(600);
    EXPECT(!overlay_active && cfg_welcomed, "Start shooting did not close the welcome (overlay %d)", overlay_active);
    run_for(300);
    EXPECT(welcome_marker_on_card, "the welcome's marker file was not written (a battery pull would bring it back)");
    run_for(6000);
    EXPECT(!overlay_active, "the welcome came back");
    /* from Settings > About it shows again, and back returns to Settings */
    open_settings();
    settings_set(SI_WELCOME);
    EXPECT(welcome_mode && screen_says("HOW IT WORKS"), "Welcome and setup check did not show the page");
    tap(K_INFO, 0); run_for(400);
    EXPECT(overlay_active && ov_mode == OV_SETTINGS && !welcome_mode && sel_id() == SI_WELCOME, "back from the welcome: mode %d welcome %d on %d",
           ov_mode, welcome_mode, sel_id());
    finish();
}

static void s_recording_preview_defaults(void)
{
    boot();
    EXPECT(menu_val("RAW video", "Preview toggle") == 1, "(setup) toggle starts on");
    open_panel(); select_profile(find_profile(MOOD_1X1)); tap(K_SET, 40); run_for(4000);
    if (overlay_active) close_panel();
    EXPECT(menu_val("RAW video", "Preview toggle") == 0, "MLV Lite still swaps the preview on half-press");
    EXPECT(menu_val("RAW video", "Preview") == 2, "recording preview not set to exact framing (%d)", menu_val("RAW video", "Preview"));
    /* the user's own choice afterwards is kept on the next apply */
    open_settings(); settings_set(SI_PREVIEW); settings_set(SI_PREVIEW);
    int chosen = menu_val("RAW video", "Preview");
    close_panel();
    open_panel(); select_profile(find_profile(MOOD_3X3)); tap(K_SET, 40); run_for(4000);
    if (overlay_active) close_panel();
    EXPECT(menu_val("RAW video", "Preview") == chosen, "applying overrode the user's preview choice (%d -> %d)", chosen, menu_val("RAW video", "Preview"));
    /* RAW already on at start-up: fixed without applying */
    finish();
}

static void s_recording_preview_at_boot(void)
{
    find_menu("Movie", "RAW video")->value = 1;
    boot();
    run_for(4000);
    EXPECT(menu_val("RAW video", "Preview toggle") == 0 && menu_val("RAW video", "Preview") == 2, "start-up with RAW on: toggle %d preview %d",
           menu_val("RAW video", "Preview toggle"), menu_val("RAW video", "Preview"));
    finish();
}

static void s_welcome_after_battery_pull(void)
{
    /* seen before, but the camera was not switched off cleanly: ML never saved moth.welcomed */
    welcome_marker_on_card = 1;
    want_welcome = 1;
    boot();
    run_for(8000);
    EXPECT(!overlay_active && cfg_welcomed, "the welcome came back after a battery pull");
    finish();
}

static void s_welcome_waits(void)
{
    /* not over the ML menu, not in photo mode, not while recording: it waits for calm LiveView */
    want_welcome = 1;
    boot();
    cam.movie = 0; run_for(5000);
    EXPECT(!overlay_active, "welcome opened in photo mode");
    cam.movie = 1; cam.menu_shown = 1; run_for(5000);
    EXPECT(!overlay_active, "welcome opened over ML's menu");
    cam.menu_shown = 0; run_for(1000);
    EXPECT(!overlay_active, "welcome opened before LiveView calmed down");
    run_for(3000);
    EXPECT(overlay_active && welcome_mode, "welcome never opened");
    tap(K_REC, 0); run_for(300);         /* REC closes it like any page, and counts as seen */
    EXPECT(!overlay_active && cfg_welcomed, "REC did not close the welcome (or it was not marked seen)");
    cam.recording = 0; run_for(3500);
    finish();
}

/* ---------------------------------------------------------------- focus at x10 */

static void crop_preset_on(void)
{
    open_panel(); select_profile(find_profile(MOOD_1X1)); tap(K_SET, 40); run_for(4000);
    if (overlay_active) close_panel();
    run_for(300);
    EXPECT(cam.dispsize == 5 && menu_val("Movie", "Crop mood") > 0, "no Crop Mood x5 preset (zoom %d)", cam.dispsize);
}

static void s_focus_half_press(void)
{
    boot(); cfg_focus = FOCUS_X10; crop_preset_on();
    gui_event(K_HALF); run_for(1600);
    EXPECT(cam.dispsize == 10 && fj_state == FJ_HOLD, "half-press did not go to x10 (zoom %d, job %d)", cam.dispsize, fj_state);
    EXPECT(af_at_x10 == 1 && af_at_x5 == 0 && canon_af_in_x5 == 0, "AF ran %d at x10, %d at x5, Canon %d at x5", af_at_x10, af_at_x5, canon_af_in_x5);
    EXPECT(cam.front_enabled, "Canon's layer (its focus box) not shown at x10");
    EXPECT(fj_focused, "focus not confirmed");
    run_for(1500);
    EXPECT(cam.dispsize == 10, "x10 left while the half-press is still held");
    gui_event(K_HALF_UP); run_for(2500);
    EXPECT(cam.dispsize == 5 && fj_state == FJ_IDLE, "release did not go back to x5 (zoom %d, job %d)", cam.dispsize, fj_state);
    EXPECT(af_cx == AF_W / 2 && af_cy == AF_H / 2, "AF point not centred again (%d,%d)", af_cx, af_cy);
    EXPECT(!cam.front_enabled && canon_layer_flashes == 0, "Canon's layer after going back: on %d, flashes %d", cam.front_enabled, canon_layer_flashes);
    EXPECT(!sw1_on, "half-shutter switch left pressed");
    finish();
}

static void s_focus_tap(void)
{
    boot(); cfg_focus = FOCUS_X10; crop_preset_on();
    touch_raw = 0xffffffff;
    touch_at(600, 140);                     /* top-right of the picture */
    run_for(900);
    EXPECT(cam.dispsize == 10 && af_cx > AF_W / 2 + 300 && af_cy < AF_H / 2, "tap: zoom %d, AF point %d,%d", cam.dispsize, af_cx, af_cy);
    EXPECT(canon_moved_window == 0, "the tap reached Canon (it would move the recorded window)");
    run_for(3500);
    EXPECT(cam.dispsize == 5 && fj_state == FJ_IDLE && af_cx == AF_W / 2 && af_cy == AF_H / 2,
           "after the tap: zoom %d, job %d, AF %d,%d", cam.dispsize, fj_state, af_cx, af_cy);
    /* a second tap while looking: done at once */
    touch_at(100, 300); run_for(1200);
    EXPECT(fj_state == FJ_HOLD, "second focus job not holding (%d)", fj_state);
    touch_at(360, 240); run_for(2200);
    EXPECT(fj_state == FJ_IDLE && cam.dispsize == 5, "a tap while looking did not end it");
    /* the focus never confirms: still comes back */
    af_never_confirms = 1;
    touch_at(360, 240); run_for(6000);
    EXPECT(fj_state == FJ_IDLE && cam.dispsize == 5 && !sw1_on, "no-focus job stuck (job %d zoom %d sw1 %d)", fj_state, cam.dispsize, sw1_on);
    finish();
}

static void s_focus_leaves_alone(void)
{
    boot();
    /* not a Crop Mood preset: Canon's own tap AF and half-press */
    int t0 = canon_got[K_UNTOUCH], h0 = canon_got[K_HALF];
    touch_at(500, 200); tap(K_HALF, 200); run_for(300);
    EXPECT(canon_got[K_UNTOUCH] == t0 + 1 && canon_got[K_HALF] == h0 + 1 && fj_state == FJ_IDLE, "x1: tap / half-press not Canon's");
    crop_preset_on();
    /* manual focus: Crop Mood's own x10 key, not Mothlight */
    cam.manual_focus = 1;
    h0 = canon_got[K_HALF];
    tap(K_HALF, 200); run_for(300);
    EXPECT(canon_got[K_HALF] == h0 + 1 && fj_state == FJ_IDLE, "MF: half-press taken");
    cam.manual_focus = 0;
    /* switched off in Settings */
    cfg_focus = 0;
    h0 = canon_got[K_HALF];
    tap(K_HALF, 200); run_for(300);
    EXPECT(canon_got[K_HALF] == h0 + 1 && fj_state == FJ_IDLE, "focus off: half-press taken");
    cfg_focus = 1;
    /* while recording: never (Crop Mood blocks taps itself) */
    menu_set_value_from_script("Movie", "RAW video", 1);
    tap(K_REC, 0); run_for(200);
    EXPECT(cam.recording, "not recording");
    touch_at(500, 200); tap(K_HALF, 200); run_for(1500);
    EXPECT(fj_state == FJ_IDLE && af_moves_rec == 0 && cam.dispsize == 5, "focus job while recording");
    cam.recording = 0; run_for(3500);
    finish();
}

static void s_focus_keys_meanwhile(void)
{
    boot(); cfg_focus = FOCUS_X10; crop_preset_on();
    menu_set_value_from_script("Movie", "RAW video", 1);
    /* REC while looking at x10: back to x5 first, then RAW */
    touch_at(360, 240); run_for(1000);
    EXPECT(cam.dispsize == 10, "no x10");
    tap(K_REC, 0); run_for(3500);
    EXPECT(raw_recordings == 1 && rec_at_x10 == 0 && cam.dispsize == 5, "REC during focus: %d RAW, %d at x10, zoom %d",
           raw_recordings, rec_at_x10, cam.dispsize);
    cam.recording = 0; run_for(3500);
    /* INFO while looking: ends it; the bar does not pop up over x10 */
    cfg_key = 1;
    touch_at(360, 240); run_for(1000);
    tap(K_INFO, 0); run_for(2500);
    EXPECT(fj_state == FJ_IDLE && cam.dispsize == 5 && !overlay_active, "INFO during focus: job %d zoom %d overlay %d", fj_state, cam.dispsize, overlay_active);
    open_bar(); close_panel();
    /* two-finger tap (ML's menu) while holding a half-press, and the release is lost */
    gui_event(K_HALF); run_for(1200);
    tap(K_TOUCH2, 0); run_for(200);
    EXPECT(cam.menu_shown, "ML's menu did not open");
    run_for(2500);
    cam.menu_shown = 0; run_for(2000);
    EXPECT(fj_state == FJ_IDLE && cam.dispsize == 5, "lost release left x10 (job %d zoom %d)", fj_state, cam.dispsize);
    finish();
}

static int box_pixels(int cx, int cy, uint8_t color)
{
    /* how many pixels of a focus-box outline centred at (cx, cy) have this colour */
    int n = 0, x0 = cx - FB_W / 2, y0 = cy - FB_H / 2;
    for (int i = 0; i < FB_W; i++) { n += bmp[y0 * BMPPITCH + x0 + i] == color; n += bmp[(y0 + FB_H - 1) * BMPPITCH + x0 + i] == color; }
    for (int i = 0; i < FB_H; i++) { n += bmp[(y0 + i) * BMPPITCH + x0] == color; n += bmp[(y0 + i) * BMPPITCH + x0 + FB_W - 1] == color; }
    return n;
}

static void s_focus_box(void)
{
    boot(); crop_preset_on();
    EXPECT(cfg_focus == FOCUS_BOX, "focus box is not the default (%d)", cfg_focus);
    run_for(400);
    EXPECT(box_pixels(UI_W / 2, UI_H / 2, COLOR_WHITE) > 200, "no white focus box in the middle (%d px)", box_pixels(UI_W / 2, UI_H / 2, COLOR_WHITE));
    /* tap: the box moves there, focuses in place (no zoom), turns green */
    int zooms_before = cam.dispsize;
    touch_at(560, 150); run_for(700);
    int bx, by;
    EXPECT(afframe_to_screen(&bx, &by) && ABS(bx - 560) < 12 && ABS(by - 150) < 12, "AF point not where tapped (%d,%d)", bx, by);
    EXPECT(cam.dispsize == zooms_before && af_at_x10 == 0 && af_at_x5 == 1, "box mode zoomed (zoom %d, AF x10 %d x5 %d)", cam.dispsize, af_at_x10, af_at_x5);
    EXPECT(ABS(fb_drawn.x - 560) < 12 && ABS(fb_drawn.y - 150) < 12, "box drawn at %d,%d, tapped 560,150", fb_drawn.x, fb_drawn.y);
    EXPECT(box_pixels(fb_drawn.x, fb_drawn.y, COLOR_GREEN1) > 200, "box not green at the tapped spot after focusing");
    EXPECT(box_pixels(UI_W / 2, UI_H / 2, COLOR_WHITE) < 20, "old box left behind in the middle");
    EXPECT(canon_moved_window == 0 && !sw1_on, "tap reached Canon, or the half-shutter switch was left on");
    run_for(2000);
    EXPECT(box_pixels(fb_drawn.x, fb_drawn.y, COLOR_WHITE) > 200, "box did not go back to white");
    /* Crop Mood clears the screen: the box comes back */
    memset(bmp, 0, BMPPITCH * 480); run_for(250);
    EXPECT(box_pixels(fb_drawn.x, fb_drawn.y, COLOR_WHITE) > 200, "box not repaired after the screen was cleared");
    /* half-press: Canon focuses at the box; it goes green, no zoom */
    gui_event(K_HALF); run_for(700);
    EXPECT(box_pixels(fb_drawn.x, fb_drawn.y, COLOR_GREEN1) > 200 && cam.dispsize == 5, "half-press: box not green, or zoomed");
    gui_event(K_HALF_UP); run_for(300);
    EXPECT(canon_af_in_x5 >= 1, "half-press did not reach Canon in box mode");
    /* the quick bar hides it, closing brings it back */
    int keep_x = fb_drawn.x, keep_y = fb_drawn.y;
    cfg_key = 1; open_bar();
    EXPECT(box_pixels(keep_x, keep_y, COLOR_WHITE) < 20, "box floats over the open bar");
    close_panel(); run_for(400);
    EXPECT(box_pixels(fb_drawn.x, fb_drawn.y, COLOR_WHITE) > 200, "box not back after closing the bar");
    /* no focus: red, then white */
    af_never_confirms = 1;
    touch_at(200, 300); run_for(2400);
    EXPECT(box_pixels(fb_drawn.x, fb_drawn.y, COLOR_RED) > 200, "failed focus not shown in red");
    run_for(2000);
    EXPECT(box_pixels(fb_drawn.x, fb_drawn.y, COLOR_WHITE) > 200, "red box did not return to white");
    finish();
}

static void s_focus_box_not_shown(void)
{
    boot();
    run_for(500);
    EXPECT(box_pixels(UI_W / 2, UI_H / 2, COLOR_WHITE) < 20, "focus box drawn outside Crop Mood (Canon draws its own there)");
    crop_preset_on();
    cam.manual_focus = 1; run_for(400);
    EXPECT(box_pixels(UI_W / 2, UI_H / 2, COLOR_WHITE) < 20, "focus box with a manual-focus lens");
    cam.manual_focus = 0; cfg_focus = FOCUS_CANON; run_for(400);
    EXPECT(box_pixels(UI_W / 2, UI_H / 2, COLOR_WHITE) < 20, "focus box with the setting off");
    cfg_focus = FOCUS_BOX; run_for(400);
    EXPECT(box_pixels(UI_W / 2, UI_H / 2, COLOR_WHITE) > 200, "focus box not back");
    cam.menu_shown = 1; run_for(400);
    EXPECT(box_pixels(UI_W / 2, UI_H / 2, COLOR_WHITE) < 20, "focus box over ML's menu");
    cam.menu_shown = 0;
    /* while recording: stays (Canon shows its box too), taps go to Crop Mood / Canon */
    menu_set_value_from_script("Movie", "RAW video", 1);
    tap(K_REC, 0); run_for(400);
    int moves = af_moves;
    touch_at(500, 200); run_for(400);
    EXPECT(af_moves == moves && af_moves_rec == 0, "the box moved while recording");
    cam.recording = 0; run_for(3500);
    finish();
}

static void add_clip(const char * name, uint32_t MB, uint32_t ts, int frames, int w, int h, int audio)
{
    struct fake_file * f = &dcim[dcim_n++];
    memset(f, 0, sizeof(*f));
    snprintf(f->name, sizeof(f->name), "%s", name);
    f->size = MB * 1000000; f->ts = ts; f->frames = frames; f->w = w; f->h = h; f->audio = audio;
}

static void clips_on_card(void)
{
    dcim_n = 0;
    add_clip("M07-0624.MLV", 255, 100, 122, 2520, 1080, 1);
    add_clip("M07-0809.MLV", 764, 200, 380, 2520, 1080, 1);
    add_clip("M07-0809.M00", 300, 201, 0, 0, 0, 0);
    add_clip("M08-0001.MLV", 40, 300, 0, 2520, 1080, 0);       /* cut short: no frame count */
    add_clip("IMG_0042.JPG", 6, 250, 0, 0, 0, 0);
}

static void s_clips(void)
{
    clips_on_card();
    find_menu("Movie", "RAW video")->value = 1;
    boot();
    tap(K_PLAY, 0); run_for(800);
    EXPECT(overlay_active && ov_mode == OV_SETTINGS && clips_mode == 1, "PLAY did not open the clip list (overlay %d mode %d clips %d)",
           overlay_active, ov_mode, clips_mode);
    EXPECT(mlv_play_runs == 0 && canon_got[K_PLAY] == 0, "PLAY went to the player / Canon as well");
    EXPECT(clip_count == 3 && !strcmp(clips[0].name, "M08-0001.MLV"), "clips: %d, first %s", clip_count, clip_count ? clips[0].name : "-");
    EXPECT(screen_says("Clips") && screen_says("M08-0001") && screen_says("--:--") && screen_says("1.0 GB") && screen_says("0:15"),
           "clip list not drawn:\n%s", ui_text_drawn);
    EXPECT(!screen_says("IMG_0042"), "a photo in the clip list");
    ui_text_overflows = 0;
    for (int i = 0; i < 8; i++) { tap(K_DOWN, 30); run_for(80); }
    EXPECT(ui_text_overflows == 0, "clip list text overflowed (%s)", ui_text_overflow_last);
    /* a clip: details, play */
    settings_goto(SI_CLIP_BASE + 1);
    tap(K_SET, 30); run_for(300);
    EXPECT(clips_mode == 2 && clip_cur == 1 && screen_says("Length 0:15") && screen_says("2 files") && screen_says("sound yes"),
           "clip page wrong:\n%s", ui_text_drawn);
    EXPECT(sel_id() == SI_CLIP_PLAY, "clip page does not start on Play");
    tap(K_INFO, 0); run_for(300);
    EXPECT(overlay_active && clips_mode == 1, "INFO on a clip did not go back to the list");
    tap(K_SET, 30); run_for(300);
    tap(K_SET, 30); run_for(1200);
    EXPECT(!overlay_active && !strcmp(mlv_played, "B:/DCIM/100CANON/M07-0809.MLV") && play_req_len == 0,
           "Play: overlay %d, player got '%s'", overlay_active, mlv_played);
    EXPECT(gui_state == 0, "gui_state held after handing over to the player");
    /* delete: asks, Keep is first, Delete removes the clip and its chunks */
    tap(K_PLAY, 0); run_for(800);
    EXPECT(clips_mode == 1, "PLAY did not reopen the list");
    settings_goto(SI_CLIP_BASE + 1); tap(K_SET, 30); run_for(300);
    settings_goto(SI_CLIP_DELETE); tap(K_SET, 30); run_for(300);
    EXPECT(clips_mode == 3 && sel_id() == SI_DEL_KEEP && screen_says("deleted for good"), "delete question wrong (mode %d on %d)", clips_mode, sel_id());
    tap(K_SET, 30); run_for(300);
    EXPECT(clips_mode == 2 && removals == 0, "Keep it removed something / did not go back");
    settings_goto(SI_CLIP_DELETE); tap(K_SET, 30); run_for(300);
    settings_goto(SI_DEL_YES); tap(K_SET, 30); run_for(500);
    EXPECT(strstr(removed, "M07-0809.MLV") && strstr(removed, "M07-0809.M00") && clip_count == 2 && clips_mode == 1,
           "delete: removed '%s', %d clips, mode %d", removed, clip_count, clips_mode);
    EXPECT(screen_says("Deleted"), "no confirmation after deleting");
    EXPECT(!strstr(removed, "IMG_") && !strstr(removed, "M07-0624"), "deleted the wrong files: %s", removed);
    /* PLAY on the list just closes it (no reopening loop) */
    tap(K_PLAY, 0); run_for(1000);
    EXPECT(!overlay_active && mlv_play_runs == 1, "PLAY on the list: overlay %d", overlay_active);
    /* Canon's playback from the list */
    tap(K_PLAY, 0); run_for(800);
    int c0 = canon_got[K_PLAY];
    settings_goto(SI_CANON_PLAY); tap(K_SET, 30); run_for(1000);
    EXPECT(!overlay_active && canon_got[K_PLAY] == c0 + 1, "Canon playback: overlay %d, Canon PLAY %d->%d", overlay_active, c0, canon_got[K_PLAY]);
    cam.playback = 0; cam.lv_on = 1; run_for(300);
    finish();
}

static void s_clips_left_alone(void)
{
    clips_on_card();
    boot();
    /* RAW off: Canon's playback, as always */
    int c0 = canon_got[K_PLAY];
    tap(K_PLAY, 0); run_for(500);
    EXPECT(!overlay_active && canon_got[K_PLAY] == c0 + 1, "RAW off: PLAY not Canon's");
    cam.playback = 0; cam.lv_on = 1; run_for(300);
    /* switched off: straight into the player, as before */
    find_menu("Movie", "RAW video")->value = 1;
    cfg_clips = 0;
    tap(K_PLAY, 0); run_for(500);
    EXPECT(!overlay_active && mlv_play_runs == 1, "Clips on PLAY off: player not started");
    cfg_clips = 1;
    /* REC on the list: closes, records RAW */
    tap(K_PLAY, 0); run_for(800);
    tap(K_REC, 0); run_for(400);
    EXPECT(!overlay_active && raw_recordings == 1, "REC on the clip list: overlay %d, %d RAW", overlay_active, raw_recordings);
    cam.recording = 0; run_for(3500);
    /* an empty card */
    dcim_n = 0;
    tap(K_PLAY, 0); run_for(800);
    EXPECT(clips_mode == 1 && screen_says("No RAW clips"), "empty card: no explanation");
    finish();
}

/* ---------------------------------------------------------------- runner */

struct sc { const char * name; void (*fn)(void); };
static const struct sc all[] = {
    { "basic navigation (no flash, no leak)", s_basic_navigation },
    { "double INFO during start-up (no ghost reopen)", s_double_info },
    { "DOWN with the panel open never reaches ML's long-press (no flash)", s_down_eosm },
    { "apply without visible flash (helper task)", s_apply_no_flash },
    { "close while applying, then apply again", s_close_mid_apply },
    { "SD Overclock hint when the card is the limit", s_sd_clock_hint },
    { "event log on the card (never while recording)", s_event_log },
    { "ML's gui_state held while open: no long-press flash on DOWN/SET", s_gui_state },
    { "REC with the panel open starts RAW (MLV Lite), not H.264", s_rec_raw_while_open },
    { "two-finger tap with the panel open: close and open ML's menu", s_two_finger_while_open },
    { "quick bar: values, SET next, UP/DOWN change, LiveView left alone", s_qbar_basic },
    { "quick bar: the dial is Canon's shutter; the bar follows it", s_qbar_dial },
    { "quick bar: tap selects a tile, tap PROFILE twice opens the list", s_qbar_touch },
    { "quick bar: auto exposure, auto ISO, manual lens explained", s_qbar_locked },
    { "quick bar: lens limits", s_qbar_limits },
    { "quick bar: UP/DOWN on PROFILE opens the list; closing / applying returns to the bar", s_qbar_to_panel },
    { "quick bar: REC starts RAW", s_qbar_rec },
    { "quick bar: follows values Canon changes", s_qbar_external },
    { "quick bar: every theme, text fits", s_qbar_themes },
    { "all themes, both directions", s_themes_all },
    { "600 random keys incl. touch and unknown events", s_key_mash },
    { "keys during open/close animations", s_keys_during_animations },
    { "rapid INFO toggling", s_rapid_toggle },
    { "REC with the panel open", s_rec_while_open },
    { "PLAY with the panel open", s_play_while_open },
    { "ML menu (two-finger tap) with the panel open", s_menu_while_open },
    { "switch to photo mode with the panel open", s_mode_switch_while_open },
    { "display turns off with the panel open", s_display_off_while_open },
    { "recording started elsewhere", s_recording_starts_elsewhere },
    { "apply a RAW profile (LiveView refresh, x5 zoom)", s_apply_with_lv_refresh },
    { "apply the H.264 profile", s_apply_off },
    { "apply with crop_rec / mlv_lite not loaded", s_apply_modules_missing },
    { "task stalls 3.5 s: watchdog and recovery", s_stalled_task_watchdog },
    { "out of memory when opening", s_malloc_failure },
    { "stale keys, then open from the ML menu", s_stale_keys_on_menu_open },
    { "INFO in photo mode", s_info_in_photo_mode },
    { "card door / Canon housekeeping events pass", s_housekeeping_events_pass },
    { "half-shutter closes and focuses", s_halfshutter },
    { "Canon redraws on its own every 0.7 s", s_canon_periodic_redraw },
    { "PROFILES.TXT: empty, garbage, 20 long names, CRLF ...", s_profiles_variants },
    { "profile parser fuzz (20 000 inputs)", s_parser_fuzz },
    { "corrupt config values at boot", s_bad_config_at_boot },
    { "animations off", s_reduced_motion },
    { "learning compression from a clip", s_learning },
    { "every profile x every theme: all text fits (23-char names too)", s_text_fits },
    { "settings: open from the bar, every row, theme, back to the bar", s_settings_basic },
    { "settings: ML's SD clock, sound, hacks, aids, overlays; missing modules", s_settings_ml_items },
    { "settings: from ML's menu; REC and two-finger tap leave it", s_settings_from_ml_menu },
    { "settings: tap selects, second tap changes", s_settings_touch },
    { "smart warnings: bar strip, MENU tile, settings; selection stays put", s_warnings },
    { "save Crop Mood's settings as a profile (duplicates, no crop_rec, card refuses)", s_save_profile },
    { "save a profile when 16 exist", s_save_profile_full },
    { "focus punch-in: x10, back, close, close mid-zoom, REC at x10", s_focus_punch_in },
    { "welcome on the first start, and again from Settings", s_welcome },
    { "welcome waits for calm movie LiveView", s_welcome_waits },
    { "welcome stays seen after a battery pull (marker file)", s_welcome_after_battery_pull },
    { "recording preview: exact framing, no half-press swap; user's choice kept", s_recording_preview_defaults },
    { "recording preview fixed at start-up when RAW is already on", s_recording_preview_at_boot },
    { "focus box: tap moves it and focuses in place (no zoom), green / red, repaired, hidden by the bar", s_focus_box },
    { "focus box: not outside Crop Mood, MF, off, ML menu; stays put while recording", s_focus_box_not_shown },
    { "focus (Precise): half-press focuses at x10, release goes back to x5", s_focus_half_press },
    { "focus (Precise): a tap focuses there at x10; second tap; no confirmation", s_focus_tap },
    { "focus: x1, manual focus, switched off, recording are left to Canon / Crop Mood", s_focus_leaves_alone },
    { "focus (Precise): REC, INFO, ML's menu during a focus job", s_focus_keys_meanwhile },
    { "clips: PLAY lists them, details, play the chosen one, delete with a question, Canon playback", s_clips },
    { "clips: RAW off, switched off, REC on the list, empty card", s_clips_left_alone },
    { "soak: 4000 random steps of keys and camera events", s_soak },
};

extern void camsim_load_fonts(const char * dir);

int main(int argc, char ** argv)
{
    const char * only = getenv("ONLY");
    int jobs = getenv("JOBS") ? atoi(getenv("JOBS")) : 2;
    if (jobs < 1) jobs = 1;
    int n = COUNT(all), failed = 0, ran = 0;
    pid_t pid[COUNT(all)] = { 0 };
    int status[COUNT(all)];
    char logs[COUNT(all)][64];
    int next = 0, running = 0, done_upto = 0;
    while (done_upto < n)
    {
        while (running < jobs && next < n)
        {
            int i = next++;
            if (only && !strstr(all[i].name, only)) { pid[i] = -1; continue; }
            snprintf(logs[i], sizeof(logs[i]), "/tmp/camsim.%d.%d.log", (int)getpid(), i);
            fflush(stdout);
            pid[i] = fork();
            if (pid[i] == 0)
            {
                FILE * f = freopen(logs[i], "w", stdout); (void)f;
                dup2(fileno(stdout), 2);
                if (argc > 1) camsim_load_fonts(argv[1]);
                if (argc > 2) dump_dir = argv[2];
                scenario = all[i].name;
                all[i].fn();
                fflush(stdout);
                _exit(failures ? 1 : 0);
            }
            running++;
        }
        int st; pid_t p = wait(&st);
        if (p > 0)
        {
            for (int i = 0; i < n; i++) if (pid[i] == p) { status[i] = st; pid[i] = -2; }
            running--;
        }
        /* print finished scenarios in order */
        while (done_upto < n && (pid[done_upto] == -1 || pid[done_upto] == -2))
        {
            int i = done_upto++;
            if (pid[i] == -1) continue;
            FILE * f = fopen(logs[i], "r");
            char line[512];
            while (f && fgets(line, sizeof(line), f))
                if (!strstr(line, "    #") && !strstr(line, "not understood")) fputs(line, stdout);
            if (f) fclose(f);
            unlink(logs[i]);
            int ok = WIFEXITED(status[i]) && WEXITSTATUS(status[i]) == 0;
            printf("%s  %s%s\n", ok ? "pass" : "FAIL", all[i].name, WIFSIGNALED(status[i]) ? "  (crashed)" : "");
            failed += !ok; ran++;
        }
    }
    printf("\n%d of %d scenarios passed\n", ran - failed, ran);
    return failed != 0;
}
