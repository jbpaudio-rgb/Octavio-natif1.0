/* SPDX-License-Identifier: MPL-2.0 */
/* The prototype's Prophet engine in fixed point: oscillator A (saw / triangle / pulse mix, sync) and B (tuned and
 * detuned against A), sub oscillator, noise, two ADSRs (amplifier / filter), a soft four-pole low-pass with feedback
 * (resonance) and poly-mod (osc B into the cutoff). Q15 audio, Q30 envelopes, Q16 pitches. */
#include "voices.h"

typedef struct {
    int16_t p[PR_COUNT];
    uint8_t dirty, sync, subty;
    int32_t wa[3], wb[3];                          /* saw / tri / pulse weights, Q15 */
    uint32_t pwa, pwb, pws;
    int32_t tuna, tunb, rnga, rngb, subo, lva, lvb, lvs, lvn;   /* pitch offsets: Q16 semitones (tune) / Q16 octaves */
    noisec_t nc;
    int32_t ai, fai, sus, fsus, cut16, env16, pm16, k16, lvl;
    int32_t cut_s, tuna_s, tunb_s;                 /* the values the previous block ended on: tune / cutoff ramp across the block instead of stepping */
    uint8_t si;
    uint32_t dc, rc, fdc, frc;
    /* voice */
    uint32_t pa, pb, so;
    int32_t note, drift, fbf, z[4], dcs, vel;
    adsr_t a, f;
    noise_t n;
    uint8_t gate;
} proph_t;

static proph_t PR[4];
static const int16_t PR_DEF[PR_COUNT] = { 833, 2, 500, 200, 0, 2, 540, 200, 900, 900, 0, 0, 500, 0, 0,
                                          0, 500, 0, 0, 500, 250, 500, 0, 210, 640, 700, 640, 210, 640, 300, 640, 1000 };
static const int16_t PR_MAX[PR_COUNT] = { 1000, 4, 1000, 1000, 1000, 4, 1000, 1000, 1000, 1000, 2, 1, 1000, 1000, 1000,
                                          4, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000 };
static const int32_t PST[7][3] = {{32768, 0, 0}, {22938, 22938, 0}, {0, 32768, 0}, {0, 22938, 22938}, {0, 0, 32768},
                                  {22938, 0, 22938}, {32768, 0, 0}};

static void weights(int v, int32_t *w, int *sync)                 /* pws(): v in thousandths of 0..1 */
{
    int x = v * 6, i = x / 1000, f;
    if (i > 5) i = 5;
    f = ((x - i * 1000) * 65536) / 1000;
    if (v >= 1000) { i = 5; f = 65536; }
    for (int k = 0; k < 3; k++) w[k] = PST[i][k] + (((PST[i + 1][k] - PST[i][k]) * f) >> 16);
    if (sync) *sync = (i == 5 && f > 32768) || 0;                 /* the 'Sync' weight [3] > .5 */
}
static int32_t tune_st(int v)                                    /* sign(d) d^2 * 12 semitones, Q16 */
{
    int d = (v - 500) * 2, ad = d < 0 ? -d : d;
    int32_t s = (int32_t)(((int64_t)ad * ad * 51539) >> 16);
    return d < 0 ? -s : s;
}

