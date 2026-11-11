/* SPDX-License-Identifier: MPL-2.0 */
/* VC3, one voice per track: a VCS3 (EMS Synthi) in a fixed patch, with the matrix reduced to the pins that make the machine what it is.
 *  - two oscillators that morph sine -> saw -> square (band-limited), a third, slow one (O3) that is the modulator: sine, triangle, square or sample & hold;
 *  - a noise generator and the ring modulator (O1 x O2), the four sources go to the filter through their own levels (the matrix row of the filter input);
 *  - the "filter / oscillator": a 4-pole low-pass that rings and, turned up, oscillates by itself as a sine (the VCS3's trademark);
 *  - the pins that are left as amounts: O3 -> pitch and -> cutoff, O1 -> O2 frequency (X-MOD), the filter output back into O1's frequency (FBK),
 *    the envelope -> cutoff and -> pitch;
 *  - the envelope shaper as an ADSR on the amplifier, the same envelope moving the filter and the pitch. */
#include "voices.h"                                    /* cdsp.c is already included by eng_moog.c */

typedef struct {
    int16_t p[V3_COUNT];
    uint8_t dirty, gate;
    int32_t oct16, o2s16, cut16, k16, lev, key, o3p, o3f, xmd, enf, fbk, enp, m1, m2, mn, mr;
    int32_t ai, sus;
    uint32_t dc, rc, inc3;
    uint32_t ph1, ph2, ph3;
    int32_t note16, y1, y2, y3, y4, lfo, vel;
    adsr_t a;
} v3_t;

static v3_t V3[4];
static const int16_t V3_DEF[V3_COUNT] = { 500, 350, 700, 503, 350, 0, 800, 600, 0, 0, 600, 450, 500, 100, 150, 100, 450, 0, 0, 20, 500, 600, 350, 1000 };
static const int16_t V3_MAX[V3_COUNT] = { 1000, 1000, 1000, 1000, 1000, 3, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000 };

static inline int32_t v3_sat(int32_t x)                /* soft clip: x - x^3 / (3 A^2) up to A = 1.5 (49152), flat beyond */
{
    const int32_t A = 49152;
    int64_t t;
    if (x > A) x = A;
    if (x < -A) x = -A;
    t = ((int64_t)x * x / A) * x / A / 3;
    return (int32_t)(x - t);
}

static void v3_derive(v3_t *m)
{
    const int16_t *p = m->p;
    int d = (p[V3_TUNE] - 500) * 2, ad = d < 0 ? -d : d;
    int32_t sh = (int32_t)(((int64_t)ad * ad * 2505) >> 16);
    m->oct16 = d < 0 ? -sh : sh;
    m->o2s16 = (int32_t)(((int64_t)(p[V3_O2T] - 500) * 131072) / 1000);        /* +-12 semitones */
    m->inc3 = exp2_fx(999424 + p[V3_O3R] * 697, 0);                            /* 0.05 Hz .. 80 Hz, per 8-sample chunk */
    m->m1 = p[V3_M1] * 32768 / 1000;
    m->m2 = p[V3_M2] * 32768 / 1000;
    m->mn = p[V3_MN] * 32768 / 1000;
    m->mr = p[V3_MR] * 32768 / 1000;
    m->cut16 = (p[V3_CUT] * 602931) / 1000;
    m->k16 = (int32_t)(((int64_t)p[V3_RES] * 314573) / 1000);                  /* up to 4.8: past 4 the ladder oscillates on its own */
    m->key = p[V3_KEY];
    m->o3p = p[V3_O3P];
    m->o3f = p[V3_O3F];
    m->xmd = p[V3_XMD] * 131072 / 1000;                                         /* up to 2x the frequency of O2 */
    m->enf = p[V3_ENF] * 5 * 65536 / 1000;
    m->fbk = p[V3_FBK] * 65536 / 1000;
    m->enp = p[V3_ENP] * 2 * 65536 / 1000;
    m->lev = p[V3_LVL] * 32768 / 1000;
    m->ai = (int32_t)exp2_fx(C_L2_AI0 - (p[V3_ATK] * C_L2_ATK) / 1000, 0);
    m->dc = c_1mexp(exp2_fx(C_L2_DC0 - (p[V3_DEC] * C_L2_DCR) / 1000, 0));
    m->rc = c_1mexp(exp2_fx(C_L2_DC0 - (p[V3_REL] * C_L2_DCR) / 1000, 0));
    m->sus = p[V3_SUS] * 1073742;
    m->dirty = 0;
}

void v3_set(int t, int id, int v)
{
    v3_t *m = &V3[t];
    if (id < 0 || id >= V3_COUNT) return;
    if (v < 0) v = 0;
    if (v > V3_MAX[id]) v = V3_MAX[id];
    m->p[id] = (int16_t)v;
    m->dirty = 1;
}
int v3_get(int t, int id) { return V3[t].p[id]; }

void v3_init(int t)
{
    v3_t *m = &V3[t];
    int i;
    for (i = 0; i < (int)sizeof(*m); i++) ((uint8_t *)m)[i] = 0;
    for (i = 0; i < V3_COUNT; i++) m->p[i] = V3_DEF[i];
    m->ph1 = 0x2468ACE1u * (uint32_t)(t + 1); m->ph2 = 0x13579BDFu * (uint32_t)(t + 3);
    m->note16 = 60 << 16;
    m->vel = 32767;
    v3_derive(m);
}

