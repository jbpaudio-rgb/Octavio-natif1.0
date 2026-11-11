/* SPDX-License-Identifier: MPL-2.0 */
/* SUPERSAW, one voice per track: seven polyBLEP (band-limited) saws, one in the centre and six detuned around it, the JP-8000 recipe.
 * DETUNE spreads them (up to +-60 cents at the outermost), MIX sets how much the side saws weigh against the centre one,
 * then a one-pole high-pass (HPF), a 12 dB resonant low-pass with a soft clipper and its envelope, and an ADSR.
 * The detune is done in the log domain (one exp2 per saw per 8 samples, no division): each saw's polyBLEP reciprocal is 2^(40 - log2 inc). */
#include "voices.h"                                    /* cdsp.c is already included by eng_moog.c */

enum { SS_TUNE, SS_RNG, SS_DET, SS_MIX, SS_HPF, SS_CUT, SS_RES, SS_ENV, SS_ATK, SS_DEC, SS_SUS, SS_REL, SS_FATK, SS_FDEC, SS_LVL, SS_DRF, SS_OCT, SS_COUNT };

typedef struct {
    int16_t p[SS_COUNT];
    uint8_t dirty;
    int32_t oct16, det16, cg, sg, hp_g, cut16, env5, k16, lev;
    int32_t ai, sus;
    uint32_t dc, rc, fdc;
    int32_t fai;
    uint32_t ph[8], lf[8];                              /* the 7 saws + the octave-up saw; the drift LFOs (7) and the shimmer LFO (7th..8th) */
    int32_t note16, y1, y2, hz;
    adsr_t a, f;
    uint8_t gate;
    int32_t vel;
} ssaw_t;

static ssaw_t SS[4];
static const int16_t SS_DEF[SS_COUNT] = { 500, 2, 500, 700, 150, 700, 150, 450, 15, 450, 750, 400, 10, 450, 1000, 450, 350 };
static const int16_t SS_MAX[SS_COUNT] = { 1000, 4, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000 };
static const int16_t SS_OFF[7] = { -1000, -630, -300, 0, 300, 630, 1000 };       /* the JP-8000's spread of the seven saws, in thousandths of the detune */

static inline int32_t ss_sat(int32_t x)                /* soft clip: x - x^3 / (3 A^2) up to A = 1.5 (49152) */
{
    const int32_t A = 49152;
    int64_t t;
    if (x > A) x = A;
    if (x < -A) x = -A;
    t = ((int64_t)x * x / A) * x / A / 3;
    return (int32_t)(x - t);
}

static void ssaw_derive(ssaw_t *m)
{
    const int16_t *p = m->p;
    int d = (p[SS_TUNE] - 500) * 2, ad = d < 0 ? -d : d;
    int32_t sh = (int32_t)(((int64_t)ad * ad * 2505) >> 16);
    m->oct16 = (p[SS_RNG] - 2) * 65536 + (d < 0 ? -sh : sh);
    m->det16 = (int32_t)(((int64_t)p[SS_DET] * 3277) / 1000);      /* up to 0.05 octave (60 cents) at the outer saws */
    m->cg = 32768 - (int32_t)((int64_t)p[SS_MIX] * 9830 / 1000);   /* the centre saw: 1 .. 0.7 */
    m->sg = (int32_t)((int64_t)p[SS_MIX] * 8192 / 1000);           /* each side saw: 0 .. 0.25 */
    m->hp_g = (int32_t)(c_expm((uint32_t)(p[SS_HPF] * 8 + 8)));    /* the high-pass one-pole: 0 .. a few hundred Hz */
    m->cut16 = (p[SS_CUT] * 602931) / 1000;
    m->env5 = p[SS_ENV] * 5 * 65536 / 1000;
    m->k16 = (int32_t)(((int64_t)p[SS_RES] * 196608) / 1000);      /* up to 3.0 on a 2-pole: it rings, it does not run away */
    m->lev = p[SS_LVL] * 32768 / 1000;
    m->ai = (int32_t)exp2_fx(C_L2_AI0 - (p[SS_ATK] * C_L2_ATK) / 1000, 0);
    m->fai = (int32_t)exp2_fx(C_L2_AI0 - (p[SS_FATK] * C_L2_ATK) / 1000, 0);
    m->dc = c_1mexp(exp2_fx(C_L2_DC0 - (p[SS_DEC] * C_L2_DCR) / 1000, 0));
    m->fdc = c_1mexp(exp2_fx(C_L2_DC0 - (p[SS_FDEC] * C_L2_DCR) / 1000, 0));
    m->rc = c_1mexp(exp2_fx(C_L2_DC0 - (p[SS_REL] * C_L2_DCR) / 1000, 0));
    m->sus = p[SS_SUS] * 1073742;
    m->dirty = 0;
}

void ssaw_set(int t, int id, int v)
{
    ssaw_t *m = &SS[t];
    if (id < 0 || id >= SS_COUNT) return;
    if (v < 0) v = 0;
    if (v > SS_MAX[id]) v = SS_MAX[id];
    m->p[id] = (int16_t)v;
    m->dirty = 1;
}
int ssaw_get(int t, int id) { return SS[t].p[id]; }

