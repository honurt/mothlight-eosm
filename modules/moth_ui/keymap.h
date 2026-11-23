/* Mothlight: what a button does while the panel is open.
 *
 * Pure function so it can be tested on a PC with the exact event stream the EOS M sends
 * (tests/test_keys.c). Keys are ML's portable MODULE_KEY_* codes; anything ML cannot
 * translate (D-pad releases on the EOS M, GUI housekeeping events) arrives as 0.
 */
#ifndef MOTH_KEYMAP_H
#define MOTH_KEYMAP_H

#include "panel.h"

struct key_route { int panel_key; int pass_to_canon; };

static inline struct key_route moth_route_key(int key)
{
    struct key_route r = { PK_NONE, 1 };
    switch (key)
    {
        case MODULE_KEY_WHEEL_UP: case MODULE_KEY_WHEEL_LEFT: case MODULE_KEY_PRESS_UP:
            r.panel_key = PK_UP;    r.pass_to_canon = 0; break;
        case MODULE_KEY_WHEEL_DOWN: case MODULE_KEY_WHEEL_RIGHT: case MODULE_KEY_PRESS_DOWN:
            r.panel_key = PK_DOWN;  r.pass_to_canon = 0; break;
        case MODULE_KEY_PRESS_LEFT:  r.panel_key = PK_LEFT;  r.pass_to_canon = 0; break;
        case MODULE_KEY_PRESS_RIGHT: r.panel_key = PK_RIGHT; r.pass_to_canon = 0; break;
        case MODULE_KEY_PRESS_SET:   r.panel_key = PK_SET;   r.pass_to_canon = 0; break;

        case MODULE_KEY_PRESS_HALFSHUTTER: case MODULE_KEY_PRESS_FULLSHUTTER:
            r.panel_key = PK_CLOSE; r.pass_to_canon = 1; break;    /* close, still focus / shoot */
        case MODULE_KEY_MENU: case MODULE_KEY_INFO: case MODULE_KEY_PLAY:
        case MODULE_KEY_TRASH: case MODULE_KEY_Q: case MODULE_KEY_REC: case MODULE_KEY_LV:
            r.panel_key = PK_CLOSE; r.pass_to_canon = 0; break;    /* explicit close keys */

        case MODULE_KEY_TOUCH_1_FINGER: case MODULE_KEY_UNTOUCH_1_FINGER:
            r.pass_to_canon = 0; break;                            /* stray touch: ignore */
        case MODULE_KEY_UNPRESS_SET:
            r.pass_to_canon = 0; break;    /* Canon redraws its LiveView screen on it */
        default:
            break;    /* untranslated (0) and housekeeping events: never close, pass on.
                         The EOS M D-pad releases also arrive as 0; moth_ui.c swallows those
                         in its raw handler, which still sees the real button code. */
    }
    return r;
}

#endif
