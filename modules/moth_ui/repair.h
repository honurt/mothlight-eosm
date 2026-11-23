/* Mothlight: notice when something else drew over the panel.
 *
 * Frames only copy the rows that changed, so a foreign redraw (Canon refreshing its
 * LiveView overlay, an ML notification) would otherwise leave holes until those rows
 * change again. Probing ~100 pixels per frame catches it; the caller then repaints all.
 * Shared by the camera glue and the PC simulator (sim/e2e.c).
 */
#ifndef MOTH_REPAIR_H
#define MOTH_REPAIR_H

#include <stdint.h>

static const short moth_probe_rows[] = { 2, 20, 40, 51, 60, 140, 240, 330, 420, 440, 460, 478 };

static inline int moth_screen_differs(const uint8_t * front, int front_pitch,
                                      const uint8_t * back, int back_pitch, int w)
{
    for (unsigned i = 0; i < sizeof(moth_probe_rows) / sizeof(moth_probe_rows[0]); i++)
        for (int x = 7; x < w; x += 89)
        {
            int y = moth_probe_rows[i];
            if (front[y * front_pitch + x] != back[y * back_pitch + x]) return 1;
        }
    return 0;
}

#endif
