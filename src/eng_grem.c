/* SPDX-License-Identifier: MPL-2.0 */
/* GREMLIN: an original, deliberately unruly and aggressive voice (not a model of any instrument), one voice per track.
 *  - two sine oscillators that phase-modulate each other and themselves (cross FM with feedback), B at a ratio of A;
 *  - HARD SYNC: A resets B on every cycle, so B screams as its ratio sweeps (the ratio is driven by the chaos source);
 *  - RING modulation A x B; a sine SUB an octave down for weight; a pitch DIVE at every note ("zap": up to 3 octaves, ~ 40 ms);
 *  - a triangle wave-folder, a hard DRIVE, a bit / sample-rate crusher, a short tuned COMB (a metallic, pitched resonance
 *    that tracks the note and tears apart when the pitch dives), then a 2-pole resonant low-pass;
 *  - a logistic-map CHAOS source (r from 3.5 to 3.99, stepped at RATE) drifts the ratio, the fold, the pitch, the cutoff;
 *  - GLITCH: every ~23 ms a dice roll may fire an event: a noise burst, a pitch jump of 1-3 octaves, a freeze (sample-and-hold
 *    buzz), a dropout, or a new random ratio. They all end by themselves. NOISE is only a thin bed. */
#include "voices.h"                                    /* cdsp.c is already included by eng_moog.c */

enum { GM_TUNE, GM_RAT, GM_XMD, GM_FLD, GM_CHS, GM_RTE, GM_NOI, GM_GLT, GM_CRU, GM_CUT, GM_RES, GM_ATK, GM_DEC, GM_REL, GM_LVL,
       GM_SYN, GM_RNG, GM_DRV, GM_SUB, GM_DIV, GM_CMB, GM_FBK, GM_BITE, GM_COUNT };
#define GM_CB 256

typedef struct {
    int16_t p[GM_COUNT];
    uint8_t dirty, gate;
    int32_t oct16, cut16, k16, lev, ai, sus;
    uint32_t dc, rc;
    uint32_t pa, pb, ps;
    int32_t note16, sa, sb, y1, y2, hold, vel, dv, fbl, up;
    uint32_t cx, cs, ca;                                /* logistic state Q16, smoothed Q16, step accumulator */
    int32_t pj16, rat16;                                /* event pitch jump (log2 Q16), event ratio override */
    uint16_t tick, nb_t, pj_t, fr_t, mu_t, rt_t, cr_n, ci, acc;
    adsr_t a;
} grem_t;

static grem_t GM[4];
static int16_t GM_CBUF[4][GM_CB];                       /* the comb's delay line (2 KB of RAM in all) */
static const int16_t GM_DEF[GM_COUNT] = { 500, 500, 420, 450, 600, 350, 25, 220, 200, 650, 250, 3, 450, 300, 1000,
                                          600, 250, 450, 300, 400, 350, 600, 600 };

static inline int32_t gm_sat(int32_t x)                 /* soft clip, up to +-32k */
{
    const int32_t A = 49152;
    if (x > A) x = A;
    if (x < -A) x = -A;
    return (int32_t)(x - ((int64_t)x * x / A) * x / A / 3);
}

static void grem_derive(grem_t *m)
{
    const int16_t *p = m->p;
    int d = (p[GM_TUNE] - 500) * 2, ad = d < 0 ? -d : d;
    int32_t sh = (int32_t)(((int64_t)ad * ad * 2505) >> 16);
    m->oct16 = d < 0 ? -sh : sh;
    m->cut16 = (p[GM_CUT] * 602931) / 1000;
    m->k16 = (int32_t)(((int64_t)p[GM_RES] * 196608) / 1000);
    m->lev = p[GM_LVL] * 32768 / 1000;
    m->ai = (int32_t)exp2_fx(C_L2_AI0 - (p[GM_ATK] * C_L2_ATK) / 1000, 0);
    m->dc = c_1mexp(exp2_fx(C_L2_DC0 - (p[GM_DEC] * C_L2_DCR) / 1000, 0));
    m->rc = c_1mexp(exp2_fx(C_L2_DC0 - (p[GM_REL] * C_L2_DCR) / 1000, 0));
    m->sus = 644245094;
    m->dirty = 0;
}

void grem_set(int t, int id, int v)
{
    grem_t *m = &GM[t];
    if (id < 0 || id >= GM_COUNT) return;
    if (v < 0) v = 0;
    if (v > 1000) v = 1000;
    m->p[id] = (int16_t)v;
    m->dirty = 1;
}
int grem_get(int t, int id) { return GM[t].p[id]; }

void grem_init(int t)
{
    grem_t *m = &GM[t];
    int i;
    for (i = 0; i < (int)sizeof(*m); i++) ((uint8_t *)m)[i] = 0;
    for (i = 0; i < GM_COUNT; i++) m->p[i] = GM_DEF[i];
    for (i = 0; i < GM_CB; i++) GM_CBUF[t][i] = 0;
    m->note16 = 60 << 16;
    m->cx = 39322u + (uint32_t)t * 3000u;                /* 0.6 .. 0.75 in Q16: the logistic map lives in 0..1 */
    m->cs = m->cx;
    m->vel = 32767;
    m->cr_n = 1;
    grem_derive(m);
}