void ssaw_init(int t)
{
    ssaw_t *m = &SS[t];
    int i;
    for (i = 0; i < (int)sizeof(*m); i++) ((uint8_t *)m)[i] = 0;
    for (i = 0; i < SS_COUNT; i++) m->p[i] = SS_DEF[i];
    for (i = 0; i < 8; i++) { m->ph[i] = 0x2468ACE1u * (uint32_t)(i + 1); m->lf[i] = 0x13579BDFu * (uint32_t)(i + 3); }       /* free-running phases and LFOs, spread */
    m->note16 = 60 << 16;
    m->vel = 32767;
    ssaw_derive(m);
}

void ssaw_on(int t, int note, int vel15)
{
    ssaw_t *m = &SS[t];
    m->vel = vel15;
    m->note16 = note << 16;
    m->gate = 1;
    m->a.st = 1;
    m->f.st = 1;
}
void ssaw_off(int t)
{
    ssaw_t *m = &SS[t];
    m->gate = 0;
    if (m->a.st) m->a.st = 4;
}
int ssaw_active(int t) { return SS[t].a.st != 0; }

void ssaw_render(int t, int32_t *out, int n)
{
    ssaw_t *m = &SS[t];
    int i = 0, j, k;
    if (m->dirty) ssaw_derive(m);
    while (i < n) {
        int cnt = n - i < 8 ? n - i : 8;
        int32_t xl, ef = m->f.e, x16, g;
        uint32_t inc[8], rd[8];
        int32_t shim;
        xl = C_L2_MINC + (int32_t)(((int64_t)(m->note16 - (60 << 16)) * 21845) >> 18) + m->oct16;
        for (k = 0; k < 8; k++) m->lf[k] += 779000u * (uint32_t)(60 + 37 * k) / 1000u;       /* 0.06 .. 0.3 Hz, a different rate for each: the saws drift apart and together */
        for (k = 0; k < 7; k++) {
            int32_t dr = (int32_t)(((int64_t)SIN_TAB[(m->lf[k] >> 22) & 1023u] * m->p[SS_DRF] * 3) >> 17);       /* up to +-0.017 oct of analogue-style drift */
            int32_t lg = xl + (int32_t)(((int64_t)m->det16 * SS_OFF[k]) / 1000) + dr;
            inc[k] = exp2_fx(lg, 0);
            rd[k] = exp2_fx((40 << 16) - lg, 0);
        }
        inc[7] = exp2_fx(xl + 65536, 0);                  /* the octave-up saw ... */
        rd[7] = exp2_fx((40 << 16) - xl - 65536, 0);
        shim = (SIN_TAB[(m->lf[7] >> 22) & 1023u] + 32768) >> 1;      /* ... breathes in and out on a slow LFO: the shimmer, 0 .. 1 (Q15) */
        x16 = (int32_t)exp2_fx(C_L2_FC0 + m->cut16 + (int32_t)(((int64_t)m->env5 * ef) >> 30), 16);
        if (x16 > 82354 || x16 < 0) x16 = 82354;
        g = (int32_t)c_expm((uint32_t)x16);
        if (g > 36000) g = 36000;                       /* a 2-pole with its one-sample feedback delay stays stable up to about here */
        for (j = 0; j < cnt; j++, i++) {
            int32_t s = 0, sd = 0, so = 0, e, o, u;
            for (k = 0; k < 7; k++) {
                int32_t w;
                m->ph[k] += inc[k];
                w = (int32_t)(m->ph[k] >> 17) - 16384;                   /* rising ramp, +-16k */
                w -= c_blep(m->ph[k], inc[k], rd[k]) >> 1;                /* polyBLEP: half the 32k jump times the residual (-16 dB aliasing at 1 kHz, measured) */
                if (k == 3) s = w; else sd += w;
            }
            m->ph[7] += inc[7];
            so = (int32_t)(m->ph[7] >> 17) - 16384;
            so -= c_blep(m->ph[7], inc[7], rd[7]) >> 1;
            so = (int32_t)(((int64_t)so * shim >> 15) * m->p[SS_OCT] / 1000);
            s = (int32_t)(((int64_t)s * m->cg + (int64_t)sd * m->sg) >> 15) + (so >> 1);
            m->hz += (int32_t)(((int64_t)(s - m->hz) * m->hp_g) >> 16);   /* one-pole high-pass: s - lowpass(s) */
            s -= m->hz;
            u = ss_sat(s - (int32_t)(((int64_t)m->k16 * m->y2) >> 16));
            m->y1 += (int32_t)(((int64_t)(u - m->y1) * g) >> 16);
            m->y2 += (int32_t)(((int64_t)(m->y1 - m->y2) * g) >> 16);
            e = c_adsr(&m->a, m->ai, m->dc, m->sus, m->rc);
            (void)c_adsr(&m->f, m->fai, m->fdc, m->sus, m->rc);
            o = (int32_t)(((int64_t)m->y2 * (e >> 15)) >> 15);
            o = (int32_t)(((int64_t)o * m->vel) >> 15);
            o = ss_sat((int32_t)((int64_t)o * 64000 >> 15));        /* the gain first, then the clipper: never beyond +-32k */
            out[i] = (int32_t)((int64_t)o * m->lev >> 15);
        }
    }
}
