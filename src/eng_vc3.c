/* SPDX-License-Identifier: MPL-2.0 */
/* VC3, one voice per track: a VCS3 (EMS Synthi) with its pin matrix.
 *  Sources (rows):      OSC1, OSC1 sine, OSC2, OSC2 sine, the slow LFO (O3), noise, the ring modulator (O1 x O2), the filter output, the envelope, the keyboard CV.
 *  Destinations (cols): OSC1 / OSC2 frequency, LFO rate, filter cutoff, filter resonance, filter input, the output, amplitude (AM), OSC1 / OSC2 wave shape.
 *  Any source goes to any destination with its own amount (a pin, 0 = no pin): that is what makes the machine - audio-rate FM both ways, the filter
 *  back into the oscillators or into its own input (feedback), ring modulation fed back, noise into everything.
 *  The oscillators morph sine -> saw -> square (band-limited); the filter is a 4-pole low-pass that rings and oscillates by itself; an ADSR sits on the output.
 *  The sources that change slowly (the LFO, the envelope, the keyboard) act in the log domain once per 8 samples; the audio sources frequency-modulate
 *  through the phase increment (linear FM, one sample late = the feedback loops are stable) and everything that goes to the filter input / output / AM is per sample. */
#include "voices.h"                                    /* cdsp.c is already included by eng_moog.c */

typedef struct { uint8_t s, d; int16_t a; } v3pin_t;
typedef struct {
    int16_t p[V3_COUNT];
    uint8_t dirty, gate, nc, n1, n2, nf, no, na;
    v3pin_t pc[56];                                     /* the pins that act once per chunk: any source into RATE / CUT / RES / O1W / O2W, the slow ones into O1F / O2F */
    uint8_t s1[7], s2[7], sf[10], so[10], sa[10];       /* the pins that act per sample: audio sources into O1F, O2F; any into FIN, OUT, AM */
    int16_t a1[7], a2[7], af[10], ao[10], aa[10];
    int32_t oct16, o2s16, cut16, k16, lev, l3, key;
    int32_t ai, sus;
    uint32_t dc, rc, inc3;
    uint32_t ph1, ph2, ph3;
    int32_t note16, y1, y2, y3, y4, lfo, vel, sv[V3S_N];
    adsr_t a;
} v3_t;

static v3_t V3[4];
static void v3_defaults(int16_t *p)
{
    static const int16_t D[V3_PIN0] = { 500, 350, 700, 503, 350, 0, 600, 450, 20, 500, 600, 350, 1000 };
    int i;
    for (i = 0; i < V3_PIN0; i++) p[i] = D[i];
    for (i = V3_PIN0; i < V3_COUNT; i++) p[i] = 0;
    p[V3_PIN(V3S_O1, V3D_FIN)] = 400; p[V3_PIN(V3S_O2, V3D_FIN)] = 300;                  /* the patch of a plain two-oscillator voice */
    p[V3_PIN(V3S_FILT, V3D_OUT)] = 1000;
    p[V3_PIN(V3S_ENV, V3D_CUT)] = 560; p[V3_PIN(V3S_KEY, V3D_CUT)] = 500; p[V3_PIN(V3S_LFO, V3D_CUT)] = 150;
    p[V3_PIN(V3S_LFO, V3D_O1F)] = 100; p[V3_PIN(V3S_LFO, V3D_O2F)] = 100;
}

static inline int32_t v3_sat(int32_t x)                /* soft clip: x - x^3 / (6.75 * 2^30) up to 1.5 (49152), flat beyond; shifts only */
{
    int32_t t;
    if (x > 49152) x = 49152;
    if (x < -49152) x = -49152;
    t = (int32_t)((((int64_t)x * x >> 15) * x >> 15) * 19 >> 7);
    return x - t;
}

