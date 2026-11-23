/*
 * Animation timing in fixed point (1.0 = 256). No floats: the EOS M's ARM926
 * has no FPU, and soft-float per frame is wasted time the RAW recorder needs.
 */
#ifndef EOSM_UI_ANIM_H
#define EOSM_UI_ANIM_H

#include <stdint.h>

/* progress 0..256 of an animation that started at t0 and lasts dur ms */
static __attribute__((unused)) int anim_t_q8(uint32_t now, uint32_t t0, int dur)
{
    if (dur <= 0) return 256;
    uint32_t dt = now - t0;
    return dt >= (uint32_t)dur ? 256 : (int)(dt * 256 / dur);
}

/* fast start, soft landing: 1 - (1-t)^3 */
static __attribute__((unused)) int ease_out_q8(int t)
{
    if (t <= 0) return 0;
    if (t >= 256) return 256;
    int u = 256 - t;
    return 256 - ((((u * u) >> 8) * u) >> 8);
}

/* soft start and end: smoothstep 3t^2 - 2t^3 */
static __attribute__((unused)) int ease_in_out_q8(int t)
{
    if (t <= 0) return 0;
    if (t >= 256) return 256;
    int t2 = (t * t) >> 8;
    return (3 * t2) - ((2 * t2 * t) >> 8);
}

/* a little overshoot, for things that "land" (selection, toasts) */
static __attribute__((unused)) int ease_back_q8(int t)
{
    if (t <= 0) return 0;
    if (t >= 256) return 256;
    int u = t - 256;                    /* -256..0 */
    int u2 = (u * u) >> 8, u3 = (u2 * u) >> 8;
    /* 1 + 2.7 u^3 + 1.7 u^2 (Penner's back-out with s = 1.7) */
    return 256 + ((u3 * 691) >> 8) + ((u2 * 435) >> 8);
}

struct tween
{
    int from, to;
    uint32_t t0;
    uint16_t dur;
    uint8_t  curve;     /* 0 out, 1 in-out, 2 back */
};

static __attribute__((unused)) void tween_start(struct tween * tw, int from, int to, uint32_t now, int dur, int curve)
{
    tw->from = from; tw->to = to; tw->t0 = now; tw->dur = (uint16_t)dur; tw->curve = (uint8_t)curve;
}

static __attribute__((unused)) void tween_retarget(struct tween * tw, int current, int to, uint32_t now, int dur)
{
    if (tw->to == to) return;
    tween_start(tw, current, to, now, dur, tw->curve);
}

static __attribute__((unused)) int tween_done(const struct tween * tw, uint32_t now)
{
    return anim_t_q8(now, tw->t0, tw->dur) >= 256;
}

static __attribute__((unused)) int tween_value(const struct tween * tw, uint32_t now)
{
    int t = anim_t_q8(now, tw->t0, tw->dur);
    int e = tw->curve == 1 ? ease_in_out_q8(t) : tw->curve == 2 ? ease_back_q8(t) : ease_out_q8(t);
    return tw->from + (((tw->to - tw->from) * e) >> 8);
}

#endif
