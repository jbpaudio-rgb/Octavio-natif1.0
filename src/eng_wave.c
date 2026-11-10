/* SPDX-License-Identifier: MPL-2.0 */
/* PROTOTYPE (size / load measurement, not wired to the UI): a monophonic wavetable engine.
 * 16 single-cycle waves of 256 samples are computed once, in RAM, from harmonic recipes (no table in flash): waves 0..7
 * morph from a soft sine towards a saw, 8..11 are the odd-harmonic family (towards a square), 12..15 put a formant peak
 * on the spectrum. WAVE (position) sweeps the table with linear interpolation between two neighbouring waves and
 * between samples. Two oscillators (the second detuned by DETUNE), a sine sub-oscillator, a two-pole low-pass that
 * tracks the key and follows a filter envelope, and an ADSR. Same parameter / voice conventions as eng_moog.c. */
#include "voices.h"                                    /* cdsp.c is already included by eng_moog.c */

#define WT_N 16
#define WT_L 256
enum { WV_POS, WV_PENV, WV_RNG, WV_TUNE, WV_DET, WV_SUB, WV_CUT, WV_ENV, WV_ATK, WV_DEC, WV_SUS, WV_REL,
       WV_FATK, WV_FDEC, WV_FSUS, WV_LVL, WV_COUNT };

typedef struct {
    int16_t p[WV_COUNT];
    uint8_t dirty, si;
    int32_t oct16, lev, cut16, env5, pos16, penv16, subl, det;
    int32_t ai, fai, sus, fsus;
    uint32_t dc, rc, fdc, frc;
    int32_t cut_s;
    uint32_t ph, ph2, phs;
    int32_t note16, y1, y2, dcs;
    adsr_t a, f;
    uint8_t gate;
    int32_t vel;
} wave_t;

static wave_t WV[4];
static int16_t WT[WT_N][WT_L + 1];                     /* 8.5 KB of RAM, built on the first use; [WT_L] = [0] so the interpolation needs no wrap */
static uint8_t wt_built;
static const int16_t WV_DEF[WV_COUNT] = { 300, 400, 2, 500, 250, 200, 650, 350, 10, 450, 650, 300, 10, 400, 300, 1000 };
static const int16_t WV_MAX[WV_COUNT] = { 1000, 1000, 4, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000 };

static int32_t wsin(uint32_t turns256)                  /* sine of (turns256 / 256) of a turn, Q15 */
{
    return SIN_TAB[(turns256 & 255u) * 4u];
}

static void wt_build(void)
{
    int w, i, h;
    for (w = 0; w < WT_N; w++) {
        int32_t amp[32], peak = 1;
        for (h = 1; h < 32; h++) {
            int32_t a;
            if (w < 8)      a = 4096 / h + ((int32_t)(8 - w) * (4096 / (h * h)) - (int32_t)(8 - w) * (4096 / h)) / 8;   /* 1/h^2 (w = 0) .. 1/h (w = 7) */
            else if (w < 12) a = (h & 1) ? (4096 / h) * (w - 7) / 4 + (4096 / (h * h)) * (11 - w) / 4 : 0;               /* odd harmonics */
            else { int d = h - (3 + (w - 12) * 3); a = (d < 0 ? -d : d) < 2 ? 4096 / (1 + (d < 0 ? -d : d)) : 4096 / (h * h); }   /* a formant at harmonic 3, 6, 9, 12 */
            amp[h] = a;
        }
        amp[0] = 0;
        for (i = 0; i < WT_L; i++) {
            int32_t s = 0, a;
            for (h = 1; h < 32; h++) s += (int32_t)(((int64_t)amp[h] * wsin((uint32_t)(h * i))) >> 12);
            WT[w][i] = (int16_t)s;
            a = s < 0 ? -s : s;
            if (a > peak) peak = a;
        }
        for (i = 0; i < WT_L; i++) WT[w][i] = (int16_t)(((int32_t)WT[w][i] * 28000) / peak);
        WT[w][WT_L] = WT[w][0];
    }
    wt_built = 1;
}

static inline int32_t wt_read(uint32_t ph, int32_t pos16)      /* ph: Q32 turn; pos16: wave position in Q16 (0 .. 15) */
{
    int w = pos16 >> 16, wf = (pos16 >> 8) & 255, i = (int)(ph >> 24), f = (int)((ph >> 16) & 255);
    const int16_t *a = WT[w], *b = WT[w < WT_N - 1 ? w + 1 : w];
    int32_t va = a[i] + (((int32_t)a[i + 1] - a[i]) * f >> 8), vb = b[i] + (((int32_t)b[i + 1] - b[i]) * f >> 8);
    return va + (((vb - va) * wf) >> 8);
}

