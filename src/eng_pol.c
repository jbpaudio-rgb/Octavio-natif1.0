/* SPDX-License-Identifier: MPL-2.0 */
/* POL, one voice per track: a Polivoks (the Soviet 1982 monosynth) in the spirit of its circuit.
 *  - two oscillators, each a triangle -> saw -> pulse morph (the pulse has its width), O2 can be hard-synced to O1; a noise source and a mixer;
 *  - the famous filter: a 12 dB/oct state-variable that goes from low-pass to band-pass (MODE), with a resonance that bites and a band-pass state that clips
 *    on its own (that is where its snarl comes from), driven by the mixer's DRIVE;
 *  - a triangle LFO that can move the pitch, the pulse widths (PWM) and the cutoff; an amplifier ADSR and a filter attack / decay. */
#include "voices.h"                                    /* cdsp.c is already included by eng_moog.c */

typedef struct {
    int16_t p[PL_COUNT];
    uint8_t dirty, gate;
    int32_t oct16, o2s16, cut16, q16, env5, lev, drv, m1, m2, mn, lp, lw, lf, mode;
    uint32_t pw1, pw2;
    int32_t ai, sus, fai;
    uint32_t dc, rc, fdc, inc3;
    uint32_t ph1, ph2, ph3;
    int32_t note16, vel, lfo, s_lp, s_bp;
    adsr_t a, f;
} pol_t;

static pol_t PL[4];
static const int16_t PL_DEF[PL_COUNT] = { 500, 503, 500, 200, 500, 200, 0, 800, 700, 0, 300, 650, 450, 0, 400, 400, 0, 0, 0, 10, 450, 650, 300, 10, 400, 1000 };
static const int16_t PL_MAX[PL_COUNT] = { 1000, 1000, 1000, 1000, 1000, 1000, 1, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000 };

static inline int32_t pl_sat(int32_t x)                /* soft clip, shifts only: x - x^3 / (6.75 * 2^30), up to 1.5 (49152), flat beyond */
{
    int32_t t;
    if (x > 49152) x = 49152;
    if (x < -49152) x = -49152;
    t = (int32_t)((((int64_t)x * x >> 15) * x >> 15) * 19 >> 7);
    return x - t;
}

static void pol_derive(pol_t *m)
{
    const int16_t *p = m->p;
    int d = (p[PL_TUNE] - 500) * 2, ad = d < 0 ? -d : d, r = 1000 - p[PL_RES];
    int32_t sh = (int32_t)(((int64_t)ad * ad * 2505) >> 16);
    m->oct16 = d < 0 ? -sh : sh;
    m->o2s16 = (int32_t)(((int64_t)(p[PL_O2T] - 500) * 131072) / 1000);        /* +-12 semitones */
    m->inc3 = exp2_fx(999424 + p[PL_LR] * 697, 0);                             /* 0.05 Hz .. 80 Hz, per 8-sample chunk */
    m->m1 = p[PL_M1] * 32768 / 1000; m->m2 = p[PL_M2] * 32768 / 1000; m->mn = p[PL_MN] * 32768 / 1000;
    m->drv = 32768 + p[PL_DRV] * 98;                                            /* the filter input: 1 .. 4x before the clipper */
    m->cut16 = (p[PL_CUT] * 602931) / 1000;
    m->q16 = (int32_t)(((int64_t)r * r * 131072) / 1000000) + 600;           /* the damping 2 .. 0.05: it rings, then it screams */
    m->mode = p[PL_MOD] * 32768 / 1000;
    m->env5 = p[PL_ENV] * 5 * 65536 / 1000;
    m->lp = p[PL_LP] * 32768 / 1000;                                            /* LFO -> pitch: up to +-0.5 octave */
    m->lw = p[PL_LW];                                                           /* LFO -> pulse widths */
    m->lf = p[PL_LF] * 262144 / 1000;                                           /* LFO -> cutoff: up to +-4 octaves */
    m->pw1 = 0x80000000u - (uint32_t)p[PL_O1P] * 1932735u;                       /* width .5 .. .05 */
    m->pw2 = 0x80000000u - (uint32_t)p[PL_O2P] * 1932735u;
    m->lev = p[PL_LVL] * 32768 / 1000;
    m->ai = (int32_t)exp2_fx(C_L2_AI0 - (p[PL_ATK] * C_L2_ATK) / 1000, 0);
    m->fai = (int32_t)exp2_fx(C_L2_AI0 - (p[PL_FATK] * C_L2_ATK) / 1000, 0);
    m->dc = c_1mexp(exp2_fx(C_L2_DC0 - (p[PL_DEC] * C_L2_DCR) / 1000, 0));
    m->fdc = c_1mexp(exp2_fx(C_L2_DC0 - (p[PL_FDEC] * C_L2_DCR) / 1000, 0));
    m->rc = c_1mexp(exp2_fx(C_L2_DC0 - (p[PL_REL] * C_L2_DCR) / 1000, 0));
    m->sus = p[PL_SUS] * 1073742;
    m->dirty = 0;
}

