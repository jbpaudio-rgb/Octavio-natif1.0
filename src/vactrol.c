/* SPDX-License-Identifier: MPL-2.0 */
/* The prototype's "Vactrol" engine (BuchlaEngine), in fixed point: a sine / FM oscillator into a wavefolder, an
 * AD envelope that drives a two-state vactrol (fast + slow cell), and a one-pole low-pass / gate (292 mode).
 * Q15 audio, Q30 envelopes, 32-bit phases, no float, no 64-bit division. */
#include "octavio.h"
#include "params.h"
#include "consts_gen.h"

static struct {
    /* derived from the parameters (vac_update) */
    uint32_t a_turns;                 /* wavefolder gain a / 2pi, Q16 */
    int32_t sym15;                    /* symmetry offset, Q15 */
    uint32_t a_att, a_dec;            /* vactrol coefficients, Q32 */
    uint32_t ai30, dk32;              /* envelope attack step (Q30 per sample), decay 1-exp(-x) (Q32) */
    int32_t fm16, fenv16, ratio16, open16;
    int32_t freq_exp;                 /* K_INC0 + s * log2(200), Q16 */
    uint8_t wave, mwave, mode, ead;
    /* voice */
    uint32_t ph, mph, env;
    int32_t note, fa, sl, v30, lp, dc, vel;      /* vel: Q15 (the arpeggiator's accent / humanize, the sequencer) */
    uint32_t inc_s;                   /* the phase increment the previous block ended on (0 = none yet): the pitch ramps across the block */
    uint8_t gate, stage;
} V;

static inline uint32_t mulhi32(uint32_t a, uint32_t b) { return (uint32_t)(((uint64_t)a * b) >> 32); }

int32_t vac_wave_q15(uint32_t p, int t)           /* the prototype's wf(): sine, triangle, saw, square */
{
    switch (t) {
    case 0: return sin_q15(p);
    case 1: {
        int32_t d = (int32_t)(p - 0x80000000u);
        uint32_t a = d < 0 ? (uint32_t)(-d) : (uint32_t)d;      /* |p - .5| * 2^32 (<= 2^31) */
        return 32768 - (int32_t)(a >> 15);
    }
    case 2: return (int32_t)(p >> 16) - 32768;
    default: return p < 0x80000000u ? 32767 : -32767;
    }
}

static uint32_t one_minus_exp(uint32_t x)         /* 1 - exp(-x), x Q32 (small), Q32 */
{
    uint32_t x2 = mulhi32(x, x), x3 = mulhi32(x2, x);
    return x - (x2 >> 1) + x3 / 6u;
}

void vac_update(void)
{
    int32_t s;
    uint32_t f, f15, d_q26, tau, smp8, x;
    s = (PV[P_FOLD] * 65536) / 1000;                                   /* fold, Q16 */
    f = s >= 65536 ? 0xFFFFFFFFu : (uint32_t)s << 16;
    f15 = (uint32_t)(((uint64_t)s * isqrt32(f)) >> 16);                 /* fold^1.5, Q16 */
    V.a_turns = 5215u + (uint32_t)(((uint64_t)f15 * 312911u) >> 16);    /* (.5 + 30 fold^1.5) / 2pi */
    s = (PV[P_SYM] * 65536) / 1000;
    V.sym15 = (int32_t)(((uint64_t)((uint32_t)s * (uint32_t)s >> 16) * 78643u) >> 17);   /* sym^2 * 1.2 */
    /* VACT decay: d = .002 * 1000^s seconds */
    {
        int32_t ld = K_DEC0 + (PV[P_DECAY] * (K_L1000 / 8)) / 125;
        d_q26 = exp2_fx(ld, 26);
        x = exp2_fx(-(ld + K_LOG2FS), 32);                              /* 1 / (d * FS) */
        V.a_dec = one_minus_exp(x);
        if (d_q26 < 167772u)                                            /* d < .0025 */
            tau = 80531u + (uint32_t)(((uint64_t)d_q26 * 18350u) >> 16);   /* .0012 + .28 d */
        else if (d_q26 < 671089u)                                       /* d < .01 */
            tau = 147640u - (uint32_t)(((uint64_t)d_q26 * 7864u) >> 16);   /* .0022 - .12 d */
        else
            tau = 67109u;                                               /* .001 */
        smp8 = (uint32_t)(((uint64_t)tau * 44100u) >> 18);              /* tau * FS in Q8 */
        x = (0xFFFFFFFFu / smp8) << 8;                                  /* 1 / (tau * FS) in Q32 */
        V.a_att = one_minus_exp(x);
    }
    V.ai30 = exp2_fx(-(K_ATK0 + (PV[P_ATK] * (K_L2000 / 8)) / 125), 30);          /* 1 / (atk * FS), Q30 */
    V.dk32 = one_minus_exp(exp2_fx(K_EDEC0 - (PV[P_EDEC] * (K_L600 / 8)) / 125, 32));   /* 1 - exp(-5 / (edec * FS)) */
    V.ratio16 = (PV[P_RATIO] * 65536) / 1000;
    s = (PV[P_FM] * 65536) / 1000;
    V.fm16 = (int32_t)(((uint32_t)s * (uint32_t)s >> 16) * 2u);         /* v^2 * 2 */
    V.fenv16 = (PV[P_FENV] * 65536) / 1000;
    V.open16 = (PV[P_OPEN] * 65536) / 1000;
    V.freq_exp = K_INC0 + (PV[P_FREQ] * (K_L200 / 8)) / 125;
    V.wave = (uint8_t)PV[P_WAVE];
    V.mwave = (uint8_t)PV[P_MWAVE];
    V.mode = (uint8_t)PV[P_MODE];
    V.ead = (uint8_t)PV[P_EAD];
}