static void proph_derive(proph_t *m)
{
    const int16_t *p = m->p;
    int sy = 0, ts = (p[PR_ST] - 500) * 2, ats = ts < 0 ? -ts : ts;
    int32_t so;
    weights(p[PR_WAVE], m->wa, &sy);
    m->sync = (uint8_t)sy;
    weights(p[PR_BWAVE] * 5 / 6, m->wb, 0);
    m->pwa = 0x80000000u - (uint32_t)p[PR_PW] * 1932735u;
    m->pwb = 0x80000000u - (uint32_t)p[PR_BPW] * 1932735u;
    m->pws = 0x80000000u - (uint32_t)p[PR_SP] * 1932735u;
    m->tuna = tune_st(p[PR_TUNE]);
    m->tunb = tune_st(p[PR_BTUNE]);
    m->rnga = (p[PR_RNG] - 2) * 65536;
    m->rngb = (p[PR_BRNG] - 2) * 65536;
    so = (int32_t)(((int64_t)ats * ats * 2505) >> 16);
    m->subo = (-(p[PR_SO]) - 1) * 65536 + (ts < 0 ? -so : so);
    m->subty = (uint8_t)p[PR_SW];
    m->lva = p[PR_LA] * 32768 / 1000;
    m->lvb = p[PR_LB] * 32768 / 1000;
    m->lvs = p[PR_SL] * 32768 / 1000;
    m->lvn = p[PR_NLEV] * 32768 / 1000;
    noise_coef(&m->nc, p[PR_NTYPE], p[PR_NTONE], p[PR_NCRUSH]);
    m->ai = (int32_t)exp2_fx(C_L2_AI0 - (p[PR_ATK] * C_L2_ATK) / 1000, 0);
    m->fai = (int32_t)exp2_fx(C_L2_AI0 - (p[PR_FATK] * C_L2_ATK) / 1000, 0);
    m->dc = c_1mexp(exp2_fx(C_L2_DC0 - (p[PR_DEC] * C_L2_DCR) / 1000, 0));
    m->rc = c_1mexp(exp2_fx(C_L2_DC0 - (p[PR_REL] * C_L2_DCR) / 1000, 0));
    m->fdc = c_1mexp(exp2_fx(C_L2_DC0 - (p[PR_FDEC] * C_L2_DCR) / 1000, 0));
    m->frc = c_1mexp(exp2_fx(C_L2_DC0 - (p[PR_FREL] * C_L2_DCR) / 1000, 0));
    m->sus = p[PR_SUS] * 1073742;
    m->fsus = p[PR_FSUS] * 1073742;
    m->cut16 = (p[PR_CUT] * 576717) / 1000;                       /* cut * 8.8 */
    m->env16 = (p[PR_ENV] * 360448) / 1000;                       /* env * 5.5 */
    m->pm16 = (p[PR_PM] * 196608) / 1000;                         /* pm * 3 */
    m->k16 = p[PR_RES] * 257;                                     /* 4 * res * .98 */
    m->lvl = p[PR_LVL] * 32768 / 1000;
    m->dirty = 0;
}

void proph_set(int t, int id, int v)
{
    proph_t *m = &PR[t];
    if (id < 0 || id >= PR_COUNT) return;
    v = v < 0 ? 0 : v > PR_MAX[id] ? PR_MAX[id] : v;
    m->p[id] = (int16_t)v;
    m->dirty = 1;
}
int proph_get(int t, int id) { return PR[t].p[id]; }

void proph_init(int t)
{
    proph_t *m = &PR[t];
    int i;
    for (i = 0; i < (int)sizeof(*m); i++) ((uint8_t *)m)[i] = 0;
    for (i = 0; i < PR_COUNT; i++) m->p[i] = PR_DEF[i];
    m->note = 60;
    m->vel = 32767;
    proph_derive(m);
}
void proph_on(int t, int note, int vel15)
{
    proph_t *m = &PR[t];
    m->note = note;
    m->vel = vel15;
    m->gate = 1;
    m->a.st = 1;
    m->f.st = 1;
}
void proph_off(int t)
{
    proph_t *m = &PR[t];
    m->gate = 0;
    if (m->a.st) m->a.st = 4;
    if (m->f.st) m->f.st = 4;
}
int proph_active(int t) { return PR[t].a.st != 0; }

