/* SPDX-License-Identifier: MPL-2.0 */
/* TB-303 style bass line voice, one voice per track: a saw or square oscillator into a resonant 4-pole (24 dB) transistor-ladder-like
 * low-pass with a soft-clipped input, a decaying filter envelope, ACCENT and SLIDE. The conventions are the other engines': the
 * velocity is the accent (a hard note, vel > 0.6), and a note that begins while the previous one is still held is a slide
 * (legato: the pitch glides, the envelopes are not re-triggered), so the step sequencer and the keys get both without a new control. */
#include "voices.h"                                    /* cdsp.c is already included by eng_moog.c */

enum { TB_WAVE, TB_TUNE, TB_CUT, TB_RES, TB_ENV, TB_DEC, TB_ACC, TB_SLD, TB_DRV, TB_LVL, TB_COUNT };

typedef struct {
    int16_t p[TB_COUNT];
    uint8_t dirty, gate, acc, si;
    int32_t oct16, cut16, env5, k16, drv, lev, slc, accg;
    int32_t ai, sus, fsus;
    uint32_t dc, fdc, rc;
    int32_t cut_s;
    uint32_t ph;
    int32_t note16, tgt16, y1, y2, y3, y4;
    adsr_t a, f;
} tb3_t;

static tb3_t TB[4];
static const int16_t TB_DEF[TB_COUNT] = { 0, 500, 400, 700, 600, 350, 600, 300, 300, 1000 };
static const int16_t TB_MAX[TB_COUNT] = { 1, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000 };

static inline int32_t tb_sat(int32_t x)                /* soft clip: x - x^3 / (3 A^2) up to A = 1.5 (49152), then flat at 2/3 A */
{
    const int32_t A = 49152;
    int64_t t;
    if (x > A) x = A;
    if (x < -A) x = -A;
    t = ((int64_t)x * x / A) * x / A / 3;
    return (int32_t)(x - t);
}

static void tb3_derive(tb3_t *m)
{
    const int16_t *p = m->p;
    int d = (p[TB_TUNE] - 500) * 2, ad = d < 0 ? -d : d;
    int32_t sh = (int32_t)(((int64_t)ad * ad * 2505) >> 16);
    m->oct16 = (d < 0 ? -sh : sh);             /* the 303 plays one octave below the middle of the keyboard */
    m->cut16 = (p[TB_CUT] * 602931) / 1000;
    m->env5 = p[TB_ENV] * 5 * 65536 / 1000;
    m->k16 = (int32_t)(((int64_t)p[TB_RES] * 249037) / 1000);   /* up to 3.8: the ladder just rings */
    m->drv = 65536 + p[TB_DRV] * 196;                  /* 1x .. 4x the oscillator into the clipper */
    m->lev = p[TB_LVL] * 32768 / 1000;
    m->accg = p[TB_ACC];
    m->ai = (int32_t)exp2_fx(C_L2_AI0, 0);             /* the 303 has no attack: ~1 ms */
    m->fdc = c_1mexp(exp2_fx(C_L2_DC0 - (p[TB_DEC] * C_L2_DCR) / 1000, 0));
    m->dc = c_1mexp(exp2_fx(C_L2_DC0, 0));
    m->rc = c_1mexp(exp2_fx(C_L2_DC0 - (150 * C_L2_DCR) / 1000, 0));
    m->sus = 1073741824;
    m->fsus = 0;
    m->slc = 524288 / (220 + (p[TB_SLD] * 33) / 5);    /* per 8-sample chunk: a one-pole glide of 5 .. 155 ms */
    m->dirty = 0;
}

void tb3_set(int t, int id, int v)
{
    tb3_t *m = &TB[t];
    if (id < 0 || id >= TB_COUNT) return;
    if (v < 0) v = 0;
    if (v > TB_MAX[id]) v = TB_MAX[id];
    m->p[id] = (int16_t)v;
    m->dirty = 1;
}
int tb3_get(int t, int id) { return TB[t].p[id]; }

