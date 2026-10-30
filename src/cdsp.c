/* SPDX-License-Identifier: MPL-2.0 */
/* Shared fixed-point helpers of the prototype's other engines (Minimoog, Prophet, drums): noise, tanh, 1-exp(-x),
 * band-limited steps (the prototype's pbl), ADSR steps, the 'noise colour' filter of the Moog / Prophet. */
#include "c_tables_gen.h"

static uint32_t c_rng = 0x1234567u;
static inline int32_t c_rand15(void)                  /* uniform -32768..32767 */
{
    uint32_t x = c_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    c_rng = x;
    return (int32_t)(x >> 16) - 32768;
}
static inline uint32_t c_rand32(void) { c_rand15(); return c_rng; }

static inline uint32_t c_mulhi(uint32_t a, uint32_t b) { return (uint32_t)(((uint64_t)a * b) >> 32); }

static int32_t c_tanh(int32_t x15)                    /* tanh, Q15 in (clipped at +-4) and out */
{
    int32_t x = x15 >> 3, i, f, a;                    /* Q12 */
    if (x > 16383) x = 16383;
    if (x < -16384) x = -16384;
    x += 16384;
    i = x >> 7;
    f = x & 127;
    a = TANH_TAB[i];
    return a + (((int32_t)TANH_TAB[i + 1] - a) * f >> 7);
}

static uint32_t c_expm(uint32_t x16)                  /* 1 - exp(-x), x Q16 (0..4), result Q16 */
{
    uint32_t i, f, a;
    if (x16 >= 4u * 65536u) return 65535u;
    i = x16 >> 10;
    f = x16 & 1023u;
    a = EXPM_TAB[i];
    return a + (((int32_t)EXPM_TAB[i + 1] - (int32_t)a) * (int32_t)f >> 10);
}

static uint32_t c_1mexp(uint32_t x)                   /* 1 - exp(-x), x Q32 (small), Q32 */
{
    uint32_t x2 = c_mulhi(x, x), x3 = c_mulhi(x2, x);
    return x - (x2 >> 1) + x3 / 6u;
}

/* the polyBLEP residual of the prototype (pbl), Q15. t: phase Q32, d: increment Q32, rd = 2^32 / (d >> 8) */
static inline int32_t c_blep(uint32_t t, uint32_t d, uint32_t rd)
{
    uint32_t u, w;
    if (t < d) {
        u = (uint32_t)(((uint64_t)(t >> 8) * rd) >> 16);        /* Q16, 0..1 */
        w = 65536u - u;
        return -(int32_t)(((w >> 1) * (w >> 1)) >> 15);           /* -(1-u)^2 in Q15 */
    }
    if (t > (uint32_t)(0u - d)) {
        u = (uint32_t)(((uint64_t)((0u - t) >> 8) * rd) >> 16);
        w = 65536u - u;
        return (int32_t)(((w >> 1) * (w >> 1)) >> 15);
    }
    return 0;
}
static inline uint32_t c_rd(uint32_t d) { uint32_t dd = d >> 8; return dd ? 0xFFFFFFFFu / dd : 0xFFFFFFFFu; }

/* the prototype's mosc(): ty 0 triangle, 1 saw, 2 square, 3 pulse of width pw (Q32 turns), Q15 */
static int32_t c_mosc(uint32_t ph, uint32_t d, uint32_t rd, int ty, uint32_t pw)
{
    uint32_t w;
    int32_t v;
    if (ty == 0) {
        int32_t s = (int32_t)(ph - 0x80000000u);
        uint32_t a = s < 0 ? (uint32_t)(-s) : (uint32_t)s;
        return 32768 - (int32_t)(a >> 15);
    }
    if (ty == 1)
        return (int32_t)(ph >> 16) - 32768 - c_blep(ph, d, rd);
    w = ty == 2 ? 0x80000000u : pw;
    v = ph < w ? 32767 : -32767;
    return v + c_blep(ph, d, rd) - c_blep(ph - w, d, rd);
}