void v3_on(int t, int note, int vel15)
{
    v3_t *m = &V3[t];
    m->vel = vel15;
    m->note16 = note << 16;
    m->gate = 1;
    m->a.st = 1;
}
void v3_off(int t)
{
    v3_t *m = &V3[t];
    m->gate = 0;
    if (m->a.st) m->a.st = 4;
}
int v3_active(int t) { return V3[t].a.st != 0; }

/* the oscillator: morph 0..1000 = sine -> saw -> square, +-16k; d / rd the band-limiting step and its reciprocal (set once per chunk) */
static inline int32_t v3_osc(uint32_t ph, uint32_t d, uint32_t rd, int morph)
{
    int32_t sn = SIN_TAB[(ph >> 22) & 1023u] >> 1, sw, sq;
    if (morph <= 0) return sn;
    sw = (c_mosc(ph, d, rd, 1, 0) >> 1);
    if (morph < 500) return sn + (int32_t)(((int64_t)(sw - sn) * (morph * 65)) >> 15);                  /* 65 / 32768 ~ 2 / 1000 */
    sq = (c_mosc(ph, d, rd, 2, 0) >> 1);
    return sw + (int32_t)(((int64_t)(sq - sw) * ((morph - 500) * 65)) >> 15);
}

void v3_render(int t, int32_t *out, int n)
{
    v3_t *m = &V3[t];
    int i = 0, j;
    if (m->dirty) v3_derive(m);
    while (i < n) {
        int cnt = n - i < 8 ? n - i : 8;
        int32_t ef = m->a.e, xl, nn, pit, cm, x16, g, lg2;
        uint32_t inc1, inc2, rd1, rd2, old3;
        old3 = m->ph3;
        m->ph3 += m->inc3;
        switch (m->p[V3_O3S]) {                                                 /* O3: sine, triangle, square, sample & hold */
        case 0: m->lfo = SIN_TAB[(m->ph3 >> 22) & 1023u]; break;
        case 1: { int32_t u = (int32_t)(m->ph3 >> 16); m->lfo = (u < 32768 ? u : 65535 - u) * 2 - 32768; break; }
        case 2: m->lfo = m->ph3 < 0x80000000u ? 32767 : -32767; break;
        default: if (m->ph3 < old3) m->lfo = c_rand15(); break;
        }
        nn = (int32_t)(((int64_t)(m->note16 - (60 << 16)) * 21845) >> 18);        /* the note, in octaves, Q16 */
        pit = (int32_t)(((int64_t)m->lfo * m->o3p * 32768 / 1000) >> 15) + (int32_t)(((int64_t)m->enp * ef) >> 30);
        xl = C_L2_MINC + nn + m->oct16 + pit;
        lg2 = xl + m->o2s16;
        inc1 = exp2_fx(xl, 0);
        rd1 = exp2_fx((40 << 16) - xl, 0);
        inc2 = exp2_fx(lg2, 0);
        rd2 = exp2_fx((40 << 16) - lg2, 0);
        cm = m->cut16 + (int32_t)(((int64_t)nn * m->key) / 1000) + (int32_t)(((int64_t)m->lfo * m->o3f * 262144 / 1000) >> 15) + (int32_t)(((int64_t)m->enf * ef) >> 30);
        x16 = (int32_t)exp2_fx(C_L2_FC0 + cm, 16);
        if (x16 > 82354 || x16 < 0) x16 = 82354;
        g = (int32_t)c_expm((uint32_t)x16);
        if (g > 26000) g = 26000;                       /* a 4-pole with its one-sample feedback delay is no longer clean above ~ 0.4 */
        for (j = 0; j < cnt; j++, i++) {
            int32_t o1, o2, rg, s, u, e, o, i1, i2;
            i1 = (int32_t)inc1 + (int32_t)(((int64_t)(((int64_t)inc1 * m->y4) >> 15) * m->fbk) >> 15);     /* the filter output back into O1's frequency */
            m->ph1 += (uint32_t)i1;
            o1 = v3_osc(m->ph1, inc1, rd1, m->p[V3_O1W]);
            i2 = (int32_t)inc2 + (int32_t)(((int64_t)(((int64_t)inc2 * o1) >> 14) * m->xmd) >> 16);         /* O1 into O2's frequency: X-MOD */
            m->ph2 += (uint32_t)i2;
            o2 = v3_osc(m->ph2, inc2, rd2, m->p[V3_O2W]);
            rg = (int32_t)(((int64_t)o1 * o2) >> 14);                                                        /* the ring modulator */
            s = (int32_t)(((int64_t)o1 * m->m1 + (int64_t)o2 * m->m2 + (int64_t)(c_rand15() >> 1) * m->mn + (int64_t)rg * m->mr) >> 15);
            u = v3_sat(s + (c_rand15() >> 10) - (int32_t)(((int64_t)m->k16 * m->y4) >> 16));          /* a whisper of noise at the input starts the self-oscillation, as on the real thing */
            m->y1 += (int32_t)(((int64_t)(u - m->y1) * g) >> 16);
            m->y2 += (int32_t)(((int64_t)(m->y1 - m->y2) * g) >> 16);
            m->y3 += (int32_t)(((int64_t)(m->y2 - m->y3) * g) >> 16);
            m->y4 += (int32_t)(((int64_t)(m->y3 - m->y4) * g) >> 16);
            e = c_adsr(&m->a, m->ai, m->dc, m->sus, m->rc);
            o = (int32_t)(((int64_t)m->y4 * (e >> 15)) >> 15);
            o = (int32_t)(((int64_t)o * m->vel) >> 15);
            o = v3_sat((int32_t)((int64_t)o * 40000 >> 15));                        /* the gain first, then the clipper: never beyond +-32k */
            out[i] = (int32_t)((int64_t)o * m->lev >> 15);
        }
    }
}
