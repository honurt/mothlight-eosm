#include "budget.h"

/* Starting points for Canon lossless JPEG on the EOS M sensor at ISO 100-800.
 * Noise and fine detail raise them, so a clip of a starry sky at ISO 3200
 * compresses worse than a face against a wall. The camera replaces each value
 * with its own measurements (budget_learn), so these only matter at first. */
static const int default_lossless_x100[5] = {
    600,    /* 10-bit */
    680,    /* 11-bit */
    760,    /* 12-bit */
    830,    /* 13-bit (unused by Crop Mood, kept for the index) */
    900,    /* 14-bit */
};

void budget_default_table(struct bpp_table * t)
{
    for (int i = 0; i < 5; i++) t->x100[i] = default_lossless_x100[i];
}

static int bits_index(int bits)
{
    if (bits < 10) bits = 10;
    if (bits > 14) bits = 14;
    return bits - 10;
}

int budget_bpp_x100(const struct rec_format * f, const struct bpp_table * t)
{
    if (!f->lossless) return f->bits * 100;               /* uncompressed: exactly the bit depth */
    int v = t->x100[bits_index(f->bits)];
    return v > 0 ? v : default_lossless_x100[bits_index(f->bits)];
}

int budget_kBps(const struct rec_format * f, const struct bpp_table * t)
{
    if (f->width <= 0 || f->height <= 0 || f->fps_x1000 <= 0) return 0;
    /* kilo-pixels per frame (<= 18,200 on the EOS M), then per second (< 2^31 up to 100 fps) */
    int kpx_frame = (f->width * f->height + 500) / 1000;
    int kpx_s     = kpx_frame * (f->fps_x1000 / 10) / 100;
    /* kpx x bits / 8 = kB;  bits are x100 */
    return (int)((int64_t)kpx_s * budget_bpp_x100(f, t) / 800);
}

enum budget_verdict budget_verdict(int need_kBps, int card_kBps)
{
    if (need_kBps <= 0 || card_kBps <= 0) return BUDGET_UNKNOWN;
    if (need_kBps * 100 <= card_kBps * 85)  return BUDGET_CONTINUOUS;
    if (need_kBps * 100 <= card_kBps * 105) return BUDGET_MARGINAL;
    return BUDGET_BURST;
}

int budget_ratio_q8(int need_kBps, int card_kBps)
{
    if (card_kBps <= 0) return 0;
    int r = (int)((int64_t)need_kBps * 256 / card_kBps);
    return r < 0 ? 0 : r > 512 ? 512 : r;
}

int budget_minutes_on_card(int need_kBps, int free_MB)
{
    if (need_kBps <= 0 || free_MB < 0) return -1;
    return (int)((int64_t)free_MB * 1000 / need_kBps / 60);
}

int budget_measure_bpp_x100(const struct rec_format * f, uint32_t kB, uint32_t ms)
{
    if (ms < 3000 || kB < 1000) return 0;                 /* too short to say anything */
    if (f->width <= 0 || f->height <= 0 || f->fps_x1000 <= 0) return 0;
    /* frames = ms * fps / 1000 ; bits/pixel = kB * 8000 / (frames * pixels) */
    int64_t frames_x1000 = (int64_t)ms * f->fps_x1000 / 1000;   /* frames, x1000 */
    int64_t px_x1000 = frames_x1000 * f->width * f->height;
    if (px_x1000 <= 0) return 0;
    int64_t bpp_x100 = (int64_t)kB * 8000 * 100 * 1000 / px_x1000;
    /* a believable result lies between 2 bits and the uncompressed depth */
    if (bpp_x100 < 200 || bpp_x100 > f->bits * 100) return 0;
    return (int)bpp_x100;
}

void budget_learn(struct bpp_table * t, int bits, int measured_x100)
{
    if (measured_x100 <= 0) return;
    int i = bits_index(bits);
    int old = t->x100[i] > 0 ? t->x100[i] : default_lossless_x100[i];
    t->x100[i] = (old * 3 + measured_x100 + 2) / 4;
}

int budget_card_kBps_for_clock(int sd_clock_preset)
{
    /* EOS M with a fast UHS-I card (SanDisk Extreme / Extreme PRO class).
     * Stock: ~40 MB/s.  160 MHz: ~65-70 MB/s, the setting most cards tolerate.
     * 192 / 240 MHz work on fewer cards; when they don't, the card falls back to slow mode. */
    switch (sd_clock_preset)
    {
        case 1:  return 65000;
        case 2:  return 72000;
        case 3:  return 78000;
        default: return 38000;
    }
}
