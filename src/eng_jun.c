/* SPDX-License-Identifier: MPL-2.0 */
/* JUN, five voices per track: a Juno-106 in the spirit of its circuit.
 *  - one DCO per voice: a saw and a pulse (manual width, or the LFO sweeping it), a square sub-oscillator an octave down and noise, each with its slider;
 *  - the high-pass (here a continuous cut-off instead of the four positions), then the 24 dB/oct resonant low-pass (it self-oscillates at the top of RESO),
 *    moved by the ONE envelope (shared with the amplifier, like the original), the triangle LFO (with its delay) and the keyboard;
 *  - the amplifier follows the envelope, or the gate;
 *  - the chorus of the Juno (I, II or both) at the end of the track, in mono: two taps of a modulated delay.
 * Polyphony works like the Prophet's: a note takes a free voice, else the quietest one in release, else the oldest held note. */
#include "voices.h"                                    /* cdsp.c is already included by eng_moog.c */

#define JN_NV 5
typedef struct {
    uint32_t ph, so;
    int32_t drift, fbf, z[4], hz, ag;
    int16_t vel, vel_s;
    uint16_t ldv, age;
    adsr_t a;
    uint8_t note, gate;
    int8_t doff;
} jvoice_t;
typedef struct {
    int16_t p[JN_COUNT];
    uint8_t dirty;
    int32_t oct16, lp, lf, cut16, env16, k16, kyb, olvl, hpg, pwm, dleta, nmix[3], mn;
    uint32_t pw, inc3, ph3, cph, cinc;
    int32_t ai, sus, dcs;
    uint32_t dc, rc;
    int32_t cdepth, cbase;
    uint16_t clk;
    uint16_t cw;
    int16_t dl[256];
    jvoice_t V[JN_NV];
} jn_t;

static jn_t JN[4];
static const int16_t JN_DEF[JN_COUNT] = { 500, 700, 500, 300, 0, 500, 0, 0, 300, 0, 0, 550, 250, 400, 0, 900, 20, 450, 600, 300, 0, 1, 150, 1000 };
static const int16_t JN_MAX[JN_COUNT] = { 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1, 3, 1000, 1000 };

static void jn_derive(jn_t *m)
{
    const int16_t *p = m->p;
    int d = (p[JN_TUNE] - 500) * 2, ad = d < 0 ? -d : d;
    int32_t sh = (int32_t)(((int64_t)ad * ad * 2505) >> 16);
    static const uint32_t CR[4] = { 0, 49961u, 84053u, 949569u };                 /* chorus I .513 Hz, II .863 Hz, I + II 9.75 Hz */
    m->oct16 = d < 0 ? -sh : sh;
    m->inc3 = exp2_fx(999424 + p[JN_LR] * 697, 0);                                /* 0.05 Hz .. 80 Hz, per 8-sample chunk */
    m->lp = (int32_t)(((int64_t)p[JN_DCO] * p[JN_DCO] * 32768) / 1000000);          /* LFO -> pitch (square law: fine at the low end), up to +-0.5 octave */
    m->lf = p[JN_LFC] * 131072 / 1000;                                            /* LFO -> cutoff, up to +-2 octaves */
    m->pw = 0x80000000u - (uint32_t)p[JN_PW] * 1932735u;
    m->pwm = p[JN_PWM];
    m->cut16 = (p[JN_CUT] * 576717) / 1000;
    m->env16 = (p[JN_ENV] * 360448) / 1000;
    m->k16 = p[JN_RES] * 268;
    m->kyb = p[JN_KYB] * 5461 / 1000;                                              /* cutoff follows the key: 0 .. 1 octave per octave */
    m->hpg = p[JN_HPF] ? (int32_t)c_expm(exp2_fx(C_L2_FC0 + p[JN_HPF] * 360448 / 1000, 16)) : 0;
    m->nmix[0] = p[JN_SAW] * 32768 / 1000; m->nmix[1] = p[JN_PUL] * 32768 / 1000; m->nmix[2] = p[JN_SUB] * 32768 / 1000;
    m->mn = p[JN_NSE] * 32768 / 1000;
    m->olvl = (int32_t)(((int64_t)p[JN_LVL] * 32768 / 1000 * 66000) >> 15);
    m->ai = (int32_t)exp2_fx(C_L2_AI0 - (p[JN_ATK] * C_L2_ATK) / 1000, 0);
    m->dc = c_1mexp(exp2_fx(C_L2_DC0 - (p[JN_DEC] * C_L2_DCR) / 1000, 0));
    m->rc = c_1mexp(exp2_fx(C_L2_DC0 - (p[JN_REL] * C_L2_DCR) / 1000, 0));
    m->sus = p[JN_SUS] * 1073742;
    m->cinc = CR[p[JN_CHO] & 3];
    m->cdepth = p[JN_CHO] == 3 ? 9 : 66;                                           /* samples of swing */
    m->cbase = p[JN_CHO] == 3 ? 150 : 154;
    m->dleta = p[JN_LD] ? 65536 * 1000 / (5512 * (100 + p[JN_LD] * 3)) + 1 : 65535;   /* LFO delay: per 8 samples, up to about 3 s */
    m->dirty = 0;
}

