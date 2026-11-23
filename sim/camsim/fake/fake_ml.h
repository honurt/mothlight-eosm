/* A fake Magic Lantern + Canon layer, just big enough to compile and run the real
 * modules/moth_ui/moth_ui.c on a PC. See camsim.c for the world model. */
#ifndef FAKE_ML_H
#define FAKE_ML_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdarg.h>
#include "module_keys.h"

struct cam_state
{
    int lv_on, dispsize, recording, raw, movie, menu_shown, display_on, front_enabled;
    int playback, canon_menu;
    struct { int focal_len; } lens;
};
extern struct cam_state cam;

#define lv            (cam.lv_on)
#define lv_dispsize   (cam.dispsize)
#define RECORDING     (cam.recording)
#define RECORDING_RAW (cam.recording && cam.raw)
#define lens_info     (cam.lens)

#define COUNT(x)   ((int)(sizeof(x)/sizeof((x)[0])))
#define ABS(x)     ((x) < 0 ? -(x) : (x))
#define streq(a,b) (strcmp((a),(b)) == 0)
#define BMPPITCH   960
#define COLOR_EMPTY    0x00
#define COLOR_RED      0x08
#define COLOR_DARK_RED 0x09
#define COLOR_GREEN1   0x06
#define COLOR_YELLOW   0x0F

int  is_movie_mode(void);
int  gui_menu_shown(void);
int  display_is_on(void);
void set_lv_zoom(int zoom);
int  fps_get_current_x1000(void);

struct menu_display_info { char value[64]; char help[160]; char warning[96]; };
int  menu_get_value_from_script(const char * menu, const char * entry);
int  menu_set_value_from_script(const char * menu, const char * entry, int value);
const char * menu_get_str_value_from_script(const char * menu, const char * entry, struct menu_display_info * info);

uint8_t * bmp_vram_real(void);
void canon_gui_disable_front_buffer(void);
void canon_gui_enable_front_buffer(int);
void msleep(int ms);
void clrscr(void);
uint32_t get_ms_clock(void);
void redraw(void);
void bmp_fill(uint8_t col, int x, int y, int w, int h);

void * cam_malloc(size_t n);
void   cam_free(void * p);
#define malloc cam_malloc
#define free   cam_free

const char * get_dcim_dir(void);
struct fio_file { int mode; uint32_t timestamp; char name[64]; };
struct fio_dirent { int i; };
#define ATTR_DIRECTORY 0x10
#define IS_ERROR(p) (!(p))
struct fio_dirent * FIO_FindFirstEx(const char * dir, struct fio_file * f);
int  FIO_FindNextEx(struct fio_dirent * d, struct fio_file * f);
void FIO_FindClose(struct fio_dirent * d);
int  FIO_GetFileSize(const char * path, uint32_t * size);
void NotifyBox(int ms, const char * fmt, ...);

struct card_info { int dummy; };
struct card_info * get_shooting_card(void);
int  get_free_space_32k(struct card_info * c);

const char * get_config_dir(void);
void * read_entire_file(const char * path, int * size);
void fio_free(void * p);
int  is_camera(const char * model, const char * fw);

struct font { const void * bitmap; };
extern struct font font_dynamic[];
void task_create(const char * name, int prio, int stack, void * fn, void * arg);
#define CBR_RET_ERROR 1

struct event { int type; int param; void * obj; int arg; };
#define FAKE_BTN 0x7E57
#define IS_FAKE(event) ((event)->arg == FAKE_BTN)

FILE * FIO_CreateFile(const char * name);
int  FIO_WriteFile(FILE * f, const void * p, size_t n);
void FIO_CloseFile(FILE * f);

/* module glue: expose entry points to the harness */
#define MODULE_INFO_START()
#define MODULE_INIT(f)   unsigned int cam_mod_init(void) { return f(); }
#define MODULE_DEINIT(f) unsigned int cam_mod_deinit(void) { return f(); }
#define MODULE_INFO_END()
#define MODULE_CBRS_START()
#define MODULE_CBR(t, f, x) unsigned int cam_##t(unsigned int a) { return f(a); }
#define MODULE_CBRS_END()
#define MODULE_CONFIGS_START()
#define MODULE_CONFIG(v)
#define MODULE_CONFIGS_END()
#define CONFIG_INT(name, var, def) int var = (def)

struct menu_entry;
typedef void (*menu_select_fn)(void * priv, int delta);
typedef void (*menu_update_fn)(struct menu_entry * entry, struct menu_display_info * info);
struct menu_entry
{
    const char * name;
    menu_select_fn select;
    menu_update_fn update;
    const char * help, * help2;
    int depends_on;
    struct menu_entry * children;
    int * priv;
    int max;
    const char ** choices;
    int icon_type;
};
#define MENU_SELECT_FUNC(n) void n(void * priv, int delta)
#define MENU_UPDATE_FUNC(n) void n(struct menu_entry * entry, struct menu_display_info * info)
#define MENU_SET_VALUE(...)   snprintf(info->value, sizeof(info->value), __VA_ARGS__)
#define MENU_SET_HELP(...)    snprintf(info->help, sizeof(info->help), __VA_ARGS__)
#define MENU_SET_WARNING(l, ...) snprintf(info->warning, sizeof(info->warning), __VA_ARGS__)
#define MENU_WARN_NOT_WORKING 1
#define CHOICES(...) ((const char * []) { __VA_ARGS__ })
#define DEP_MOVIE_MODE 1
#define IT_ALWAYS_ON 1
#define MENU_EOL { .name = 0 }
void menu_add(const char * parent, struct menu_entry * e, int n);
void gui_stop_menu(void);

struct lvinfo_item;
typedef void (*lvinfo_update_fn)(struct lvinfo_item * item, int can_draw);
struct lvinfo_item { const char * name; int which_bar; lvinfo_update_fn update; int preferred_position; int color_fg; };
#define LVINFO_UPDATE_FUNC(n) void n(struct lvinfo_item * item, int can_draw)
#define LVINFO_BUFFER(n) char buffer[n]; (void) buffer
#define LV_BOTTOM_BAR_ONLY 1
void lvinfo_add_items(struct lvinfo_item * items, int n);

#endif
