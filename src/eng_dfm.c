/* SPDX-License-Identifier: MPL-2.0 */
/* DFM, one voice per track: a Moog DFAM (Drummer From Another Mother) in the spirit of its circuit, with the same controls minus the patch bay and the
 * sequencer (a key plays the note the sequencer's pitch knob would, and triggers the voice).
 *  - two VCOs, each triangle or square, each with its frequency knob, its own pitch-envelope amount and a common VCO decay; VCO 1 can hard-sync VCO 2
 *    and frequency-modulate it (FM);
 *  - a mixer: VCO 1, VCO 2 and noise;
 *  - the filter: the Moog ladder (24 dB/oct low-pass) or a high-pass, with its resonance, a decay-only envelope and the amount that envelope opens it;
 *  - a decay-only amplifier (the DFAM is a drum voice: the note is a trigger, the key release changes nothing). */
#include "voices.h"                                    /* cdsp.c is already included by eng_moog.c */

typedef struct {
    int16_t p[DF_COUNT];
    uint8_t dirty;
    int16_t fmq, l1, l2, ln, vel;
    int32_t oct16, f1, f2, e1, e2, cut16, feg16, k16, olvl;
    int32_t fz[4], fbf, pe, note16;
    uint32_t pdc8, fdc8, adc, ph1, ph2;
    adsr_t a, f;
} df_t;

static df_t DF[4];
static int32_t df_ai;                                 /* the amplifier's attack, ~ 3 ms (no click); the same for every track */
static const int16_t DF_DEF[DF_COUNT] = { 500, 500, 520, 1, 1, 0, 0, 450, 350, 350, 800, 700, 0, 0, 450, 350, 500, 450, 500, 1000 };
static const int16_t DF_MAX[DF_COUNT] = { 1000, 1000, 1000, 1, 1, 1, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1, 1000, 1000, 1000, 1000, 1000, 1000 };

static void df_derive(df_t *m)
{
    const int16_t *p = m->p;
    int d = (p[DF_TUNE] - 500) * 2, ad = d < 0 ? -d : d;
    int32_t sh = (int32_t)(((int64_t)ad * ad * 2505) >> 16);
    m->oct16 = d < 0 ? -sh : sh;
    m->f1 = (int32_t)(((int64_t)(p[DF_V1F] - 500) * 131072 * 2) / 1000);          /* +-2 octaves around the key's pitch */
    m->f2 = (int32_t)(((int64_t)(p[DF_V2F] - 500) * 131072 * 2) / 1000);
    m->e1 = p[DF_V1E] * 4 * 65536 / 1000;                                         /* the pitch envelope: up to 4 octaves down to rest */
    m->e2 = p[DF_V2E] * 4 * 65536 / 1000;
    m->fmq = (int16_t)(p[DF_FM] * 16);                                                       /* VCO 2's increment moves by up to +-16 * VCO 1 / 1000 of itself... Q14 */
    m->l1 = (int16_t)(p[DF_V1L] * 32767 / 1000); m->l2 = (int16_t)(p[DF_V2L] * 32767 / 1000); m->ln = (int16_t)(p[DF_NSE] * 32767 / 1000);
    m->cut16 = (p[DF_CUT] * 576717) / 1000;
    m->feg16 = (p[DF_FEG] * 360448) / 1000;
    m->k16 = p[DF_RES] * 268;
    m->pdc8 = c_pow8(c_1mexp(exp2_fx(C_L2_DC0 - (p[DF_VDC] * C_L2_DCR) / 1000, 0)));
    m->fdc8 = c_pow8(c_1mexp(exp2_fx(C_L2_DC0 - (p[DF_FDC] * C_L2_DCR) / 1000, 0)));
    m->adc = c_1mexp(exp2_fx(C_L2_DC0 - (p[DF_ADC] * C_L2_DCR) / 1000, 0));
    df_ai = (int32_t)exp2_fx(C_L2_AI0 - (150 * C_L2_ATK) / 1000, 0);
    m->olvl = (int32_t)(((int64_t)p[DF_LVL] * 32768 / 1000 * 56000) >> 15);
    m->dirty = 0;
}

void df_set(int t, int id, int v)
{
    df_t *m = &DF[t];
    if (id < 0 || id >= DF_COUNT) return;
    if (v < 0) v = 0;
    if (v > DF_MAX[id]) v = DF_MAX[id];
    m->p[id] = (int16_t)v;
    m->dirty = 1;
}
int df_get(int t, int id) { return DF[t].p[id]; }

void df_init(int t)
{
    df_t *m = &DF[t];
    int i;
    for (i = 0; i < (int)sizeof(*m); i++) ((uint8_t *)m)[i] = 0;
    for (i = 0; i < DF_COUNT; i++) m->p[i] = DF_DEF[i];
    m->note16 = 60 << 16;
    df_derive(m);
}