void jn_set(int t, int id, int v)
{
    jn_t *m = &JN[t];
    if (id < 0 || id >= JN_COUNT) return;
    if (v < 0) v = 0;
    if (v > JN_MAX[id]) v = JN_MAX[id];
    m->p[id] = (int16_t)v;
    m->dirty = 1;
}
int jn_get(int t, int id) { return JN[t].p[id]; }

void jn_init(int t)
{
    jn_t *m = &JN[t];
    int i;
    for (i = 0; i < (int)sizeof(*m); i++) ((uint8_t *)m)[i] = 0;
    for (i = 0; i < JN_COUNT; i++) m->p[i] = JN_DEF[i];
    for (i = 0; i < JN_NV; i++) m->V[i].doff = (int8_t)((i * 37) % 11 - 5);            /* each voice's fixed detune step, -5 .. +5 */
    jn_derive(m);
}

static jvoice_t *jn_alloc(jn_t *m, int note)
{
    int i;
    jvoice_t *q = 0;
    for (i = 0; i < JN_NV; i++) if (m->V[i].gate && m->V[i].note == note) return &m->V[i];
    for (i = 0; i < JN_NV; i++) if (!m->V[i].a.st) return &m->V[i];
    for (i = 0; i < JN_NV; i++) { jvoice_t *v = &m->V[i]; if (!v->gate && (!q || v->a.e < q->a.e)) q = v; }
    if (q) return q;
    for (i = 0; i < JN_NV; i++) { jvoice_t *v = &m->V[i]; if (!q || (int16_t)(v->age - q->age) < 0) q = v; }
    return q;
}
void jn_on(int t, int note, int vel15)
{
    jn_t *m = &JN[t];
    jvoice_t *v = jn_alloc(m, note);
    if (!v->a.st) { int8_t d = v->doff; int j; for (j = 0; j < (int)sizeof(*v); j++) ((uint8_t *)v)[j] = 0; v->doff = d; v->vel_s = (int16_t)vel15; }
    v->note = (uint8_t)note;
    v->vel = (int16_t)vel15;
    v->gate = 1;
    v->ldv = 0;
    v->age = ++m->clk;
    v->a.st = 1;
}
static void jn_rel(jvoice_t *v) { v->gate = 0; if (v->a.st) v->a.st = 4; }
void jn_off_note(int t, int note)
{
    int i;
    for (i = 0; i < JN_NV; i++) if (JN[t].V[i].gate && JN[t].V[i].note == note) jn_rel(&JN[t].V[i]);
}
void jn_off(int t) { int i; for (i = 0; i < JN_NV; i++) jn_rel(&JN[t].V[i]); }
int jn_active(int t)
{
    int i;
    for (i = 0; i < JN_NV; i++) if (JN[t].V[i].a.st) return 1;
    return 0;
}

