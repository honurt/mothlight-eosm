/*
 * Recording profiles: one name for a whole Crop Mood + MLV Lite setup.
 *
 * A profile stores the *menu positions* of the Crop Mood module (mode,
 * preset, aspect ratio, frame rate, bit depth), so applying it is the
 * same as a person setting those menus by hand -- nothing in Magic
 * Lantern's core is touched. `nominal` is what the profile records on
 * an EOS M, used for the data-rate budget before it is applied.
 *
 * No Magic Lantern headers: compiled into the PC tests as well.
 */
#ifndef EOSM_PROFILES_H
#define EOSM_PROFILES_H

#include "budget.h"

enum crop_mood { MOOD_OFF = 0, MOOD_1X1, MOOD_1X3, MOOD_3X3 };

/* Crop Mood menu indices on the EOS M */
enum { P11_2_5K, P11_2_8K, P11_3K, P11_1440P, P11_1280P, P11_FULLRES };   /* 1:1 "Preset:"   */
enum { P13_HIGHEST, P13_HIGHER, P13_MEDIUM };                             /* 1x3 "Preset: "  */
enum { P33_HFR, P33_1080P };                                              /* 3x3 "Preset:  " */
enum { AR_16_9, AR_2_1, AR_2_20, AR_2_35, AR_2_39 };
enum { FPS_23_976, FPS_25, FPS_30 };
enum { BITS_14, BITS_12, BITS_11, BITS_10 };

#define PROFILE_NAME_LEN 24
#define MAX_PROFILES     16

struct rec_profile
{
    char name[PROFILE_NAME_LEN];
    signed char mood, preset, ratio, fps, bits;
    signed char squeeze;            /* 3 for 1x3 anamorphic, else 1 */
    struct rec_format nominal;
};

int  profiles_builtin(struct rec_profile * out, int max);

/* Parse a PROFILES.TXT. Returns the number of profiles read; *bad_line is the first
 * line that could not be read (0 if none). Lines:
 *     name | mode | preset | ratio | fps | bits
 *     3K Scope | 1:1 | 3K | 2.35 | 23.976 | 10
 * mode: off 1:1 1x3 3x3   preset: 2.5K 2.8K 3K 1440p 1280p fullres | highest higher medium | hfr 1080p
 * ratio: 16:9 2:1 2.20 2.35 2.39   fps: 23.976 25 30   bits: 14 12 11 10      # starts a comment */
int  profiles_parse(const char * text, struct rec_profile * out, int max, int * bad_line);

/* fills p->nominal and p->squeeze from the menu positions */
void profile_fill_nominal(struct rec_profile * p);

int  profile_bits(const struct rec_profile * p);           /* 14, 12, 11 or 10 */
const char * profile_fps_str(const struct rec_profile * p);

/* "3072x1308  10-bit  23.976" (or "1736x2178 1x3  10-bit  23.976"); returns buf */
char * profile_summary(const struct rec_profile * p, char * buf, int len);

#endif