/* ADSR of the prototype (envStep), Q30 */
typedef struct { uint8_t st; int32_t e; } adsr_t;
static inline int32_t c_adsr(adsr_t *s, int32_t ai, uint32_t dc, int32_t su, uint32_t rc)
{
    switch (s->st) {
    case 1: s->e += ai; if (s->e >= (1 << 30)) { s->e = 1 << 30; s->st = 2; } break;
    case 2: s->e += (int32_t)(((int64_t)(su - s->e) * dc) >> 32);
            if ((s->e > su ? s->e - su : su - s->e) < 858993) s->st = 3; break;
    case 3: s->e = su; break;
    case 4: s->e -= (int32_t)(((int64_t)s->e * rc) >> 32); if (s->e < 536871) { s->e = 0; s->st = 0; } break;
    }
    return s->e;
}

/* the noise source of the Moog / Prophet: white, pink, brown, blue, crackle; tone = one-pole LP (below the centre)
 * or HP (above) and 'crush' = sample-and-hold of 1..49 samples */
typedef struct { int32_t b[7], br, pw, v, l; uint32_t c; } noise_t;
typedef struct { uint8_t lp; uint8_t type; uint16_t hold; uint32_t k16; } noisec_t;   /* k16: one-pole coefficient, 0 = off */

static void noise_coef(noisec_t *n, int type, int tone, int crush)       /* tone, crush in thousandths */
{
    int32_t lx;
    n->type = (uint8_t)type;
    n->lp = tone < 500;
    n->hold = (uint16_t)(1 + (crush * crush * 48) / 1000000);
    if (tone == 500) { n->k16 = 0; return; }
    if (tone < 500) lx = C_L2_NZ0 + (tone * 2 * C_L2_NZR) / 1000;
    else lx = C_L2_NZ1 + ((tone - 500) * 2 * C_L2_NZS) / 1000;
    n->k16 = c_expm(exp2_fx(lx, 16));
}

static int32_t noise_step(noise_t *e, const noisec_t *c)                  /* Q15 */
{
    int32_t nw = c_rand15(), sb;
    if (++e->c >= c->hold) {
        int32_t x = nw;
        e->c = 0;
        switch (c->type) {
        case 1:
            e->b[0] = (e->b[0] * 32730 >> 15) + (nw * 1819 >> 15);       /* .99886 / .0555179 */
            e->b[1] = (e->b[1] * 32549 >> 15) + (nw * 2460 >> 15);
            e->b[2] = (e->b[2] * 31752 >> 15) + (nw * 5041 >> 15);
            e->b[3] = (e->b[3] * 28392 >> 15) + (nw * 10173 >> 15);
            e->b[4] = (e->b[4] * 18022 >> 15) + (nw * 17464 >> 15);
            e->b[5] = (e->b[5] * -24957 >> 15) - (nw * 554 >> 15);
            x = ((e->b[0] + e->b[1] + e->b[2] + e->b[3] + e->b[4] + e->b[5] + e->b[6] + (nw * 17570 >> 15)) * 3604) >> 15;
            e->b[6] = nw * 3799 >> 15;
            break;
        case 2:
            e->br = (int32_t)(((int64_t)(e->br + nw * 655 / 32768) * 64333) >> 16);   /* (br + .02 nw) / 1.02 */
            x = e->br * 7 / 2;
            if (x > 32767) x = 32767;
            if (x < -32767) x = -32767;
            break;
        case 3: x = (nw - e->pw) >> 1; e->pw = nw; break;
        case 4: x = (c_rand32() & 0xFFFu) < 16u ? nw * 13 / 8 : 0; break;     /* p = .004 */
        }
        e->v = x;
    }
    sb = e->v;
    if (c->k16) {
        e->l += (int32_t)(((int64_t)(sb - e->l) * c->k16) >> 16);
        sb = c->lp ? e->l : sb - e->l;
    }
    return sb;
}