static void jn_voice(jn_t *m, jvoice_t *v, int32_t *out, int n, const int32_t *lfoc)
{
    int i = 0, j, ch = 0;
    int32_t rq = 65536 / n, vel0 = v->vel_s, gm = m->p[JN_VCA];
    v->drift = (int32_t)(((int64_t)(v->drift + (c_rand15() >> 11)) * 65503) >> 16);
    while (i < n) {
        int cnt = n - i < 8 ? n - i : 8;
        int32_t lfo = lfoc[ch], lfd, base = ((int32_t)v->note - 60) << 16, xa, lx0, velc, vg, pwq;
        uint32_t inc, rd, incS, rdS, pw, g0;
        int64_t pq;
        if (v->ldv < 65535) { int32_t l = v->ldv + m->dleta; v->ldv = (uint16_t)(l > 65535 ? 65535 : l); }
        lfd = (int32_t)(((int64_t)lfo * v->ldv) >> 16);                         /* the LFO after its delay */
        xa = C_L2_MINC + (int32_t)(((int64_t)(base + v->drift + v->doff * m->p[JN_DET] * 3) * 21845) >> 18) + m->oct16 + (int32_t)(((int64_t)lfd * m->lp) >> 15);
        inc = exp2_fx(xa, 0); rd = c_rd(inc);
        incS = inc >> 1; rdS = rd > 0x7FFFFFFFu ? rd : rd << 1;
        pq = (int64_t)m->pw + (int64_t)lfo * m->pwm * 52; pwq = 0;
        if (pq < 214748365) pq = 214748365;
        if (pq > 2147483648LL) pq = 2147483648LL;
        pw = (uint32_t)pq; (void)pwq;
        lx0 = C_L2_FC0 + 92735 + m->cut16 + (int32_t)(((int64_t)base * m->kyb) >> 16) + (int32_t)(((int64_t)lfd * m->lf) >> 15) +
              (int32_t)(((int64_t)m->env16 * v->a.e) >> 30);
        {
            int32_t x16 = (int32_t)exp2_fx(lx0, 16);
            if (x16 > 164707 || x16 < 0) x16 = 164707;
            g0 = c_expm((uint32_t)x16);
        }
        velc = vel0 + (int32_t)(((int64_t)(v->vel - vel0) * ((i + cnt) * rq)) >> 16);
        vg = (int32_t)(((int64_t)velc * m->olvl) >> 15);
        for (j = 0; j < cnt; j++, i++) {
            int32_t s = 0, sub = 0, o, x, ea, k = m->k16;
            v->ph += inc; v->so += incS;
            if (m->nmix[0]) s += (int32_t)(((int64_t)c_mosc(v->ph, inc, rd, 1, 0) * m->nmix[0]) >> 15);
            if (m->nmix[1]) s += (int32_t)(((int64_t)c_mosc(v->ph, inc, rd, 3, pw) * m->nmix[1]) >> 15);
            if (m->nmix[2]) sub = c_mosc(v->so, incS, rdS, 2, 0), s += (int32_t)(((int64_t)sub * m->nmix[2]) >> 15);
            if (m->mn) s += (int32_t)(((int64_t)(c_rand15() >> 1) * m->mn) >> 15);
            s = (int32_t)(((int64_t)s * 14746) >> 15);                               /* * .45 */
            if (m->hpg) { v->hz += (int32_t)(((int64_t)((s << 5) - v->hz) * (int32_t)m->hpg) >> 16); s -= v->hz >> 5; }
            s += c_rand15() >> 11;                                                   /* a trace of noise: lets the resonance start oscillating from silence */
            ea = c_adsr(&v->a, m->ai, m->dc, m->sus, m->rc);
            v->fbf += (v->z[3] - v->fbf) >> 1;
            x = c_tanh((int32_t)(((int64_t)s * 72090) >> 16) - (int32_t)(((int64_t)k * (v->fbf >> 5)) >> 16));
            v->z[0] += (int32_t)(((int64_t)(x - (v->z[0] >> 5)) * (int32_t)g0) >> 11);
            v->z[1] += (int32_t)(((int64_t)((v->z[0] - v->z[1]) >> 5) * (int32_t)g0) >> 11);
            v->z[2] += (int32_t)(((int64_t)((v->z[1] - v->z[2]) >> 5) * (int32_t)g0) >> 11);
            v->z[3] += (int32_t)(((int64_t)((v->z[2] - v->z[3]) >> 5) * (int32_t)g0) >> 11);
            o = (int32_t)(((int64_t)(v->z[3] >> 5) * (65536 + (k >> 1))) >> 16);
            if (gm) { int32_t tg = v->gate ? (1 << 30) : 0; v->ag += (int32_t)(((int64_t)(tg - v->ag) * 214748365) >> 32); ea = v->ag; if (!v->gate && ea < 1000000) v->a.st = 0; }
            o = (int32_t)(((int64_t)o * (int32_t)(((int64_t)(ea >> 15) * vg) >> 15)) >> 15);
            out[i] += o;
        }
        ch++;
    }
    v->vel_s = v->vel;
}