void pol_set(int t, int id, int v)
{
    pol_t *m = &PL[t];
    if (id < 0 || id >= PL_COUNT) return;
    if (v < 0) v = 0;
    if (v > PL_MAX[id]) v = PL_MAX[id];
    m->p[id] = (int16_t)v;
    m->dirty = 1;
}
int pol_get(int t, int id) { return PL[t].p[id]; }

void pol_init(int t)
{
    pol_t *m = &PL[t];
    int i;
    for (i = 0; i < (int)sizeof(*m); i++) ((uint8_t *)m)[i] = 0;
    for (i = 0; i < PL_COUNT; i++) m->p[i] = PL_DEF[i];
    m->ph1 = 0x2468ACE1u * (uint32_t)(t + 1); m->ph2 = 0x13579BDFu * (uint32_t)(t + 3);
    m->note16 = 60 << 16;
    m->vel = 32767;
    pol_derive(m);
}

void pol_on(int t, int note, int vel15)
{
    pol_t *m = &PL[t];
    m->vel = vel15;
    m->note16 = note << 16;
    m->gate = 1;
    m->a.st = 1;
    m->f.st = 1;
}
void pol_off(int t)
{
    pol_t *m = &PL[t];
    m->gate = 0;
    if (m->a.st) m->a.st = 4;
}
int pol_active(int t) { return PL[t].a.st != 0; }

/* triangle (0) -> saw (500) -> pulse of width pw (1000), +-16k */
static inline int32_t pol_osc(uint32_t ph, uint32_t d, uint32_t rd, int morph, uint32_t pw)
{
    int32_t tr = c_mosc(ph, d, rd, 0, 0) >> 1, sw, pu;
    if (morph <= 0) return tr;
    sw = c_mosc(ph, d, rd, 1, 0) >> 1;
    if (morph < 500) return tr + (int32_t)(((int64_t)(sw - tr) * (morph * 65)) >> 15);
    pu = c_mosc(ph, d, rd, 3, pw) >> 1;
    return sw + (int32_t)(((int64_t)(pu - sw) * ((morph - 500) * 65)) >> 15);
}

