#include "profiles.h"

#ifdef MODULE
    #include <dryos.h>          /* snprintf, strlen, ... inside Magic Lantern */
#else
    #include <stdio.h>
    #include <string.h>
#endif

/* ---- what Crop Mood records on an EOS M (from the module's own help text, Crop Mood 2023-2026) */

/* 1:1 presets: full width, and the tallest frame they can give */
static const short w11[] = { 2520, 2880, 3072, 2560, 1920, 5208 };
static const short h11[] = { 1080, 1226, 1308, 1440, 1280, 3478 };

/* 1x3 presets on the EOS M: [ratio][preset] -> RAW width x height (de-squeezed width = 3 x RAW width) */
static const short w13[5][3] = {
    { 1504, 1376, 1280 }, { 1600, 1472, 1360 }, { 1664, 1552, 1424 },
    { 1736, 1600, 1472 }, { 1736, 1600, 1472 },
};
static const short h13[5][3] = {
    { 2538, 2322, 2160 }, { 2400, 2208, 2040 }, { 2268, 2216, 1942 },
    { 2214, 2040, 1878 }, { 2178, 2008, 1846 },
};

/* 3x3 high frame rate: height and frame rate by aspect ratio */
static const short h33_hfr[]   = { 976, 868, 790, 738, 694 };
static const int   fps33_hfr[] = { 46800, 50000, 54000, 57000, 60000 };

static const int fps_x1000[] = { 23976, 25000, 29970 };
static const int ratio_x100[] = { 178, 200, 220, 235, 239 };
static const int bits_val[] = { 14, 12, 11, 10 };

int profile_bits(const struct rec_profile * p)
{
    return bits_val[(p->bits >= 0 && p->bits < 4) ? p->bits : 0];
}

const char * profile_fps_str(const struct rec_profile * p)
{
    static const char * s[] = { "23.976", "25", "30" };
    return s[(p->fps >= 0 && p->fps < 3) ? p->fps : 0];
}

void profile_fill_nominal(struct rec_profile * p)
{
    struct rec_format * f = &p->nominal;
    int r = (p->ratio >= 0 && p->ratio < 5) ? p->ratio : 0;
    int fi = (p->fps >= 0 && p->fps < 3) ? p->fps : 0;

    f->bits = profile_bits(p);
    f->lossless = 1;
    f->fps_x1000 = fps_x1000[fi];
    p->squeeze = 1;

    switch (p->mood)
    {
        case MOOD_1X1:
        {
            int i = (p->preset >= 0 && p->preset < 6) ? p->preset : 0;
            f->width = w11[i];
            /* height follows the ratio, up to what the preset can read out */
            int h = 2 * ((w11[i] * 100 + ratio_x100[r]) / (2 * ratio_x100[r]));   /* nearest even */
            f->height = h < h11[i] ? h : h11[i];
            if (i == P11_FULLRES) f->fps_x1000 = 3000;
            if (i == P11_3K) f->fps_x1000 = 23976;   /* 3K runs at 23.976 only */
            break;
        }
        case MOOD_1X3:
        {
            int i = (p->preset >= 0 && p->preset < 3) ? p->preset : 0;
            f->width  = w13[r][i];
            f->height = h13[r][i];
            p->squeeze = 3;
            if (i == P13_HIGHEST && r == AR_16_9) f->fps_x1000 = 22250;
            if (i == P13_HIGHEST && r == AR_2_1)  f->fps_x1000 = 23300;
            break;
        }
        case MOOD_3X3:
            f->width = 1736;
            if (p->preset == P33_HFR) { f->height = h33_hfr[r]; f->fps_x1000 = fps33_hfr[r]; }
            else                      { f->height = 1160; }
            break;
        default:                       /* Crop Mood off: Canon 1080p, 3x3 binned, 14-bit */
            f->width = 1736; f->height = 976; f->bits = 14;
            break;
    }
}

char * profile_summary(const struct rec_profile * p, char * buf, int len)
{
    const struct rec_format * f = &p->nominal;
    if (p->mood == MOOD_OFF && p->bits < 0)
        snprintf(buf, len, "Canon H.264, RAW off");
    else
        snprintf(buf, len, "%dx%d%s  %d-bit  %s",
                 f->width, f->height, p->squeeze == 3 ? " 1x3" : "",
                 f->bits, f->fps_x1000 == 3000 ? "3" : f->fps_x1000 == 46800 ? "46.8" :
                 f->fps_x1000 == 22250 ? "22.25" : f->fps_x1000 == 23300 ? "23.3" :
                 f->fps_x1000 >= 50000 ? (f->fps_x1000 == 50000 ? "50" : f->fps_x1000 == 54000 ? "54" :
                                          f->fps_x1000 == 57000 ? "57" : "60")
                                       : profile_fps_str(p));
    return buf;
}