static void v3_derive(v3_t *m)
{
    const int16_t *p = m->p;
    int d = (p[V3_TUNE] - 500) * 2, ad = d < 0 ? -d : d, s, c;
    int32_t sh = (int32_t)(((int64_t)ad * ad * 2505) >> 16);
    m->oct16 = d < 0 ? -sh : sh;
    m->o2s16 = (int32_t)(((int64_t)(p[V3_O2T] - 500) * 131072) / 1000);        /* +-12 semitones */
    m->l3 = 999424 + p[V3_O3R] * 697;                                          /* 0.05 Hz .. 80 Hz, per 8-sample chunk (log2, Q16) */
    m->cut16 = (p[V3_CUT] * 602931) / 1000;
    m->k16 = p[V3_RES] <= 800 ? p[V3_RES] * 295 : 236000 + (p[V3_RES] - 800) * 950;   /* 0 .. 6.5: past ~4 the ladder rings on its own */
    m->lev = p[V3_LVL] * 32768 / 1000;
    m->ai = (int32_t)exp2_fx(C_L2_AI0 - (p[V3_ATK] * C_L2_ATK) / 1000, 0);
    m->dc = c_1mexp(exp2_fx(C_L2_DC0 - (p[V3_DEC] * C_L2_DCR) / 1000, 0));
    m->rc = c_1mexp(exp2_fx(C_L2_DC0 - (p[V3_REL] * C_L2_DCR) / 1000, 0));
    m->sus = p[V3_SUS] * 1073742;
    m->nc = m->n1 = m->n2 = m->nf = m->no = m->na = 0;
    for (s = 0; s < V3S_N; s++)
        for (c = 0; c < V3D_N; c++) {
            int v = p[V3_PIN(s, c)], a = v * 32768 / 1000;
            int audio = s == V3S_O1 || s == V3S_O1S || s == V3S_O2 || s == V3S_O2S || s == V3S_NOI || s == V3S_RING || s == V3S_FILT;
            if (!v) continue;
            if (c == V3D_FIN) { m->sf[m->nf] = (uint8_t)s; m->af[m->nf++] = (int16_t)a; }
            else if (c == V3D_OUT) { m->so[m->no] = (uint8_t)s; m->ao[m->no++] = (int16_t)a; }
            else if (c == V3D_AM) { m->sa[m->na] = (uint8_t)s; m->aa[m->na++] = (int16_t)a; }
            else if ((c == V3D_O1F || c == V3D_O2F) && audio) {
                if (c == V3D_O1F) { m->s1[m->n1] = (uint8_t)s; m->a1[m->n1++] = (int16_t)a; }
                else { m->s2[m->n2] = (uint8_t)s; m->a2[m->n2++] = (int16_t)a; }
            } else { m->pc[m->nc].s = (uint8_t)s; m->pc[m->nc].d = (uint8_t)c; m->pc[m->nc++].a = (int16_t)a; }
        }
    m->dirty = 0;
}

void v3_set(int t, int id, int v)
{
    v3_t *m = &V3[t];
    if (id < 0 || id >= V3_COUNT) return;
    if (v < 0) v = 0;
    if (v > (id == V3_O3S ? 3 : 1000)) v = id == V3_O3S ? 3 : 1000;
    m->p[id] = (int16_t)v;
    m->dirty = 1;
}
int v3_get(int t, int id) { return V3[t].p[id]; }

