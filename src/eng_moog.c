/* SPDX-License-Identifier: MPL-2.0 */
/* The prototype's Minimoog engine in fixed point: three band-limited oscillators (triangle / saw / square / pulse,
 * 32' .. 2' with tune), a noise source (white / pink / brown / blue / crackle with tone and crush), a four-pole
 * transistor ladder with a tanh at every stage (two sub-steps per sample), key tracking and a contour envelope,
 * an ADSR for the amplifier, glide, and the decay doubling as release. Q15 audio, Q30 envelopes, Q16 pitches. */
#include "voices.h"
#include "cdsp.c"

typedef struct {
    int16_t p[MG_COUNT];
    uint8_t dirty;
    /* derived (moog_derive) */
    uint8_t ty[3];
    int32_t oct16[3], lev[3], nlev;
    uint32_t pw[3];
    noisec_t nc;
    uint32_t gc;                                   /* glide coefficient Q32; 0xFFFFFFFF = off */
    int32_t ai, fai, sus, fsus, cut16, env5, k16, gain16, lvl;
    int32_t cut_s, oct_s[3];                       /* the values the previous block ended on: the pitch / cutoff ramp across the block instead of stepping */
    uint8_t si;
    uint32_t dc, rc, fdc, frc;
    /* voice */
    uint32_t ph[3];
    int32_t note16, gn16, drift, dcs, y[4], ty4[4];
    adsr_t a, f;
    noise_t n;
    uint8_t gate;
    int32_t vel;
} moog_t;

static moog_t MG[4];
static const int16_t MG_DEF[MG_COUNT] = {
    1, 2, 500, 500, 800,   1, 2, 550, 500, 700,   1, 1, 460, 500, 500,
    0, 500, 0, 300, 550, 300, 500, 0, 1, 210, 640, 700, 210, 640, 300, 1000 };
static const int16_t MG_MAX[MG_COUNT] = {
    3, 4, 1000, 1000, 1000,   3, 4, 1000, 1000, 1000,   3, 4, 1000, 1000, 1000,
    4, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1, 1000, 1000, 1000, 1000, 1000, 1000, 1000 };

static int32_t log_time(int v, int32_t l0, int32_t range) { return l0 - (v * range) / 1000; }

static void moog_derive(moog_t *m)
{
    int k;
    const int16_t *p = m->p;
    for (k = 0; k < 3; k++) {
        int d = (p[MG_T1 + 5 * k] - 500) * 2, ad = d < 0 ? -d : d;
        int32_t sh = (int32_t)(((int64_t)ad * ad * 2505) >> 16);        /* d^2 * 7/12, Q16 octaves */
        m->ty[k] = (uint8_t)p[MG_W1 + 5 * k];
        m->oct16[k] = (p[MG_R1 + 5 * k] - 2) * 65536 + (d < 0 ? -sh : sh);
        m->pw[k] = 0x80000000u - (uint32_t)p[MG_P1 + 5 * k] * 1932735u;
        m->lev[k] = p[MG_L1 + 5 * k] * 32768 / 1000;
    }
    m->nlev = p[MG_NLEV] * 32768 / 1000;
    noise_coef(&m->nc, p[MG_NTYPE], p[MG_NTONE], p[MG_NCRUSH]);
    if (p[MG_GLIDE] < 10)
        m->gc = 0xFFFFFFFFu;
    else {
        int32_t g16 = p[MG_GLIDE] * 65536 / 1000;
        int32_t tau16 = 197 + (int32_t)(((int64_t)g16 * g16 >> 16) * 78643 >> 16);   /* .003 + g^2 * 1.2 */
        m->gc = c_1mexp((1595670528u) / (uint32_t)(tau16 >> 2));
    }
    m->ai = (int32_t)exp2_fx(log_time(p[MG_ATK], C_L2_AI0, C_L2_ATK), 0);
    m->fai = (int32_t)exp2_fx(log_time(p[MG_FATK], C_L2_AI0, C_L2_ATK), 0);
    m->dc = c_1mexp(exp2_fx(log_time(p[MG_DEC], C_L2_DC0, C_L2_DCR), 0));
    m->fdc = c_1mexp(exp2_fx(log_time(p[MG_FDEC], C_L2_DC0, C_L2_DCR), 0));
    m->rc = p[MG_REL] ? m->dc : c_1mexp(exp2_fx(C_L2_DC8, 0));
    m->frc = p[MG_REL] ? m->fdc : m->rc;
    m->sus = p[MG_SUS] * 1073742;
    m->fsus = p[MG_FSUS] * 1073742;
    m->cut16 = (p[MG_CUT] * 602931) / 1000;                              /* cut * 9.2 */
    m->env5 = p[MG_ENV] * 5 * 65536 / 1000;
    m->k16 = p[MG_RES] * 267;                                           /* 4 * res * 1.02 */
    m->gain16 = (int32_t)(((int64_t)104858 * (65536 + (((int64_t)m->k16 * 22938) >> 16))) >> 16);   /* 1.6 * (1 + k * .35) */
    m->lvl = p[MG_LVL] * 32768 / 1000;
    m->dirty = 0;
}

void moog_set(int t, int id, int v)
{
    moog_t *m = &MG[t];
    if (id < 0 || id >= MG_COUNT) return;
    if (v < 0) v = 0;
    if (v > MG_MAX[id]) v = MG_MAX[id];
    m->p[id] = (int16_t)v;
    m->dirty = 1;
}
int moog_get(int t, int id) { return MG[t].p[id]; }