void grem_on(int t, int note, int vel15)
{
    grem_t *m = &GM[t];
    m->vel = vel15;
    m->note16 = note << 16;
    m->dv = m->p[GM_DIV] * 3 * 65536 / 1000;            /* the zap: the pitch starts up to 3 octaves high and falls */
    m->gate = 1;
    m->a.st = 1;
}
void grem_off(int t)
{
    grem_t *m = &GM[t];
    m->gate = 0;
    if (m->a.st) m->a.st = 4;
}
int grem_active(int t) { return GM[t].a.st != 0; }

static void gm_dice(grem_t *m)                         /* every 1024 samples (~23 ms) */
{
    uint32_t r = c_rand32();
    if ((int32_t)((r >> 8) & 0xFFFF) * 1000 >= m->p[GM_GLT] * 4096 || !m->p[GM_GLT]) return;      /* ~ 6 % of the ticks at GLITCH = 1000 */
    switch ((r >> 24) % 5) {
    case 0: m->nb_t = 2 + (uint16_t)((r >> 4) & 3); break;
    case 1: m->pj16 = (int32_t)(1 + ((r >> 3) & 3)) * ((r & 0x400) ? 65536 : -65536); m->pj_t = 2 + (uint16_t)((r >> 5) & 7); break;
    case 2: m->fr_t = 2 + (uint16_t)((r >> 5) & 3); m->cr_n = 3 + (uint16_t)((r >> 12) & 31); break;
    case 3: m->mu_t = 1 + (uint16_t)((r >> 5) & 1); break;
    default: m->rat16 = (int32_t)((r >> 6) & 0x7FFFF) - 0x40000; m->rt_t = 3 + (uint16_t)((r >> 2) & 7); break;
    }
}