void vac_init(void)
{
    V.note = 60;
    V.vel = 32767;
    vac_update();
}
void vac_note_on(int note, int vel15) { V.note = note; V.vel = vel15; V.gate = 1; V.stage = 1; }
void vac_note_off(void) { V.gate = 0; }

void vac_render(int32_t *out, int n)
{
    uint32_t inc1 = exp2_fx(V.freq_exp + ((V.note - 60) * 65536) / 12, 0);
    uint32_t inc0 = V.inc_s ? V.inc_s : inc1, inc, minc;
    int64_t dinc = (int64_t)inc1 - (int64_t)inc0;
    int32_t rq = n > 0 ? 65536 / n : 0;                              /* 1 / n, Q16 */
    int i;
    V.inc_s = inc1;
    for (i = 0; i < n; i++) {
        int32_t o2, osc, x, y, t, g, c, v15, d;
        uint32_t off, pf;
        int64_t m;
        inc = (uint32_t)((int64_t)inc0 + ((dinc * ((i + 1) * rq)) >> 16));
        minc = (uint32_t)(((uint64_t)inc * (uint32_t)V.ratio16) >> 16);
        V.ph += inc;
        V.mph += minc;
        o2 = vac_wave_q15(V.mph, V.mwave);
        /* fm * (1 - fenv + fenv * v) * o2, as a phase offset in turns */
        g = (65536 - V.fenv16) + (int32_t)(((int64_t)V.fenv16 * (V.v30 >> 14)) >> 16);
        m = ((int64_t)V.fm16 * g) >> 16;
        off = (uint32_t)(m * o2) << 1;
        osc = vac_wave_q15(V.ph + off, V.wave);
        pf = ((uint32_t)V.a_turns * (uint32_t)(osc + V.sym15)) << 1;    /* a * (osc + sym) in turns */
        x = sin_q15(pf);                                                /* the wavefolder */
        if (V.ead) {
            if (V.stage == 1) {
                V.env += V.ai30;
                if (V.env >= (1u << 30)) { V.env = 1u << 30; V.stage = 2; }
            } else if (V.stage == 2) {
                V.env -= mulhi32(V.env, V.dk32);
                if (V.env < (1u << 30) / 1000u) { V.env = 0; V.stage = 0; }
            }
            if (V.stage == 1) {                                         /* smoothstep */
                int32_t e = (int32_t)(V.env >> 14);
                int32_t s2 = (int32_t)(((int64_t)e * e) >> 16);
                t = (int32_t)(((int64_t)s2 * (196608 - 2 * e)) >> 16) << 14;
            } else
                t = (int32_t)V.env;
        } else
            t = V.gate ? (1 << 30) : 0;
        if (V.vel < 32767) t = (int32_t)(((int64_t)t * V.vel) >> 15);      /* the note's velocity scales the vactrol's drive, as the prototype (t * vel) */
        /* the vactrol: a fast cell and a slow tail */
        d = t - V.fa;
        if (d > 0)
            V.fa += (int32_t)(((int64_t)d * V.a_att) >> 32);
        else {
            uint32_t cf = (uint32_t)(((uint64_t)V.a_dec * (26214u + (uint32_t)(((uint64_t)V.fa * 104858u) >> 30))) >> 16);
            V.fa += (int32_t)(((int64_t)d * cf) >> 32);
        }
        d = t - V.sl;
        if (d > 0)
            V.sl += (int32_t)(((int64_t)d * (V.a_att >> 1)) >> 32);
        else
            V.sl += (int32_t)(((int64_t)d * (uint32_t)(((uint64_t)V.a_dec * 13107u) >> 16)) >> 32);
        V.v30 = (int32_t)(((int64_t)V.fa * 21299 + (int64_t)V.sl * 11469) >> 15);   /* .65 fa + .35 sl */
        v15 = V.v30 >> 15;
        y = x;
        if (V.mode < 2) {                                               /* one-pole low-pass */
            int32_t gi, fr, gq;
            c = V.mode == 0 ? V.open16 + (int32_t)(((int64_t)(65536 - V.open16) * (V.v30 >> 14)) >> 16) : (V.v30 >> 14);
            if (c > 65536) c = 65536;
            gi = c >> 8;
            fr = c & 255;
            {
                extern const uint16_t LPG_TAB[257];
                gq = LPG_TAB[gi] + (((int32_t)LPG_TAB[gi + 1 > 256 ? 256 : gi + 1] - LPG_TAB[gi]) * fr >> 8);
            }
            V.lp += (int32_t)(((int64_t)((x << 8) - V.lp) * gq) >> 16);
            y = V.lp >> 8;
        }
        if (V.mode > 0)
            y = (y * v15) >> 15;                                        /* gate: the same vactrol drives the level */
        V.dc += (int32_t)(((int64_t)((y << 8) - V.dc) * 4294967) >> 32);   /* .001 */
        out[i] = (y - (V.dc >> 8)) >> 1;                                /* * .5 */
    }
}

/* the scope draws the engine's shape from the same parameters: u is the phase in turns (Q32) */
int32_t vac_scope(uint32_t u, int folded, int32_t *carrier)
{
    uint32_t um = (uint32_t)(((uint64_t)u * (uint32_t)V.ratio16) >> 16);
    int32_t o2 = vac_wave_q15(um, V.mwave), osc;
    osc = vac_wave_q15(u + ((uint32_t)((int64_t)V.fm16 * o2) << 1), V.wave);
    *carrier = vac_wave_q15(u, V.wave);
    if (folded)
        return sin_q15(((uint32_t)V.a_turns * (uint32_t)(osc + V.sym15)) << 1);
    return osc;
}
