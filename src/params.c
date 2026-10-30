/* SPDX-License-Identifier: MPL-2.0 */
#include "octavio.h"
#include "params.h"
#include "consts_gen.h"

static const char *const OPT_WAVE[4] = { "Sine", "Triangle", "Sawtooth", "Square" };
static const char *const OPT_MODE[3] = { "Low pass", "Combo", "Gate" };
static const char *const OPT_OFF[2] = { "Off", "On" };

#include "pext_opts.h"
const pdesc_t PD[P_COUNT] = {
#include "pext_desc.h"
    [P_FOLD]  = { "Fold",               PK_RANGE,  0, FMT_PCT,   0, 1000, 10, 250, 0 },
    [P_SYM]   = { "Symmetry",           PK_RANGE,  0, FMT_PCT,   0, 1000, 10, 100, 0 },
    [P_DECAY] = { "VACT decay",         PK_RANGE,  0, FMT_MS,    0, 1000,  5, 750, 0 },
    [P_WAVE]  = { "Waveform",           PK_SELECT, 4, FMT_TEXT,  0, 3, 1, 0, OPT_WAVE },
    [P_FREQ]  = { "Frequency",          PK_RANGE,  0, FMT_FREQ,  0, 1000,  1, 485, 0 },
    [P_MWAVE] = { "Modulator waveform", PK_SELECT, 4, FMT_TEXT,  0, 3, 1, 0, OPT_WAVE },
    [P_RATIO] = { "FM ratio",           PK_RANGE,  0, FMT_RATIO, 250, 8000, 10, 2000, 0 },
    [P_FM]    = { "FM amount",          PK_RANGE,  0, FMT_PCT,   0, 1000, 10, 0, 0 },
    [P_FENV]  = { "FM envelope",        PK_RANGE,  0, FMT_PCT,   0, 1000, 10, 0, 0 },
    [P_EAD]   = { "AD envelope",        PK_CHECK,  2, FMT_TEXT,  0, 1, 1, 1, OPT_OFF },
    [P_ATK]   = { "Envelope attack",    PK_RANGE,  0, FMT_MS,    0, 1000,  5, 210, 0 },
    [P_EDEC]  = { "Envelope decay",     PK_RANGE,  0, FMT_MS,    0, 1000,  5, 640, 0 },
    [P_OPEN]  = { "LP cutoff",          PK_RANGE,  0, FMT_CUT,   0, 1000,  5, 0, 0 },
    [P_MODE]  = { "292 mode",           PK_SELECT, 3, FMT_TEXT,  0, 2, 1, 1, OPT_MODE },
};
int16_t PV[P_COUNT];

void param_reset(void)
{
    int i;
    for (i = 0; i < P_COUNT; i++)
        PV[i] = PD[i].def;
}

int32_t param_n_q16(int id)
{
    const pdesc_t *d = &PD[id];
    if (d->kind == PK_SELECT)
        return ((int32_t)PV[id] * 65536) / (d->nopt - 1);
    if (d->kind == PK_CHECK)
        return PV[id] ? 65536 : 0;
    return (((int32_t)PV[id] - d->min) * 65536) / (d->max - d->min);     /* <= 8000 * 65536: fits */
}

void param_set_n(int id, int32_t n)             /* n: Q16, 0..65536 */
{
    const pdesc_t *d = &PD[id];
    int32_t v;
    if (n < 0) n = 0;
    if (n > 65536) n = 65536;
    if (d->kind == PK_SELECT) {                 /* selectedIndex = round(n * (len-1)) */
        PV[id] = (int16_t)((n * (d->nopt - 1) + 32768) >> 16);
    } else if (d->kind == PK_CHECK) {
        PV[id] = n >= 32768;
    } else {                                    /* value = min + n*range, snapped to the step like <input type=range> */
        int32_t raw = n * (d->max - d->min);                              /* Q16 thousandths, <= 8000 * 65536 */
        int32_t k = (raw + (d->step << 15)) / (d->step << 16);
        v = d->min + k * d->step;
        if (v > d->max) v = d->max;
        PV[id] = (int16_t)v;
    }
}

static char *put_u(char *o, uint32_t v)
{
    char t[10];
    int n = 0;
    do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n)
        *o++ = t[--n];
    return o;
}
static char *put_fix(char *o, uint32_t v100)    /* v100: hundredths -> "d.dd" */
{
    o = put_u(o, v100 / 100u);
    *o++ = '.';
    *o++ = (char)('0' + (v100 / 10u) % 10u);
    *o++ = (char)('0' + v100 % 10u);
    return o;
}
static char *put_s(char *o, const char *s)
{
    while (*s)
        *o++ = *s++;
    return o;
}
static char *put_dur(char *o, uint32_t ms)      /* fmt(): "123 ms" or "1.23 s" */
{
    if (ms < 1000u) {
        o = put_u(o, ms);
        return put_s(o, " ms");
    }
    o = put_fix(o, (ms + 5u) / 10u);
    return put_s(o, " s");
}