void v3_init(int t)
{
    v3_t *m = &V3[t];
    int i;
    for (i = 0; i < (int)sizeof(*m); i++) ((uint8_t *)m)[i] = 0;
    v3_defaults(m->p);
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
    int i = 0, j, q;
    if (m->dirty) v3_derive(m);
    while (i < n) {
        int cnt = n - i < 8 ? n - i : 8;
        int32_t *sv = m->sv, nn, lg1 = 0, lg2 = 0, cm, ratem = 0, x16, g, k16, mo1, mo2, lg3;
        uint32_t inc1, inc2, rd1, rd2, old3;
        nn = (int32_t)(((int64_t)(m->note16 - (60 << 16)) * 21845) >> 18);          /* the note, in octaves, Q16 */
        sv[V3S_ENV] = m->a.e >> 15;
        sv[V3S_KEY] = nn >> 1 > 32767 ? 32767 : nn >> 1 < -32767 ? -32767 : nn >> 1;
        for (q = 0; q < m->nc; q++)                                                   /* the LFO's own rate: pins into RATE (the sources as they stood at the end of the last chunk) */
            if (m->pc[q].d == V3D_RATE) ratem += (int32_t)(((int64_t)sv[m->pc[q].s] * m->pc[q].a) >> 15);
        lg3 = m->l3 + (ratem << 3);
        lg3 = lg3 < 900000 ? 900000 : lg3 > 1800000 ? 1800000 : lg3;
        old3 = m->ph3;
        m->ph3 += exp2_fx(lg3, 0);
        switch (m->p[V3_O3S]) {                                                       /* O3: sine, triangle, square, sample & hold */
        case 0: m->lfo = SIN_TAB[(m->ph3 >> 22) & 1023u]; break;
        case 1: { int32_t u = (int32_t)(m->ph3 >> 16); m->lfo = (u < 32768 ? u : 65535 - u) * 2 - 32768; break; }
        case 2: m->lfo = m->ph3 < 0x80000000u ? 32767 : -32767; break;
        default: if (m->ph3 < old3) m->lfo = c_rand15(); break;
        }
        sv[V3S_LFO] = m->lfo;
        cm = m->cut16; k16 = m->k16; mo1 = m->p[V3_O1W]; mo2 = m->p[V3_O2W];
        for (q = 0; q < m->nc; q++) {
            int s = m->pc[q].s, d = m->pc[q].d;
            int32_t a = m->pc[q].a, v = (int32_t)(((int64_t)sv[s] * a) >> 15);
            if (d == V3D_O1F) lg1 += s == V3S_KEY ? (int32_t)(((int64_t)nn * a) >> 15) : v << 1;                  /* 1 octave at full */
            else if (d == V3D_O2F) lg2 += s == V3S_KEY ? (int32_t)(((int64_t)nn * a) >> 15) : v << 1;
            else if (d == V3D_CUT) cm += s == V3S_KEY ? (int32_t)(((int64_t)nn * a) >> 15) : v << 3;                  /* 4 octaves at full */
            else if (d == V3D_RES) k16 += (int32_t)(((int64_t)v * 196608) >> 15);
            else if (d == V3D_O1W) mo1 += (int32_t)(((int64_t)v * 1000) >> 15);
            else if (d == V3D_O2W) mo2 += (int32_t)(((int64_t)v * 1000) >> 15);
        }
        mo1 = mo1 < 0 ? 0 : mo1 > 1000 ? 1000 : mo1; mo2 = mo2 < 0 ? 0 : mo2 > 1000 ? 1000 : mo2;
        k16 = k16 < 0 ? 0 : k16 > 600000 ? 600000 : k16;
        lg1 += C_L2_MINC + nn + m->oct16;
        lg2 += C_L2_MINC + nn + m->oct16 + m->o2s16;
        inc1 = exp2_fx(lg1, 0); rd1 = exp2_fx((40 << 16) - lg1, 0);
        inc2 = exp2_fx(lg2, 0); rd2 = exp2_fx((40 << 16) - lg2, 0);
        x16 = (int32_t)exp2_fx(C_L2_FC0 + cm, 16);
        if (x16 > 82354 || x16 < 0) x16 = 82354;
        g = (int32_t)c_expm((uint32_t)x16);
        if (g > 26000) g = 26000;                       /* a 4-pole with its one-sample feedback delay is no longer clean above ~ 0.4 */
        for (j = 0; j < cnt; j++, i++) {
            int32_t o1, o2, s, u, e, o, fm1 = 0, fm2 = 0, am = 32768, ob = 0;
            for (q = 0; q < m->n1; q++) fm1 += (int32_t)(((int64_t)sv[m->s1[q]] * m->a1[q]) >> 15);        /* audio-rate FM: last sample's sources */
            for (q = 0; q < m->n2; q++) fm2 += (int32_t)(((int64_t)sv[m->s2[q]] * m->a2[q]) >> 15);
            m->ph1 += (uint32_t)((int32_t)inc1 + (int32_t)(((int64_t)inc1 * fm1) >> 15));
            m->ph2 += (uint32_t)((int32_t)inc2 + (int32_t)(((int64_t)inc2 * fm2) >> 15));
            o1 = v3_osc(m->ph1, inc1, rd1, mo1);
            o2 = v3_osc(m->ph2, inc2, rd2, mo2);
            sv[V3S_O1] = o1 << 1; sv[V3S_O2] = o2 << 1;
            sv[V3S_O1S] = SIN_TAB[(m->ph1 >> 22) & 1023u]; sv[V3S_O2S] = SIN_TAB[(m->ph2 >> 22) & 1023u];
            sv[V3S_RING] = (int32_t)(((int64_t)o1 * o2) >> 13);
            sv[V3S_NOI] = c_rand15();
            s = 0;
            for (q = 0; q < m->nf; q++) s += (int32_t)(((int64_t)sv[m->sf[q]] * m->af[q]) >> 15);
            u = v3_sat(s + (c_rand15() >> 10) - (int32_t)(((int64_t)k16 * (m->y4 >> 8)) >> 16));          /* a whisper of noise at the input starts the self-oscillation, as on the real thing */
            m->y1 += (int32_t)(((int64_t)((u << 8) - m->y1) * g) >> 16);                                        /* the four poles keep 8 extra bits: at a low cutoff the steps are tiny */
            m->y2 += (int32_t)(((int64_t)(m->y1 - m->y2) * g) >> 16);
            m->y3 += (int32_t)(((int64_t)(m->y2 - m->y3) * g) >> 16);
            m->y4 += (int32_t)(((int64_t)(m->y3 - m->y4) * g) >> 16);
            sv[V3S_FILT] = m->y4 >> 8;
            for (q = 0; q < m->no; q++) ob += (int32_t)(((int64_t)sv[m->so[q]] * m->ao[q]) >> 15);
            for (q = 0; q < m->na; q++) am += (int32_t)(((int64_t)sv[m->sa[q]] * m->aa[q]) >> 15);
            am = am < 0 ? 0 : am > 65535 ? 65535 : am;
            e = c_adsr(&m->a, m->ai, m->dc, m->sus, m->rc);
            o = (int32_t)(((int64_t)ob * am) >> 15);
            o = (int32_t)(((int64_t)o * (e >> 15)) >> 15);
            o = (int32_t)(((int64_t)o * m->vel) >> 15);
            o = v3_sat((int32_t)((int64_t)o * 40000 >> 15));                        /* the gain first, then the clipper: never beyond +-32k */
            out[i] = (int32_t)((int64_t)o * m->lev >> 15);
        }
    }
}
