/* SPDX-License-Identifier: MPL-2.0 */
/* TB-303 style bass line voice, one voice per track. What makes the original sound like itself, and what is done here:
 *  - its oscillator is a rounded saw (a capacitor charging, so the ramp is curved) and a square made by slicing that saw: not the
 *    ideal shapes of a textbook synth -> the saw is curved, the square is slanted and a little asymmetric;
 *  - the diode ladder: 4 poles, resonance fed back through a high-pass (the coupling capacitor), so the resonance never thickens the
 *    bass, it only squeals around the cutoff -> the feedback goes through a one-pole high-pass at ~130 Hz; the input is soft-clipped;
 *  - the cutoff range is narrow (~150 Hz .. 3 kHz) and the envelope adds up to 2.5 octaves; DECAY runs ~170 ms .. 2 s;
 *  - the ACCENT: a note is accented when it comes in hard (vel > 0.6); it (a) uses the shortest decay, (b) opens the sweep more, and
 *    (c) charges a capacitor (the accent-sweep circuit) that stays up for a few hundred ms, so consecutive accents build up and
 *    the filter sweep gets wilder and wilder;
 *  - the SLIDE: a note that begins while the previous one is still held glides to the new pitch (~60 ms) and does not re-trigger
 *    the envelopes (legato) -- the sequencer and the keys get it without any new control;
 *  - a 3 ms attack on the amplifier, no attack on the filter envelope. */
#include "voices.h"                                    /* cdsp.c is already included by eng_moog.c */


typedef struct {
    int16_t p[TB_COUNT];
    uint8_t dirty, gate, acc, si;
    int32_t oct16, cut16, env5, k16, drv, lev, slc, accg;
    int32_t ai, aai, sus, fsus;
    uint32_t dc, rc, fdc, fdca;
    uint32_t ph;
    int32_t note16, tgt16, y1, y2, y3, y4, fbl, accv, wz;
    adsr_t a, f;
} tb3_t;

static tb3_t TB[4];
static const int16_t TB_DEF[TB_COUNT] = { 0, 500, 450, 800, 650, 300, 700, 300, 100, 1000 };
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
    m->oct16 = d < 0 ? -sh : sh;
    m->cut16 = 222800 + (p[TB_CUT] * 288358) / 1000;           /* ~150 Hz .. 3 kHz per pole */
    m->env5 = p[TB_ENV] * 2 * 65536 / 1000 + p[TB_ENV] * 32768 / 1000;     /* up to 2.5 octaves of sweep */
    m->k16 = (int32_t)(((int64_t)p[TB_RES] * 393216) / 1000);  /* up to 6.0: with the high-pass in the loop it just starts to self-oscillate */
    m->drv = 65536 + p[TB_DRV] * 196;                  /* 1x .. 4x the oscillator into the clipper */
    m->lev = p[TB_LVL] * 32768 / 1000;
    m->accg = p[TB_ACC];
    m->ai = (int32_t)exp2_fx(C_L2_AI0 - (150 * C_L2_ATK) / 1000, 0);        /* ~ 3 ms */
    m->aai = (int32_t)exp2_fx(C_L2_AI0, 0);                                  /* the filter envelope: ~ 1 ms */
    m->fdc = c_1mexp(exp2_fx(C_L2_DC0 - ((520 + p[TB_DEC] * 38 / 100) * C_L2_DCR) / 1000, 0));      /* ~ 170 ms .. 2 s */
    m->fdca = c_1mexp(exp2_fx(C_L2_DC0 - (520 * C_L2_DCR) / 1000, 0));       /* an accented note: always the shortest */
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
    if (m->acc) m->accv += (int32_t)(((int64_t)(65536 - m->accv) * 3) >> 3);       /* the accent capacitor charges */
    if (m->gate && m->a.st) return;                    /* legato: slide to the new pitch, no new envelope */
    m->note16 = m->tgt16;
    m->gate = 1;
    m->a.st = 1;
    m->f.st = 1;
    m->f.e = 0;
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
        int32_t ef = m->f.e, x16, g, envg, cutacc;
        uint32_t inc;
        m->note16 += (int32_t)(((int64_t)(m->tgt16 - m->note16) * m->slc) >> 16);
        inc = exp2_fx(C_L2_MINC + (int32_t)(((int64_t)(m->note16 - (60 << 16)) * 21845) >> 18) + m->oct16, 0);
        m->accv -= m->accv >> 11;                                                   /* the capacitor leaks: ~ 0.4 s */
        envg = m->env5 + (int32_t)(((int64_t)m->env5 * m->accg * (m->acc ? 1000 + (m->accv >> 6) : (m->accv >> 6))) >> 20);   /* the sweep opens more with the accent and its stored charge */
        cutacc = (int32_t)(((int64_t)m->accv * m->accg) >> 12);                     /* and the charge lifts the cutoff itself a bit (up to ~ 1/4 octave) */
        x16 = (int32_t)exp2_fx(C_L2_FC0 + m->cut16 + cutacc + (int32_t)(((int64_t)envg * ef) >> 30), 16);
        if (x16 > 82354 || x16 < 0) x16 = 82354;
        g = (int32_t)c_expm((uint32_t)x16);
        if (g > 26000) g = 26000;                      /* ~ 0.4: above that a 4-pole with its one-sample feedback delay is no longer clean */
        for (j = 0; j < cnt; j++, i++) {
            int32_t s, u, e, o, fb, ramp;
            m->ph += inc;
            ramp = (int32_t)(m->ph >> 17);              /* rising 0 .. 32767 */
            ramp -= (int32_t)(((int64_t)ramp * ramp) >> 17) ;                                              /* the capacitor's curve: concave, ~ 25 % */
            if (m->p[TB_WAVE]) {                        /* the square: the saw sliced, slanted and a little asymmetric */
                s = (ramp - 13000) * 6;
                if (s > 17000) s = 17000;
                if (s < -21000) s = -21000;
                s += ramp >> 3;
            } else {
                s = ramp - 13500;                       /* the curved saw, re-centred */
            }
            s = (int32_t)(((int64_t)s * m->drv) >> 16);
            fb = m->y4 - m->fbl;                        /* the resonance goes through the high-pass of the loop */
            m->fbl += (int32_t)(((int64_t)(m->y4 - m->fbl) * 1200) >> 16);
            u = tb_sat(s - (int32_t)(((int64_t)m->k16 * fb) >> 16));
            m->y1 += (int32_t)(((int64_t)(u - m->y1) * g) >> 16);
            m->y2 += (int32_t)(((int64_t)(m->y1 - m->y2) * g) >> 16);
            m->y3 += (int32_t)(((int64_t)(m->y2 - m->y3) * g) >> 16);
            m->y4 += (int32_t)(((int64_t)(m->y3 - m->y4) * g) >> 16);
            e = c_adsr(&m->a, m->ai, m->dc, m->sus, m->rc);
            (void)c_adsr(&m->f, m->aai, m->acc ? m->fdca : m->fdc, m->fsus, m->fdc);
            o = (int32_t)(((int64_t)m->y4 * (e >> 15)) >> 15);
            if (m->acc) o += o >> 2;                    /* accent: a little louder */
            o = tb_sat(o);                              /* the resonance can peak very high: never beyond +-32k */
            out[i] = (int32_t)(((int64_t)o * 30000 >> 15) * m->lev >> 15);
        }
    }
}