void grem_render(int t, int32_t *out, int n)
{
    grem_t *m = &GM[t];
    int16_t *cb = GM_CBUF[t];
    int i = 0, j;
    if (m->dirty) grem_derive(m);
    while (i < n) {
        int cnt = n - i < 8 ? n - i : 8;
        int32_t chs = m->p[GM_CHS], ch, lg, lb, fld, x16, g, nz, drv, clen, fbg;
        uint32_t ia, ib;
        /* chaos: a few logistic steps per chunk (RATE), smoothed */
        m->ca += exp2_fx(m->p[GM_RTE] * 9 * 65536 / 1000, 0) * 6u;
        while (m->ca >= 65536u) {
            m->ca -= 65536u;
            m->cx = (uint32_t)(((uint64_t)(229376u + (uint32_t)chs * 32u) * m->cx >> 16) * (65536u - m->cx) >> 16);
            if (m->cx < 2048u || m->cx > 63488u) m->cx = 16384u + (c_rand32() & 0x7FFFu);        /* it fell into a fixed point (or out of 0..1): kick it back in */
        }
        m->cs += (uint32_t)(((int32_t)m->cx - (int32_t)m->cs) / 4);
        ch = (int32_t)m->cs - 32768;                                                              /* -0.5 .. 0.5, Q16 */
        fbg = (int32_t)(((int64_t)m->p[GM_FBK] * (1024 + (int32_t)(((int64_t)ch * chs) >> 15))) >> 10);       /* the re-injection gain: the chaos swells and chokes it */
        if (fbg < 0) fbg = 0;
        m->dv -= m->dv >> 8;                                                                      /* the zap decays: ~ 40 ms */
        lg = C_L2_MINC + (int32_t)(((int64_t)(m->note16 - (60 << 16)) * 21845) >> 18) + m->oct16
             + (int32_t)(((int64_t)ch * chs) >> 12) + (m->pj_t ? m->pj16 : 0) + m->dv
             + (int32_t)(((int64_t)(m->y2 - m->fbl) * fbg) >> 9);               /* and the re-injected signal bends the pitch: a feedback shriek */           /* the pitch drifts by up to +-0.12 oct */
        lb = lg + (m->rt_t ? m->rat16 : ((m->p[GM_RAT] * 4 - 1000) * 65536 / 1000)) + (int32_t)(((int64_t)ch * chs) >> 7);   /* B: 1/2 .. 8 x A, swept hard by the chaos */
        ia = exp2_fx(lg, 0);
        ib = exp2_fx(lb, 0);
        fld = 32768 + (m->p[GM_FLD] * 6 * 32768) / 1000 + (int32_t)(((int64_t)ch * chs * 6) >> 10);      /* the fold gain: 1 .. 7, drifting */
        if (fld < 16384) fld = 16384;
        drv = 32768 + m->p[GM_DRV] * 8 * 32;                                                      /* 1x .. 9x */
        x16 = (int32_t)exp2_fx(C_L2_FC0 + m->cut16 + (int32_t)(((int64_t)ch * chs) >> 7) + (int32_t)(((int64_t)(m->y2 - m->fbl) * fbg) >> 8), 16);   /* ... and the cutoff */
        if (x16 > 82354 || x16 < 0) x16 = 82354;
        g = (int32_t)c_expm((uint32_t)x16);
        if (g > 36000) g = 36000;
        nz = m->p[GM_NOI] * 32 + (m->nb_t ? 9000 : 0);
        clen = (int32_t)exp2_fx((32 << 16) - lg, 0);                                              /* the comb: one period of the note ... */
        if (clen < 1) clen = 1;                                                                   /* (a very high note: 2^-n rounds to 0) */
        while (clen > GM_CB - 1) clen >>= 1;                                                       /* ... or of a lower octave of it */
        while (clen < 32) clen <<= 1;
        for (j = 0; j < cnt; j++, i++) {
            int32_t s, u, e, o, bits, cm, sub, rg;
            int32_t fbv = m->y2 - m->fbl;                       /* the output of the filter, without its DC: re-injected into the start of the voice */
            uint32_t xa = (uint32_t)(((int64_t)(m->sb + (m->sa >> 1)) * m->p[GM_XMD] * 166 + (int64_t)fbv * fbg * 90) & 0xFFFFFFFF);
            uint32_t xb = (uint32_t)(((int64_t)m->sa * m->p[GM_XMD] * 166) & 0xFFFFFFFF);
            uint32_t pa0 = m->pa;
            m->fbl += (m->y2 - m->fbl) >> 9;
            m->pa += ia; m->pb += ib; m->ps += ia >> 1;
            if (m->pa < pa0 && m->p[GM_SYN])                                                      /* A wrapped: hard-sync B (partly, below 1000) */
                m->pb = (uint32_t)(((uint64_t)m->pb * (uint32_t)(1000 - m->p[GM_SYN])) / 1000u);
            m->sa = SIN_TAB[((m->pa + xa) >> 22) & 1023u];
            m->sb = SIN_TAB[((m->pb + xb) >> 22) & 1023u];
            sub = (int32_t)(((int64_t)SIN_TAB[(m->ps >> 22) & 1023u] * m->p[GM_SUB]) >> 10);        /* the sub: up to 1/1 of the voice */
            rg = (int32_t)(((int64_t)m->sa * m->sb >> 15) * m->p[GM_RNG] / 1000);                  /* ring modulation */
            s = m->sa + (m->sb >> 1) + rg + (int32_t)(((int64_t)(c_rand15()) * nz) >> 15) + (int32_t)(((int64_t)fbv * fbg) >> 6);
            u = (int32_t)(((int64_t)s * fld) >> 15) + 32768;                                        /* triangle fold: reflect around +-32k */
            u &= 0x1FFFF;
            u = (u < 65536 ? u : 131071 - u) - 32768;
            { int32_t u0 = u; u += (int32_t)(((int64_t)(u0 - m->up) * m->p[GM_BITE]) >> 8); m->up = u0; }         /* the bite: a first-difference boost, the edges get sharp and hissy */
            u = gm_sat((int32_t)(((int64_t)u * drv) >> 15));                                       /* the hard drive */
            bits = 15 - m->p[GM_CRU] * 11 / 1000;                                                    /* crush: down to 4 bits */
            u = (u >> (15 - bits)) << (15 - bits);
            if (m->fr_t) { if (!(m->tick++ % m->cr_n)) m->hold = u; u = m->hold; }                   /* freeze: sample-and-hold at 1/cr_n of the rate */
            cm = cb[(m->ci - clen) & (GM_CB - 1)];                                                   /* the comb: a pitched, metallic ring */
            cm = (cm + cb[(m->ci - clen - 1) & (GM_CB - 1)]) >> 1;
            cb[m->ci & (GM_CB - 1)] = (int16_t)gm_sat(u + (int32_t)(((int64_t)cm * m->p[GM_CMB]) >> 10) * 15 / 16);
            m->ci++;
            u = gm_sat(u + (cm * m->p[GM_CMB] >> 10));
            u = gm_sat(u - (int32_t)(((int64_t)m->k16 * m->y2) >> 16));
            m->y1 += (int32_t)(((int64_t)(u - m->y1) * g) >> 16);
            m->y2 += (int32_t)(((int64_t)(m->y1 - m->y2) * g) >> 16);
            e = c_adsr(&m->a, m->ai, m->dc, m->sus, m->rc);
            o = (int32_t)(((int64_t)(m->y2 + sub) * (e >> 15)) >> 15);
            o = (int32_t)(((int64_t)o * m->vel) >> 15);
            if (m->mu_t) o = 0;                                                                      /* a dropout */
            out[i] = (int32_t)(((int64_t)gm_sat(o) * 26000 >> 15) * m->lev >> 15);
        }
        {
            m->acc = (uint16_t)(m->acc + cnt);
            if (m->acc >= 1024) {
                m->acc = (uint16_t)(m->acc - 1024);
                if (m->nb_t) m->nb_t--;
                if (m->pj_t) m->pj_t--;
                if (m->fr_t) m->fr_t--;
                if (m->mu_t) m->mu_t--;
                if (m->rt_t) m->rt_t--;
                gm_dice(m);
            }
        }
    }
}