static void wave_derive(wave_t *m)
{
    const int16_t *p = m->p;
    int d = (p[WV_TUNE] - 500) * 2, ad = d < 0 ? -d : d;
    int32_t sh = (int32_t)(((int64_t)ad * ad * 2505) >> 16);
    m->oct16 = (p[WV_RNG] - 2) * 65536 + (d < 0 ? -sh : sh);
    m->pos16 = p[WV_POS] * 15 * 65536 / 1000;
    m->penv16 = p[WV_PENV] * 15 * 65536 / 1000;
    m->det = p[WV_DET];
    m->subl = p[WV_SUB] * 32768 / 1000;
    m->cut16 = (p[WV_CUT] * 602931) / 1000;
    m->env5 = p[WV_ENV] * 5 * 65536 / 1000;
    m->lev = p[WV_LVL] * 32768 / 1000;
    m->ai = (int32_t)exp2_fx(C_L2_AI0 - (p[WV_ATK] * C_L2_ATK) / 1000, 0);
    m->fai = (int32_t)exp2_fx(C_L2_AI0 - (p[WV_FATK] * C_L2_ATK) / 1000, 0);
    m->dc = c_1mexp(exp2_fx(C_L2_DC0 - (p[WV_DEC] * C_L2_DCR) / 1000, 0));
    m->fdc = c_1mexp(exp2_fx(C_L2_DC0 - (p[WV_FDEC] * C_L2_DCR) / 1000, 0));
    m->rc = c_1mexp(exp2_fx(C_L2_DC0 - (p[WV_REL] * C_L2_DCR) / 1000, 0));
    m->frc = m->rc;
    m->sus = p[WV_SUS] * 1073742;
    m->fsus = p[WV_FSUS] * 1073742;
    m->dirty = 0;
}

void wave_set(int t, int id, int v)
{
    wave_t *m = &WV[t];
    if (id < 0 || id >= WV_COUNT) return;
    if (v < 0) v = 0;
    if (v > WV_MAX[id]) v = WV_MAX[id];
    m->p[id] = (int16_t)v;
    m->dirty = 1;
}
int wave_get(int t, int id) { return WV[t].p[id]; }

void wave_init(int t)
{
    wave_t *m = &WV[t];
    int i;
    for (i = 0; i < (int)sizeof(*m); i++) ((uint8_t *)m)[i] = 0;
    for (i = 0; i < WV_COUNT; i++) m->p[i] = WV_DEF[i];
    m->note16 = 60 << 16;
    m->vel = 32767;
    wave_derive(m);
}

void wave_on(int t, int note, int vel15)
{
    wave_t *m = &WV[t];
    if (!wt_built) wt_build();                          /* the tables are computed at the first note, not at boot */
    m->vel = vel15;
    m->note16 = note << 16;
    m->gate = 1;
    m->a.st = 1;
    m->f.st = 1;
}
void wave_off(int t)
{
    wave_t *m = &WV[t];
    m->gate = 0;
    if (m->a.st) m->a.st = 4;
    if (m->f.st) m->f.st = 4;
}
int wave_active(int t) { return WV[t].a.st != 0; }

void wave_render(int t, int32_t *out, int n)
{
    wave_t *m = &WV[t];
    int i = 0, j;
    int32_t cut0, rq = n > 0 ? 65536 / n : 0;
    if (m->dirty) wave_derive(m);
    if (!m->si) { m->si = 1; m->cut_s = m->cut16; }
    cut0 = m->cut_s;
    while (i < n) {
        int cnt = n - i < 8 ? n - i : 8;
        int32_t rel = m->note16 - (60 << 16), ef = m->f.e, x16, g, pos;
        int32_t cutc = cut0 + (int32_t)(((int64_t)(m->cut16 - cut0) * ((i + cnt) * rq)) >> 16);
        uint32_t inc, inc2, incs;
        inc = exp2_fx(C_L2_MINC + (int32_t)(((int64_t)rel * 21845) >> 18) + m->oct16, 0);
        inc2 = inc + (uint32_t)(((uint64_t)inc * (uint32_t)m->det * 21u) / 1000u >> 10);        /* up to ~ +2 % (34 cents) */
        incs = inc >> 1;
        x16 = (int32_t)exp2_fx(C_L2_FC0 + cutc + (int32_t)(((int64_t)rel * 3604) >> 16) + (int32_t)(((int64_t)m->env5 * ef) >> 30), 16);
        if (x16 > 82354 || x16 < 0) x16 = 82354;
        g = (int32_t)c_expm((uint32_t)x16);
        pos = m->pos16 + (int32_t)(((int64_t)m->penv16 * ef) >> 30);
        if (pos < 0) pos = 0;
        if (pos > 15 * 65536) pos = 15 * 65536;
        for (j = 0; j < cnt; j++, i++) {
            int32_t s, e, o;
            m->ph += inc; m->ph2 += inc2; m->phs += incs;
            s = wt_read(m->ph, pos) + wt_read(m->ph2, pos);
            s = (s >> 1) + (int32_t)(((int64_t)SIN_TAB[(m->phs >> 22) & 1023u] * m->subl) >> 16);
            e = c_adsr(&m->a, m->ai, m->dc, m->sus, m->rc);
            (void)c_adsr(&m->f, m->fai, m->fdc, m->fsus, m->frc);
            m->y1 += (int32_t)(((int64_t)(s - m->y1) * g) >> 16);
            m->y2 += (int32_t)(((int64_t)(m->y1 - m->y2) * g) >> 16);
            o = (int32_t)(((int64_t)m->y2 * (e >> 15)) >> 15);
            o = (int32_t)(((int64_t)o * m->vel) >> 15);
            m->dcs += (int32_t)(((int64_t)((o << 8) - m->dcs) * 4294967) >> 32);
            o -= m->dcs >> 8;
            out[i] = (int32_t)(((int64_t)o * 39322 >> 15) * m->lev >> 15);
        }
    }
    m->cut_s = m->cut16;
}