/* ---- writing profiles back (the parser's words, in a readable case) */

static const char * const w_modes[]  = { "off", "1:1", "1x3", "3x3" };
static const char * const w_p11[]    = { "2.5K", "2.8K", "3K", "1440p", "1280p", "fullres" };
static const char * const w_p13[]    = { "highest", "higher", "medium" };
static const char * const w_p33[]    = { "hfr", "1080p" };
static const char * const w_ratios[] = { "16:9", "2:1", "2.20", "2.35", "2.39" };
static const char * const w_fps[]    = { "23.976", "25", "30" };
static const char * const w_bits[]   = { "14", "12", "11", "10" };

static const char * preset_word(const struct rec_profile * p)
{
    switch (p->mood)
    {
        case MOOD_1X1: return (p->preset >= 0 && p->preset < 6) ? w_p11[(int)p->preset] : "?";
        case MOOD_1X3: return (p->preset >= 0 && p->preset < 3) ? w_p13[(int)p->preset] : "?";
        case MOOD_3X3: return (p->preset >= 0 && p->preset < 2) ? w_p33[(int)p->preset] : "?";
    }
    return "";
}

int profile_format_line(const struct rec_profile * p, char * buf, int len)
{
    int m = (p->mood >= 0 && p->mood < 4) ? p->mood : 0;
    if (m == MOOD_OFF)
        return snprintf(buf, len, "%s | off |  |  |  | \n", p->name);
    return snprintf(buf, len, "%s | %s | %s | %s | %s | %s\n", p->name, w_modes[m], preset_word(p),
                    w_ratios[(p->ratio >= 0 && p->ratio < 5) ? p->ratio : 0],
                    w_fps[(p->fps >= 0 && p->fps < 3) ? p->fps : 0],
                    w_bits[(p->bits >= 0 && p->bits < 4) ? p->bits : 0]);
}

void profile_auto_name(const struct rec_profile * p, char * buf, int len)
{
    int r = (p->ratio >= 0 && p->ratio < 5) ? p->ratio : 0;
    if (p->mood == MOOD_OFF)       snprintf(buf, len, "My Canon H.264");
    else if (p->mood == MOOD_1X1)  snprintf(buf, len, "My %s %s %d-bit", preset_word(p), w_ratios[r], profile_bits(p));
    else if (p->mood == MOOD_1X3)  snprintf(buf, len, "My 1x3 %s %s", preset_word(p), w_ratios[r]);
    else                           snprintf(buf, len, "My 3x3 %s %d-bit", preset_word(p), profile_bits(p));
    if (len > PROFILE_NAME_LEN) buf[PROFILE_NAME_LEN - 1] = 0;
}

int profile_same_setup(const struct rec_profile * a, const struct rec_profile * b)
{
    if (a->mood != b->mood) return 0;
    if (a->mood == MOOD_OFF) return 1;
    return a->preset == b->preset && a->ratio == b->ratio && a->fps == b->fps && a->bits == b->bits;
}

/* ---- built-in profiles: a sensible set for the EOS M, best first */

static void add(struct rec_profile * out, int * n, int max, const char * name,
                int mood, int preset, int ratio, int fps, int bits)
{
    if (*n >= max) return;
    struct rec_profile * p = &out[(*n)++];
    snprintf(p->name, sizeof(p->name), "%s", name);
    p->mood = mood; p->preset = preset; p->ratio = ratio; p->fps = fps; p->bits = bits;
    profile_fill_nominal(p);
}

int profiles_builtin(struct rec_profile * out, int max)
{
    int n = 0;
    add(out, &n, max, "3K Scope",          MOOD_1X1, P11_3K,      AR_2_35, FPS_23_976, BITS_10);
    add(out, &n, max, "3K Wide 2.39",      MOOD_1X1, P11_3K,      AR_2_39, FPS_23_976, BITS_10);
    add(out, &n, max, "2.8K Scope",        MOOD_1X1, P11_2_8K,    AR_2_35, FPS_23_976, BITS_10);
    add(out, &n, max, "2.5K Cinema",       MOOD_1X1, P11_2_5K,    AR_2_35, FPS_23_976, BITS_10);
    add(out, &n, max, "2.5K 12-bit",       MOOD_1X1, P11_2_5K,    AR_2_39, FPS_23_976, BITS_12);
    add(out, &n, max, "1440p 16:9",        MOOD_1X1, P11_1440P,   AR_16_9, FPS_23_976, BITS_10);
    add(out, &n, max, "5.2K Anamorphic",   MOOD_1X3, P13_HIGHEST, AR_2_39, FPS_23_976, BITS_10);
    add(out, &n, max, "4.4K Anamorphic",   MOOD_1X3, P13_MEDIUM,  AR_2_39, FPS_23_976, BITS_10);
    add(out, &n, max, "1080p Full Sensor", MOOD_3X3, P33_1080P,   AR_16_9, FPS_23_976, BITS_12);
    add(out, &n, max, "Slow-mo 46.8p",     MOOD_3X3, P33_HFR,     AR_16_9, FPS_23_976, BITS_10);
    add(out, &n, max, "Canon H.264",       MOOD_OFF, 0,           AR_16_9, FPS_23_976, -1);
    return n;
}

