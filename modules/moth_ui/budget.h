/*
 * Data-rate model for RAW video on the Canon EOS M.
 *
 * Integer maths only and no Magic Lantern headers, so the same file is
 * compiled into the camera module and into the PC tests (tests/).
 *
 * The model:   kB/s = pixels/frame x frames/s x compressed bits/pixel / 8
 * "compressed bits/pixel" starts from a table of typical values for
 * Canon's lossless JPEG at each bit depth and is replaced, per bit depth,
 * by what the camera measured on your own clips (budget_learn).
 */
#ifndef EOSM_BUDGET_H
#define EOSM_BUDGET_H

#include <stdint.h>

struct rec_format
{
    int width, height;      /* recorded RAW frame, in pixels (before any 1x3 de-squeeze) */
    int fps_x1000;          /* 23976 for 23.976 fps */
    int bits;               /* 10, 11, 12 or 14 */
    int lossless;           /* 1 = Canon lossless JPEG (LJ92), 0 = uncompressed */
};

enum budget_verdict
{
    BUDGET_UNKNOWN = 0,
    BUDGET_CONTINUOUS,      /* needs <= 85% of the card: records until the card is full */
    BUDGET_MARGINAL,        /* 85..105%: depends on the scene (detail and noise compress worse) */
    BUDGET_BURST,           /* > 105%: the RAM buffer fills, recording stops after a while */
};

/* compressed bits per pixel x100, for 10, 11, 12, 13, 14-bit (index bits - 10) */
struct bpp_table { int x100[5]; };

void budget_default_table(struct bpp_table * t);
int  budget_bpp_x100(const struct rec_format * f, const struct bpp_table * t);
int  budget_kBps(const struct rec_format * f, const struct bpp_table * t);   /* 1 kB = 1000 bytes */
enum budget_verdict budget_verdict(int need_kBps, int card_kBps);
int  budget_ratio_q8(int need_kBps, int card_kBps);       /* need / card, 256 = 100%, clamped 0..512 */
int  budget_minutes_on_card(int need_kBps, int free_MB);  /* -1 if unknown */

/* A clip finished: `kB` written in `ms` milliseconds at format `f`.
 * Returns the measured bits/pixel x100, or 0 if the clip is too short to trust. */
int  budget_measure_bpp_x100(const struct rec_format * f, uint32_t kB, uint32_t ms);

/* Blend a measurement into the table (exponential moving average, weight 1/4). */
void budget_learn(struct bpp_table * t, int bits, int measured_x100);

/* SD clock preset (0 = stock, 1 = 160 MHz, 2 = 192, 3 = 240) -> typical EOS M write speed, kB/s */
int  budget_card_kBps_for_clock(int sd_clock_preset);

#endif