void proph_render(int t, int32_t *out, int n)
{
    proph_t *m = &PR[t];
    int i = 0, j;
    int32_t cut0, ta0, tb0, rq = n > 0 ? 65536 / n : 0;                /* 1 / n, Q16: the ramp position of sample i is i * rq */
    if (m->dirty) proph_derive(m);
    if (!m->si) { m->si = 1; m->cut_s = m->cut16; m->tuna_s = m->tuna; m->tunb_s = m->tunb; }
    cut0 = m->cut_s; ta0 = m->tuna_s; tb0 = m->tunb_s;
    m->drift = (int32_t)(((int64_t)(m->drift + (c_rand15() >> 11)) * 65503) >> 16);
    while (i < n) {
        int32_t base = (m->note - 60) << 16, xa, xb, xs, ef = m->f.e, lx0;
        uint32_t incA, incB, incS, rdA, rdB, rdS, g0;
        int cnt = n - i < 8 ? n - i : 8;
        int32_t cutc = cut0 + (int32_t)(((int64_t)(m->cut16 - cut0) * ((i + cnt) * rq)) >> 16);
        int32_t tua = ta0 + (int32_t)(((int64_t)(m->tuna - ta0) * ((i + cnt) * rq)) >> 16), tub = tb0 + (int32_t)(((int64_t)(m->tunb - tb0) * ((i + cnt) * rq)) >> 16);
        xa = C_L2_MINC + (int32_t)(((int64_t)(base + m->drift + tua) * 21845) >> 18) + m->rnga;
        xb = C_L2_MINC + (int32_t)(((int64_t)(base - m->drift + tub) * 21845) >> 18) + m->rngb;
        xs = C_L2_MINC + (int32_t)(((int64_t)base * 21845) >> 18) + m->subo;
        incA = exp2_fx(xa, 0); incB = exp2_fx(xb, 0); incS = exp2_fx(xs, 0);
        rdA = c_rd(incA); rdB = c_rd(incB); rdS = c_rd(incS);
        lx0 = C_L2_FC0 + 92735 /* log2(80/30): 2pi * 40 / FS */ + cutc + (int32_t)(((int64_t)base * 4369) >> 16) +
              (int32_t)(((int64_t)m->env16 * ef) >> 30);               /* 2pi fc / FS with fc = 40 * ... */
        g0 = 0;
        if (!m->p[PR_PM]) {
            int32_t x16 = (int32_t)exp2_fx(lx0, 16);
            if (x16 > 164707 || x16 < 0) x16 = 164707;                 /* fc <= .4 FS : 2 pi * .4 */
            g0 = c_expm((uint32_t)x16);
        }
        for (j = 0; j < cnt; j++, i++) {
            int32_t a, b, sub, nz, sg, ea, x, o, g, k = m->k16;
            m->pb += incB;
            if (m->pb < incB && m->sync) m->pa = 0;
            m->pa += incA;
            m->so += incS;
            b = (int32_t)(((int64_t)c_mosc(m->pb, incB, rdB, 1, 0) * m->wb[0] + (int64_t)c_mosc(m->pb, incB, rdB, 0, 0) * m->wb[1] +
                           (int64_t)c_mosc(m->pb, incB, rdB, 3, m->pwb) * m->wb[2]) >> 15);
            a = (int32_t)(((int64_t)c_mosc(m->pa, incA, rdA, 1, 0) * m->wa[0] + (int64_t)c_mosc(m->pa, incA, rdA, 0, 0) * m->wa[1] +
                           (int64_t)c_mosc(m->pa, incA, rdA, 3, m->pwa) * m->wa[2]) >> 15);
            sub = m->subty == 0 ? sin_q15(m->so) : c_mosc(m->so, incS, rdS, m->subty == 1 ? 0 : 3, m->pws);
            nz = noise_step(&m->n, &m->nc);
            sg = (int32_t)(((int64_t)a * m->lva + (int64_t)b * m->lvb + (int64_t)sub * m->lvs + (int64_t)nz * m->lvn) >> 15);
            sg = (int32_t)(((int64_t)sg * 14746) >> 15);                 /* * .45 */
            ea = c_adsr(&m->a, m->ai, m->dc, m->sus, m->rc);
            c_adsr(&m->f, m->fai, m->fdc, m->fsus, m->frc);
            if (m->p[PR_PM]) {
                int32_t x16 = (int32_t)exp2_fx(lx0 + (int32_t)(((int64_t)m->pm16 * b) >> 15), 16);
                if (x16 > 164707 || x16 < 0) x16 = 164707;
                g = (int32_t)c_expm((uint32_t)x16);
            } else
                g = (int32_t)g0;
            m->fbf += (m->z[3] - m->fbf) >> 1;
            x = c_tanh((int32_t)(((int64_t)sg * 72090) >> 16) - (int32_t)(((int64_t)k * (m->fbf >> 5)) >> 16));   /* sg * 1.1 - k fbf */
            m->z[0] += (int32_t)(((int64_t)(x - (m->z[0] >> 5)) * g) >> 11);
            m->z[1] += (int32_t)(((int64_t)((m->z[0] - m->z[1]) >> 5) * g) >> 11);
            m->z[2] += (int32_t)(((int64_t)((m->z[1] - m->z[2]) >> 5) * g) >> 11);
            m->z[3] += (int32_t)(((int64_t)((m->z[2] - m->z[3]) >> 5) * g) >> 11);
            o = (int32_t)(((int64_t)(m->z[3] >> 5) * (65536 + (k >> 1))) >> 16);      /* * (1 + k * .5) */
            o = (int32_t)(((int64_t)o * (ea >> 15)) >> 15);
            o = (int32_t)(((int64_t)o * m->vel) >> 15);
            m->dcs += (int32_t)(((int64_t)((o << 8) - m->dcs) * 4294967) >> 32);
            o -= m->dcs >> 8;
            out[i] = (int32_t)(((int64_t)o * 35389 >> 15) * m->lvl >> 15);             /* * 1.8 * .6 = 1.08 */
        }
    }
    m->cut_s = m->cut16; m->tuna_s = m->tuna; m->tunb_s = m->tunb;
}