/* ---- PROFILES.TXT */

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int word_eq(const char * a, int alen, const char * b)
{
    int i = 0;
    for (; i < alen && b[i]; i++)
        if (lower(a[i]) != lower(b[i])) return 0;
    return i == alen && b[i] == 0;
}

/* index of field [s, s+len) in a NULL-terminated list, or -1 */
static int pick(const char * s, int len, const char * const * list)
{
    for (int i = 0; list[i]; i++)
        if (word_eq(s, len, list[i])) return i;
    return -1;
}

int profiles_parse(const char * text, struct rec_profile * out, int max, int * bad_line)
{
    static const char * const modes[]   = { "off", "1:1", "1x3", "3x3", 0 };
    static const char * const p11[]     = { "2.5k", "2.8k", "3k", "1440p", "1280p", "fullres", 0 };
    static const char * const p13[]     = { "highest", "higher", "medium", 0 };
    static const char * const p33[]     = { "hfr", "1080p", 0 };
    static const char * const ratios[]  = { "16:9", "2:1", "2.20", "2.35", "2.39", 0 };
    static const char * const fpss[]    = { "23.976", "25", "30", 0 };
    static const char * const bitss[]   = { "14", "12", "11", "10", 0 };

    int n = 0, line_no = 0;
    if (bad_line) *bad_line = 0;

    for (const char * line = text; line && *line && n < max; )
    {
        const char * end = line;
        while (*end && *end != '\n') end++;
        line_no++;

        /* split on '|' into up to 6 trimmed fields; '#' starts a comment */
        const char * f[6]; int fl[6]; int nf = 0;
        const char * s = line;
        while (s < end && nf < 6)
        {
            const char * e = s;
            while (e < end && *e != '|' && *e != '#') e++;
            const char * a = s, * b = e;
            while (a < b && (*a == ' ' || *a == '\t')) a++;
            while (b > a && (b[-1] == ' ' || b[-1] == '\t' || b[-1] == '\r')) b--;
            f[nf] = a; fl[nf] = (int)(b - a); nf++;
            if (e >= end || *e == '#') break;
            s = e + 1;
        }

        int blank = (nf == 0) || (nf == 1 && fl[0] == 0);
        if (!blank)
        {
            struct rec_profile p;
            memset(&p, 0, sizeof(p));
            int ok = (nf == 6 && fl[0] > 0);
            if (ok)
            {
                int nl = fl[0] < PROFILE_NAME_LEN - 1 ? fl[0] : PROFILE_NAME_LEN - 1;
                memcpy(p.name, f[0], nl); p.name[nl] = 0;
                p.mood = pick(f[1], fl[1], modes);
                p.preset = p.mood == MOOD_1X1 ? pick(f[2], fl[2], p11) :
                           p.mood == MOOD_1X3 ? pick(f[2], fl[2], p13) :
                           p.mood == MOOD_3X3 ? pick(f[2], fl[2], p33) : 0;
                p.ratio = pick(f[3], fl[3], ratios);
                p.fps   = pick(f[4], fl[4], fpss);
                p.bits  = pick(f[5], fl[5], bitss);
                if (p.mood == MOOD_OFF) { p.preset = 0; if (p.ratio < 0) p.ratio = 0; if (p.fps < 0) p.fps = 0; p.bits = -1; }
                ok = p.mood >= 0 && p.preset >= 0 && p.ratio >= 0 && p.fps >= 0 && (p.bits >= 0 || p.mood == MOOD_OFF);
            }
            if (ok)
            {
                profile_fill_nominal(&p);
                out[n++] = p;
            }
            else if (bad_line && !*bad_line)
            {
                *bad_line = line_no;
            }
        }
        line = *end ? end + 1 : 0;
    }
    return n;
}