void tb3_init(int t)
{
    tb3_t *m = &TB[t];
    int i;
    for (i = 0; i < (int)sizeof(*m); i++) ((uint8_t *)m)[i] = 0;
    for (i = 0; i < TB_COUNT; i++) m->p[i] = TB_DEF[i];
    m->note16 = m->tgt16 = 60 << 16;
    tb3_derive(m);
}

void tb3_on(int t, int note, int vel15)
{
    tb3_t *m = &TB[t];
    m->tgt16 = note << 16;
    m->acc = vel15 > 19661;
    if (m->gate && m->a.st) return;                    /* legato: slide to the new pitch, no new envelope */
    m->note16 = m->tgt16;
    m->gate = 1;
    m->a.st = 1;
    m->f.st = 1;
}
void tb3_off(int t)
{
    tb3_t *m = &TB[t];
    m->gate = 0;
    if (m->a.st) m->a.st = 4;
}
int tb3_active(int t) { return TB[t].a.st != 0; }

void tb3_render(int t, int32_t *out, int n)
{
    tb3_t *m = &TB[t];
    int i = 0, j;
    if (m->dirty) tb3_derive(m);
    while (i < n) {
        int cnt = n - i < 8 ? n - i : 8;
        int32_t ef = m->f.e, x16, g, envg, mk;
        uint32_t inc;
        m->note16 += (int32_t)(((int64_t)(m->tgt16 - m->note16) * m->slc) >> 16);
        inc = exp2_fx(C_L2_MINC + (int32_t)(((int64_t)(m->note16 - (60 << 16)) * 21845) >> 18) + m->oct16, 0);
        envg = m->env5 + (m->acc ? (int32_t)(((int64_t)m->env5 * m->accg) >> 11) : 0);       /* accent: the sweep opens up to 50 % more */
        x16 = (int32_t)exp2_fx(C_L2_FC0 + m->cut16 + (int32_t)(((int64_t)envg * ef) >> 30), 16);
        if (x16 > 82354 || x16 < 0) x16 = 82354;
        g = (int32_t)c_expm((uint32_t)x16);
        if (g > 22000) g = 22000;                      /* above ~ 0.33 the 4 poles with their one-sample feedback delay oscillate at the Nyquist rate (checked on a model) */
        mk = 65536 + (int32_t)(((int64_t)m->k16 * 22938) >> 16);       /* makes up for the passband loss of the resonance */
        for (j = 0; j < cnt; j++, i++) {
            int32_t s, u, e, o;
            m->ph += inc;
            s = m->p[TB_WAVE] ? ((m->ph >> 31) ? -22000 : 22000) : (int32_t)(m->ph >> 17) - 16384;      /* square / falling saw, +-16k..22k */
            s = (int32_t)(((int64_t)s * m->drv) >> 16);
            u = tb_sat(s - (int32_t)(((int64_t)m->k16 * m->y4) >> 16));
            m->y1 += (int32_t)(((int64_t)(u - m->y1) * g) >> 16);
            m->y2 += (int32_t)(((int64_t)(m->y1 - m->y2) * g) >> 16);
            m->y3 += (int32_t)(((int64_t)(m->y2 - m->y3) * g) >> 16);
            m->y4 += (int32_t)(((int64_t)(m->y3 - m->y4) * g) >> 16);
            e = c_adsr(&m->a, m->ai, m->dc, m->sus, m->rc);
            (void)c_adsr(&m->f, m->ai, m->fdc, m->fsus, m->fdc);
            o = (int32_t)(((int64_t)m->y4 * mk) >> 16);
            o = (int32_t)(((int64_t)o * (e >> 15)) >> 15);
            if (m->acc) o += o >> 2;                    /* accent: a little louder */
            o = tb_sat(o);                              /* the resonance can peak very high: never beyond +-32k */
            out[i] = (int32_t)(((int64_t)o * 12000 >> 15) * m->lev >> 15);      /* ~ the Minimoog level */
        }
    }
}