void pol_render(int t, int32_t *out, int n)
{
    pol_t *m = &PL[t];
    int i = 0, j;
    if (m->dirty) pol_derive(m);
    while (i < n) {
        int cnt = n - i < 8 ? n - i : 8;
        int32_t ef = m->f.e, nn, xl, lg2, cm, x16, g, pwm, ratio;
        uint32_t inc1, inc2, rd1, rd2, pw1, pw2;
        int64_t pq;
        m->ph3 += m->inc3;
        { int32_t u = (int32_t)(m->ph3 >> 16); m->lfo = (u < 32768 ? u : 65535 - u) * 2 - 32768; }      /* triangle */
        nn = (int32_t)(((int64_t)(m->note16 - (60 << 16)) * 21845) >> 18);
        xl = C_L2_MINC + nn + m->oct16 + (int32_t)(((int64_t)m->lfo * m->lp) >> 15);
        lg2 = xl + m->o2s16;
        inc1 = exp2_fx(xl, 0); rd1 = exp2_fx((40 << 16) - xl, 0);
        inc2 = exp2_fx(lg2, 0); rd2 = exp2_fx((40 << 16) - lg2, 0);
        ratio = (int32_t)exp2_fx(m->o2s16 + (16 << 16), 0);                                  /* for the hard sync: O2's phase when O1 restarts */
        pwm = (int32_t)(((int64_t)m->lfo * m->lw) * 52 >> 0);                                          /* +-0.4 turn at full: 2^32 * .4 / (32768 * 1000) = 52.4 */
        pq = (int64_t)m->pw1 + pwm; if (pq < 214748365) pq = 214748365; if (pq > 2147483648LL) pq = 2147483648LL; pw1 = (uint32_t)pq;
        pq = (int64_t)m->pw2 + pwm; if (pq < 214748365) pq = 214748365; if (pq > 2147483648LL) pq = 2147483648LL; pw2 = (uint32_t)pq;
        cm = m->cut16 + nn * 7 / 10 + (int32_t)(((int64_t)m->lfo * m->lf) >> 15) + (int32_t)(((int64_t)m->env5 * ef) >> 30);
        x16 = (int32_t)exp2_fx(C_L2_FC0 + cm, 16);
        if (x16 > 82354 || x16 < 0) x16 = 82354;
        g = (int32_t)c_expm((uint32_t)x16);
        if (g > 36000) g = 36000;                       /* a 2-pole with its one-sample feedback delay stays stable up to about here */
        for (j = 0; j < cnt; j++, i++) {
            int32_t o1, o2, s, u, e, o, hp, bp, lp8;
            uint32_t old = m->ph1;
            m->ph1 += inc1;
            if (m->p[PL_SYN] && m->ph1 < old) m->ph2 = (uint32_t)(((uint64_t)m->ph1 * (uint32_t)ratio) >> 16);   /* hard sync */
            else m->ph2 += inc2;
            o1 = pol_osc(m->ph1, inc1, rd1, m->p[PL_O1W], pw1);
            o2 = pol_osc(m->ph2, inc2, rd2, m->p[PL_O2W], pw2);
            s = (int32_t)(((int64_t)o1 * m->m1 + (int64_t)o2 * m->m2 + (int64_t)(c_rand15() >> 1) * m->mn) >> 15);
            s = (int32_t)(((int64_t)s * m->drv) >> 15);
            u = pl_sat(s + (c_rand15() >> 10));
            lp8 = m->s_lp + (int32_t)(((int64_t)m->s_bp * g) >> 16);                                        /* the state-variable, 8 extra bits */
            hp = (u << 8) - lp8 - (int32_t)(((int64_t)m->q16 * m->s_bp) >> 16);
            bp = m->s_bp + (int32_t)(((int64_t)hp * g) >> 16);
            bp = pl_sat(bp >> 8) << 8;                                                                      /* the band-pass clips by itself */
            m->s_lp = lp8; m->s_bp = bp;
            lp8 >>= 8; bp >>= 8;
            s = lp8 + (int32_t)(((int64_t)(bp - lp8) * m->mode) >> 15);
            e = c_adsr(&m->a, m->ai, m->dc, m->sus, m->rc);
            (void)c_adsr(&m->f, m->fai, m->fdc, m->sus, m->rc);
            o = (int32_t)(((int64_t)s * (e >> 15)) >> 15);
            o = (int32_t)(((int64_t)o * m->vel) >> 15);
            o = pl_sat((int32_t)((int64_t)o * 40000 >> 15));
            out[i] = (int32_t)((int64_t)o * m->lev >> 15);
        }
    }
}
