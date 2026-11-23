/* shared helpers for the PC simulators: ML fonts, ML palette, a synthetic LiveView, frame output */
#ifndef SIMLIB_H
#define SIMLIB_H
#include <stdint.h>
#include "../modules/moth_ui/ui_text.h"
extern uint8_t front[UI_W * UI_H];
extern uint8_t lv[UI_W * UI_H * 3];
struct ui_rbf * load_rbf(const char * path);
void load_fonts(const char * dir);
void pal(uint8_t i, int * r, int * g, int * b);
void make_lv(void);
void write_frame(const char * dir, int n);
#endif
