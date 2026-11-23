/* Replays EOS M button streams through the panel key map.
 * Regression: D-pad releases reach modules as key 0 and used to close the panel. */
#include <stdio.h>
#include "module_keys.h"          /* MODULE_KEY_* extracted from the engine's module.h */
#include "../modules/moth_ui/keymap.h"

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

/* feed a stream; return number of PK_CLOSE produced and collect nav keys */
static int run(const int * keys, int n, int * nav, int * nav_n)
{
    int closes = 0; *nav_n = 0;
    for (int i = 0; i < n; i++)
    {
        struct key_route r = moth_route_key(keys[i]);
        if (r.panel_key == PK_CLOSE) closes++;
        else if (r.panel_key != PK_NONE) nav[(*nav_n)++] = r.panel_key;
    }
    return closes;
}

int main(void)
{
    int nav[32], nn;
    /* EOS M: press DOWN, release (untranslated -> 0), press DOWN, release, SET, release SET */
    const int dpad[] = { MODULE_KEY_PRESS_DOWN, 0, MODULE_KEY_PRESS_DOWN, 0,
                         MODULE_KEY_PRESS_RIGHT, 0, MODULE_KEY_PRESS_SET, MODULE_KEY_UNPRESS_SET };
    CHECK(run(dpad, 8, nav, &nn) == 0, "D-pad press/release must not close the panel");
    CHECK(nn == 4 && nav[0] == PK_DOWN && nav[1] == PK_DOWN && nav[2] == PK_RIGHT && nav[3] == PK_SET,
          "D-pad stream must navigate DOWN DOWN RIGHT SET");

    const int wheel[] = { MODULE_KEY_WHEEL_RIGHT, MODULE_KEY_WHEEL_RIGHT, MODULE_KEY_WHEEL_LEFT };
    CHECK(run(wheel, 3, nav, &nn) == 0 && nn == 3 && nav[2] == PK_UP, "rear wheel scrolls");

    const int touch[] = { MODULE_KEY_TOUCH_1_FINGER, 0, MODULE_KEY_UNTOUCH_1_FINGER };
    CHECK(run(touch, 3, nav, &nn) == 0, "a stray touch must not close the panel");

    const int closers[] = { MODULE_KEY_INFO, MODULE_KEY_MENU, MODULE_KEY_PLAY, MODULE_KEY_REC,
                            MODULE_KEY_PRESS_HALFSHUTTER };
    for (int i = 0; i < 5; i++)
        CHECK(run(&closers[i], 1, nav, &nn) == 1, "INFO/MENU/PLAY/REC/half-shutter close");

    CHECK(moth_route_key(MODULE_KEY_PRESS_HALFSHUTTER).pass_to_canon == 1, "half-shutter still focuses");
    CHECK(moth_route_key(MODULE_KEY_PRESS_DOWN).pass_to_canon == 0, "nav keys are swallowed");
    CHECK(moth_route_key(0).pass_to_canon == 1, "untranslated events go on to Canon");

    printf(fails ? "%d key test(s) failed\n" : "key routing: all tests passed\n", fails);
    return fails != 0;
}