void moog_init(int t)
{
    moog_t *m = &MG[t];
    int i;
    for (i = 0; i < (int)sizeof(*m); i++) ((uint8_t *)m)[i] = 0;
    for (i = 0; i < MG_COUNT; i++) m->p[i] = MG_DEF[i];
    m->gn16 = m->note16 = 60 << 16;
    m->vel = 32767;
    moog_derive(m);
}

void moog_on(int t, int note, int vel15)
{
    moog_t *m = &MG[t];
    m->vel = vel15;
    if (!m->gate && m->a.e < (1 << 30) / 100) m->gn16 = note << 16;
    m->note16 = note << 16;
    m->gate = 1;
    m->a.st = 1;
    m->f.st = 1;
}
void moog_off(int t)
{
    moog_t *m = &MG[t];
    m->gate = 0;
    if (m->a.st) m->a.st = 4;
    if (m->f.st) m->f.st = 4;
}
int moog_active(int t) { return MG[t].a.st != 0; }

void moog_render(int t, int32_t *out, int n)
{
    moog_t *m = &MG[t];
    int i = 0, k, j;
    int32_t cut0, oct0[3], rq = n > 0 ? 65536 / n : 0;                /* 1 / n, Q16: the ramp position of sample i is i * rq */
    if (m->dirty) moog_derive(m);
    if (!m->si) { m->si = 1; m->cut_s = m->cut16; for (k = 0; k < 3; k++) m->oct_s[k] = m->oct16[k]; }
    cut0 = m->cut_s; for (k = 0; k < 3; k++) oct0[k] = m->oct_s[k];
    m->drift = (int32_t)(((int64_t)(m->drift + (c_rand15() >> 11)) * 65503) >> 16);       /* the slow drift, +-.0002 st */
    while (i < n) {
        uint32_t inc[3], rd[3], g;
        int32_t rel = m->gn16 + m->drift - (60 << 16), x16, ef = m->f.e;
        int cnt = n - i < 8 ? n - i : 8;
        int32_t cutc = cut0 + (int32_t)(((int64_t)(m->cut16 - cut0) * ((i + cnt) * rq)) >> 16);
        for (k = 0; k < 3; k++) {
            int32_t lx = C_L2_MINC + (int32_t)(((int64_t)rel * 21845) >> 18) + oct0[k] + (int32_t)(((int64_t)(m->oct16[k] - oct0[k]) * ((i + cnt) * rq)) >> 16);
            inc[k] = exp2_fx(lx, 0);
            rd[k] = c_rd(inc[k]);
        }
        x16 = (int32_t)exp2_fx(C_L2_FC0 + cutc + (int32_t)(((int64_t)(m->gn16 - (60 << 16)) * 3604) >> 16) +
                               (int32_t)(((int64_t)m->env5 * ef) >> 30), 16);
        if (x16 > 82354 || x16 < 0) x16 = 82354;                         /* fc <= .4 FS */
        g = c_expm((uint32_t)x16);
        for (j = 0; j < cnt; j++, i++) {
            int32_t sum, sg, xin, e, ef2, o, y3;
            int s2;
            if (m->gc == 0xFFFFFFFFu) m->gn16 = m->note16;
            else m->gn16 += (int32_t)(((int64_t)(m->note16 - m->gn16) * m->gc) >> 32);
            for (k = 0; k < 3; k++) m->ph[k] += inc[k];
            sum = (int32_t)(((int64_t)noise_step(&m->n, &m->nc) * m->nlev) >> 15);
            for (k = 0; k < 3; k++)
                sum += (int32_t)(((int64_t)c_mosc(m->ph[k], inc[k], rd[k], m->ty[k], m->pw[k]) * m->lev[k]) >> 15);
            sg = (int32_t)(((int64_t)sum * 11469) >> 15);                /* * .35 */
            e = c_adsr(&m->a, m->ai, m->dc, m->sus, m->rc);
            ef2 = c_adsr(&m->f, m->fai, m->fdc, m->fsus, m->frc);
            (void)ef2;
            xin = (int32_t)(((int64_t)sg * m->gain16) >> 16);
            for (s2 = 0; s2 < 2; s2++) {
                int32_t x = c_tanh(xin - (int32_t)(((int64_t)m->k16 * (m->y[3] >> 5)) >> 16));
                m->y[0] += (int32_t)(((int64_t)(x - m->ty4[0]) * g) >> 11);
                m->ty4[0] = c_tanh(m->y[0] >> 5);
                m->y[1] += (int32_t)(((int64_t)(m->ty4[0] - m->ty4[1]) * g) >> 11);
                m->ty4[1] = c_tanh(m->y[1] >> 5);
                m->y[2] += (int32_t)(((int64_t)(m->ty4[1] - m->ty4[2]) * g) >> 11);
                m->ty4[2] = c_tanh(m->y[2] >> 5);
                m->y[3] += (int32_t)(((int64_t)(m->ty4[2] - m->ty4[3]) * g) >> 11);
                m->ty4[3] = c_tanh(m->y[3] >> 5);
            }
            y3 = m->y[3] >> 5;
            o = (int32_t)(((int64_t)y3 * (e >> 15)) >> 15);              /* * envelope */
            o = (int32_t)(((int64_t)o * m->vel) >> 15);                  /* * velocity */
            m->dcs += (int32_t)(((int64_t)((o << 8) - m->dcs) * 4294967) >> 32);   /* .001 */
            o -= m->dcs >> 8;
            out[i] = (int32_t)(((int64_t)o * 39322 >> 15) * m->lvl >> 15);       /* * 2 * .6 * lvl */
        }
    }
    m->cut_s = m->cut16; for (k = 0; k < 3; k++) m->oct_s[k] = m->oct16[k];
}