void df_on(int t, int note, int vel15)               /* a trigger: every envelope restarts (the amplitude from where it is: no click) */
{
    df_t *m = &DF[t];
    m->note16 = note << 16;
    m->vel = (int16_t)vel15;
    m->pe = 1 << 30;
    m->a.st = 1;
    m->f.st = 1; m->f.e = 1 << 30;
}
void df_off(int t) { (void)t; }                       /* the DFAM has no gate: the decays run to the end */
int df_active(int t) { return DF[t].a.st != 0; }

void df_render(int t, int32_t *out, int n)
{
    df_t *m = &DF[t];
    int i = 0, j;
    if (m->dirty) df_derive(m);
    while (i < n) {
        int cnt = n - i < 8 ? n - i : 8;
        int32_t base, pe = m->pe, x1, x2, lx0, x16, g, ef = m->f.e, mode = m->p[DF_MODE], w1 = m->p[DF_V1W], w2 = m->p[DF_V2W];
        uint32_t inc1, inc2, rd1, rd2, ratio = 0;
        m->pe -= (int32_t)(((int64_t)pe * m->pdc8) >> 32);
        m->f.e -= (int32_t)(((int64_t)ef * m->fdc8) >> 32);
        base = (int32_t)(((int64_t)(m->note16 - (60 << 16)) * 21845) >> 18) + m->oct16;
        x1 = C_L2_MINC + base + m->f1 + (int32_t)(((int64_t)m->e1 * pe) >> 30);
        x2 = C_L2_MINC + base + m->f2 + (int32_t)(((int64_t)m->e2 * pe) >> 30);
        inc1 = exp2_fx(x1, 0); rd1 = c_rd(inc1);
        inc2 = exp2_fx(x2, 0); rd2 = c_rd(inc2);
        if (m->p[DF_SYN]) ratio = exp2_fx(x2 - x1 + (16 << 16), 0);                 /* VCO 2's phase when VCO 1 restarts: Q16 ratio of the two */
        lx0 = C_L2_FC0 + 92735 + m->cut16 + (int32_t)(((int64_t)m->feg16 * ef) >> 30);
        x16 = (int32_t)exp2_fx(lx0, 16);
        if (x16 > 164707 || x16 < 0) x16 = 164707;
        g = (int32_t)c_expm((uint32_t)x16);
        for (j = 0; j < cnt; j++, i++) {
            int32_t o1, o2, s, x, ea, o, k = m->k16, im;
            uint32_t old = m->ph1, i2 = inc2;
            m->ph1 += inc1;
            o1 = c_mosc(m->ph1, inc1, rd1, w1 ? 2 : 0, 0);
            if (m->fmq) {                                                           /* VCO 1 bends VCO 2's pitch (linear FM, one sample behind) */
                im = (int32_t)(((int64_t)(((int64_t)inc2 * o1) >> 15) * m->fmq) >> 8);
                if (im < -(int32_t)(inc2 >> 1)) im = -(int32_t)(inc2 >> 1);
                i2 = (uint32_t)((int32_t)inc2 + im);
            }
            if (ratio && m->ph1 < old) m->ph2 = (uint32_t)(((uint64_t)m->ph1 * ratio) >> 16);
            else m->ph2 += i2;
            o2 = c_mosc(m->ph2, inc2, rd2, w2 ? 2 : 0, 0);
            s = (int32_t)(((int64_t)o1 * m->l1 + (int64_t)o2 * m->l2 + (int64_t)(c_rand15() >> 1) * m->ln) >> 15);
            s = (int32_t)(((int64_t)s * 14746) >> 15);                               /* * .45 */
            s += c_rand15() >> 11;
            m->fbf += (m->fz[3] - m->fbf) >> 1;
            x = c_tanh((int32_t)(((int64_t)s * 72090) >> 16) - (int32_t)(((int64_t)k * (m->fbf >> 5)) >> 16));
            m->fz[0] += (int32_t)(((int64_t)(x - (m->fz[0] >> 5)) * g) >> 11);
            m->fz[1] += (int32_t)(((int64_t)((m->fz[0] - m->fz[1]) >> 5) * g) >> 11);
            m->fz[2] += (int32_t)(((int64_t)((m->fz[1] - m->fz[2]) >> 5) * g) >> 11);
            m->fz[3] += (int32_t)(((int64_t)((m->fz[2] - m->fz[3]) >> 5) * g) >> 11);
            o = m->fz[3] >> 5;
            if (mode) o = x - o;                                                    /* high-pass: what the four poles took out */
            o = (int32_t)(((int64_t)o * (65536 + (k >> 1))) >> 16);
            ea = c_adsr(&m->a, df_ai, m->adc, 0, m->adc);
            if (m->a.st == 3) { m->a.st = 0; m->a.e = 0; }                           /* decayed to nothing: the voice is free */
            o = (int32_t)(((int64_t)o * (int32_t)(((int64_t)(ea >> 15) * (((int64_t)m->vel * m->olvl) >> 15)) >> 15)) >> 15);
            out[i] = c_tanh(o);
        }
    }
}