static int32_t lg(int32_t s, int32_t k) { return (s * (k / 8)) / 125; }    /* s * k / 1000 without 64 bits */

void param_text(int id, char *out)
{
    const pdesc_t *d = &PD[id];
    char *o = out;
    int32_t s = PV[id];
    switch (d->fmt) {
    case FMT_PCT:                               /* Math.round(value * 100) + " %" */
        o = put_u(o, (uint32_t)((s * 100 + 500) / 1000));
        o = put_s(o, " %");
        break;
    case FMT_RATIO:
        o = put_fix(o, (uint32_t)(s / 10));
        break;
    case FMT_FREQ:                              /* 20 * 200^s Hz */
        o = put_u(o, (exp2_fx(K_LOG2_20 + lg(s, K_L200), 8) + 128u) >> 8);
        o = put_s(o, " Hz");
        break;
    case FMT_MS:
        if (id == P_DECAY) {                    /* .002 * 1000^s s: "ms" below 100 ms, else seconds, two decimals */
            o = put_dur(o, (exp2_fx(K_DEC0 + lg(s, K_L1000), 18) * 1000u + (1u << 17)) >> 18);   /* .002 * 1000^s s, fmt() */
        } else if (id == P_ATK) {               /* .001 * 2000^s s = 2000^s ms */
            o = put_dur(o, (exp2_fx(lg(s, K_L2000), 8) + 128u) >> 8);
        } else {                                /* .005 * 600^s s */
            o = put_dur(o, (exp2_fx(lg(s, K_L600), 8) * 5u + 128u) >> 8);
        }
        break;
    case FMT_INT:
        o = put_u(o, (uint32_t)s);
        break;
    case FMT_STEP:                              /* a sequencer step: "+50", "0", "-30" (Math.round(value * 100)) */
        if (s > 0) *o++ = '+';
        else if (s < 0) { *o++ = '-'; s = -s; }
        o = put_u(o, (uint32_t)((s + 5) / 10));
        break;
    case FMT_RATE:                              /* LFO rate: .05 * 800^s Hz ("0.7Hz": two decimals, the third dropped), or the division when synced */
        if (PV[id + 2]) {
            static const char *const DIVN[8] = { "4 bars", "2 bars", "1 bar", "1/2", "1/4", "1/8", "1/16", "1/32" };
            int di = s * 8 / 1000;
            o = put_s(o, DIVN[di > 7 ? 7 : di]);
        } else {
            uint32_t h = exp2_fx(-K_LOG2_20 + ((s * 632020) / 1000), 12);   /* Hz * 4096 */
            uint32_t h100 = (h * 100u + 2048u) >> 12;
            o = put_u(o, h100 / 100u);
            *o++ = '.';
            *o++ = (char)('0' + h100 / 10u % 10u);
            o = put_s(o, "Hz");
        }
        break;
    case FMT_CUT: {                             /* 50 * 2^(9 s) Hz: "123Hz" or "1.2k" */
        uint32_t f8 = exp2_fx(lg(s * 9, 65536) + 0, 8);
        uint32_t f = (f8 * 50u + 128u) >> 8;
        if (f < 1000u) {
            o = put_u(o, f);
            o = put_s(o, "Hz");
        } else {
            uint32_t t = (f8 * 50u * 10u + 128u) >> 8;                 /* tenths of kHz *100 */
            t = (t + 500u) / 1000u;
            o = put_u(o, t / 10u);
            *o++ = '.';
            o = put_u(o, t % 10u);
            *o++ = 'k';
        }
        break;
    }
    default:
        o = put_s(o, d->kind == PK_CHECK ? d->opts[PV[id] ? 1 : 0] : d->opts[PV[id]]);
    }
    *o = 0;
}

void param_text_ms(int attack, int s, char *out)    /* an envelope time: attack 2000^s ms, the others .005 * 600^s s (engine pages) */
{
    char *o = out;
    if (attack) o = put_dur(o, (exp2_fx(lg(s, K_L2000), 8) + 128u) >> 8);
    else o = put_dur(o, (exp2_fx(lg(s, K_L600), 8) * 5u + 128u) >> 8);
    *o = 0;
}