void jn_render(int t, int32_t *out, int n)
{
    jn_t *m = &JN[t];
    int i, ch, nch = (n + 7) >> 3;
    int32_t lfoc[16];
    if (m->dirty) jn_derive(m);
    for (ch = 0; ch < nch; ch++) {
        int32_t u;
        m->ph3 += m->inc3;
        u = (int32_t)(m->ph3 >> 16);
        lfoc[ch] = (u < 32768 ? u : 65535 - u) * 2 - 32768;
    }
    for (i = 0; i < n; i++) out[i] = 0;
    for (i = 0; i < JN_NV; i++) if (m->V[i].a.st) jn_voice(m, &m->V[i], out, n, lfoc);
    for (i = 0; i < n; i++) {
        int32_t o = (int32_t)(((int64_t)out[i] * ENGINE_TRIM_Q15) >> 15);
        m->dcs += (int32_t)(((int64_t)((o << 8) - m->dcs) * 4294967) >> 32);
        o -= m->dcs >> 8;
        if (m->p[JN_CHO]) {                                                         /* chorus: two taps swept in opposite directions */
            int32_t tri, d1, d2, a1, a2, w1, w2, wet;
            uint32_t ix;
            m->dl[m->cw & 255] = (int16_t)(o > 32767 ? 32767 : o < -32768 ? -32768 : o);
            m->cph += m->cinc;
            tri = (int32_t)(m->cph >> 16); tri = (tri < 32768 ? tri : 65535 - tri) * 2 - 32768;       /* -32768 .. 32767 */
            d1 = (m->cbase << 8) + (int32_t)(((int64_t)tri * m->cdepth) >> 7);
            d2 = (m->cbase << 8) - (int32_t)(((int64_t)tri * m->cdepth) >> 7);
            ix = (uint32_t)(m->cw - (uint32_t)(d1 >> 8)); a1 = d1 & 255;
            w1 = (m->dl[ix & 255] * (256 - a1) + m->dl[(ix - 1) & 255] * a1) >> 8;
            ix = (uint32_t)(m->cw - (uint32_t)(d2 >> 8)); a2 = d2 & 255;
            w2 = (m->dl[ix & 255] * (256 - a2) + m->dl[(ix - 1) & 255] * a2) >> 8;
            wet = (w1 + w2) >> 1;
            m->cw++;
            o = (int32_t)(((int64_t)o * 21299 + (int64_t)wet * 21299) >> 15);
        }
        out[i] = c_tanh(o);
    }
}
