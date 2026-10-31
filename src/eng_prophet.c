/* SPDX-License-Identifier: MPL-2.0 */
/* The prototype's Prophet engine in fixed point: oscillator A (saw / triangle / pulse mix, sync) and B (tuned and
 * detuned against A), sub oscillator, noise, two ADSRs (amplifier / filter), a soft four-pole low-pass with feedback
 * (resonance) and poly-mod (osc B into the cutoff). Q15 audio, Q30 envelopes, Q16 pitches. */
#include "voices.h"

#define PR_NV 5                                    /* voices per Prophet track (a Prophet-5 has five) */
typedef struct {                                   /* one voice: oscillator phases, filter, envelopes, noise */
    uint32_t pa, pb, so;
    int32_t note, drift, fbf, z[4], dcs, vel, vel_s, oq;      /* oq: the last half-rate sample (ECO) */
    adsr_t a, f;
    noise_t n;
    uint16_t age;                                  /* allocation order: the oldest gated voice is stolen first */
    uint8_t gate, kill, sus, fade;                 /* governor: fade 1 = fading out over this block (then freed if kill, else suspended), 2 = fading in; sus: suspended, a held note that is not rendered */
} pvoice_t;
typedef struct {
    int16_t p[PR_COUNT];
    uint8_t dirty, sync, subty, wam, wbm, usea, useb, usesub, usenz;     /* wam / wbm: bit 0 saw, 1 triangle, 2 pulse: the weights that are not zero */
    int32_t wa[3], wb[3];                          /* saw / tri / pulse weights, Q15 */
    uint32_t pwa, pwb, pws;
    int32_t tuna, tunb, rnga, rngb, subo, lva, lvb, lvs, lvn;   /* pitch offsets: Q16 semitones (tune) / Q16 octaves */
    noisec_t nc;
    int32_t ai, fai, sus, fsus, cut16, env16, pm16, k16, lvl;
    int32_t cut_s, tuna_s, tunb_s;                 /* the values the previous block ended on: tune / cutoff ramp across the block instead of stepping */
    uint8_t si;
    uint16_t clk;
    uint32_t dc, rc, fdc, frc, fdc8, frc8;           /* fdc8 / frc8: the filter envelope's decay / release for 8 samples at once */
    int32_t dcs, olvl;                                /* the DC blocker, once per track (not per voice); the output gain with the 1.08 folded in */
    pvoice_t V[PR_NV];
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
    m->wam = (uint8_t)((m->wa[0] != 0) | ((m->wa[1] != 0) << 1) | ((m->wa[2] != 0) << 2));
    m->wbm = (uint8_t)((m->wb[0] != 0) | ((m->wb[1] != 0) << 1) | ((m->wb[2] != 0) << 2));
    m->lva = p[PR_LA] * 32768 / 1000;
    m->lvb = p[PR_LB] * 32768 / 1000;
    m->lvs = p[PR_SL] * 32768 / 1000;
    m->lvn = p[PR_NLEV] * 32768 / 1000;
    m->usea = m->lva != 0; m->useb = m->lvb != 0 || p[PR_PM] != 0; m->usesub = m->lvs != 0; m->usenz = m->lvn != 0;       /* a source at level 0 costs nothing */
    noise_coef(&m->nc, p[PR_NTYPE], p[PR_NTONE], p[PR_NCRUSH]);
    m->ai = (int32_t)exp2_fx(C_L2_AI0 - (p[PR_ATK] * C_L2_ATK) / 1000, 0);
    m->fai = (int32_t)exp2_fx(C_L2_AI0 - (p[PR_FATK] * C_L2_ATK) / 1000, 0);
    m->dc = c_1mexp(exp2_fx(C_L2_DC0 - (p[PR_DEC] * C_L2_DCR) / 1000, 0));
    m->rc = c_1mexp(exp2_fx(C_L2_DC0 - (p[PR_REL] * C_L2_DCR) / 1000, 0));
    m->fdc = c_1mexp(exp2_fx(C_L2_DC0 - (p[PR_FDEC] * C_L2_DCR) / 1000, 0));
    m->frc = c_1mexp(exp2_fx(C_L2_DC0 - (p[PR_FREL] * C_L2_DCR) / 1000, 0));
    m->fdc8 = c_pow8(m->fdc); m->frc8 = c_pow8(m->frc);
    m->sus = p[PR_SUS] * 1073742;
    m->fsus = p[PR_FSUS] * 1073742;
    m->cut16 = (p[PR_CUT] * 576717) / 1000;                       /* cut * 8.8 */
    m->env16 = (p[PR_ENV] * 360448) / 1000;                       /* env * 5.5 */
    m->pm16 = (p[PR_PM] * 196608) / 1000;                         /* pm * 3 */
    m->k16 = p[PR_RES] * 257;                                     /* 4 * res * .98 */
    m->lvl = p[PR_LVL] * 32768 / 1000;
    m->olvl = (int32_t)(((int64_t)35389 * m->lvl) >> 15);                  /* * 1.8 * .6 = 1.08, and the level */
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
    proph_derive(m);
}

/* ---- the voices: a note takes a free voice (the one with the quietest tail), else the quietest voice in release, else the oldest held note;
 * taking a voice never resets its envelope or its phase: the amplitude continues from where it was (attack from the current level), so a steal does not click ---- */
static pvoice_t *proph_alloc(proph_t *m, int note)
{
    int i;
    pvoice_t *q = 0;
    for (i = 0; i < PR_NV; i++) if (m->V[i].gate && m->V[i].note == note && !m->V[i].kill && !m->V[i].sus) return &m->V[i];        /* the same key again: retrigger its voice */
    for (i = 0; i < PR_NV; i++) {                                                   /* free: the one that stopped longest ago is the first one found, any will do */
        pvoice_t *v = &m->V[i];
        if (!v->a.st) { q = v; break; }
    }
    if (q) return q;
    for (i = 0; i < PR_NV; i++) {                                                   /* a suspended voice is silent already: the oldest of them */
        pvoice_t *v = &m->V[i];
        if (v->sus && (!q || (int16_t)(v->age - q->age) < 0)) q = v;
    }
    if (q) return q;
    for (i = 0; i < PR_NV; i++) {                                                   /* in release (or being killed): the quietest */
        pvoice_t *v = &m->V[i];
        if (!v->gate && (!q || v->a.e < q->a.e)) q = v;
    }
    if (q) return q;
    for (i = 0; i < PR_NV; i++) {                                                   /* all held: the oldest note */
        pvoice_t *v = &m->V[i];
        if (!q || (int16_t)(v->age - q->age) < 0) q = v;
    }
    return q;
}
void proph_on(int t, int note, int vel15)
{
    proph_t *m = &PR[t];
    pvoice_t *v = proph_alloc(m, note);
    if (!v->a.st || v->sus) { v->sus = 0; v->a.e = 0; v->f.e = 0; v->fbf = 0; v->z[0] = v->z[1] = v->z[2] = v->z[3] = 0; v->vel_s = vel15; }    /* a voice that was at rest starts clean */
    v->note = note;
    v->vel = vel15;
    v->gate = 1; v->kill = 0; v->fade = 0;
    v->age = ++m->clk;
    v->a.st = 1;
    v->f.st = 1;
}
static void proph_rel(pvoice_t *v)
{
    v->gate = 0;
    if (v->sus) { v->sus = 0; v->a.st = 0; v->f.st = 0; v->a.e = 0; v->f.e = 0; return; }         /* a suspended note that is let go has no tail to play */
    if (v->a.st) v->a.st = 4;
    if (v->f.st) v->f.st = 4;
}
void proph_off_note(int t, int note)                 /* the key (or the arpeggiator step) of `note` ends: its voice goes into release */
{
    proph_t *m = &PR[t];
    int i;
    for (i = 0; i < PR_NV; i++) if (m->V[i].gate && m->V[i].note == note) proph_rel(&m->V[i]);
}
void proph_off(int t)                                /* every voice of the track into release */
{
    proph_t *m = &PR[t];
    int i;
    for (i = 0; i < PR_NV; i++) proph_rel(&m->V[i]);
}
int proph_active(int t)
{
    int i;
    for (i = 0; i < PR_NV; i++) if (PR[t].V[i].a.st && !PR[t].V[i].sus) return 1;
    return 0;
}
int proph_nvoices(int t)                             /* voices sounding (not suspended, not fading out) */
{
    int i, n = 0;
    for (i = 0; i < PR_NV; i++) n += PR[t].V[i].a.st != 0 && !PR[t].V[i].sus && PR[t].V[i].fade != 1;
    return n;
}
int proph_nsuspended(int t)
{
    int i, n = 0;
    for (i = 0; i < PR_NV; i++) n += PR[t].V[i].sus;
    return n;
}
/* the governor takes a voice away: a voice in release (or the quietest one) fades out over the next block and is freed; a note that is still held is only
 * suspended (it keeps its place, costs nothing, and comes back with proph_resume_one when the load allows): a held chord does not lose notes for good */
int proph_shed_one(int t)
{
    proph_t *m = &PR[t];
    pvoice_t *q = 0;
    int i;
    for (i = 0; i < PR_NV; i++) {
        pvoice_t *v = &m->V[i];
        if (!v->a.st || v->sus || v->fade == 1) continue;
        if (!q || (!v->gate && q->gate) || (v->gate == q->gate && v->a.e < q->a.e)) q = v;       /* voices in release go first, then the quietest */
    }
    if (!q) return 0;
    q->fade = 1; q->kill = !q->gate;
    return 1;
}
int proph_resume_one(int t)
{
    proph_t *m = &PR[t];
    int i;
    for (i = 0; i < PR_NV; i++) if (m->V[i].sus) { m->V[i].sus = 0; m->V[i].fade = 2; return 1; }
    return 0;
}

static inline int32_t proph_osc(uint32_t ph, uint32_t inc, uint32_t rd, const int32_t *w, int mask, uint32_t pw)
{
    int64_t s = 0;
    if (mask & 1) s += (int64_t)c_mosc(ph, inc, rd, 1, 0) * w[0];
    if (mask & 2) s += (int64_t)c_mosc(ph, inc, rd, 0, 0) * w[1];
    if (mask & 4) s += (int64_t)c_mosc(ph, inc, rd, 3, pw) * w[2];
    return (int32_t)(s >> 15);
}

static uint8_t pv_eco;                             /* ECO (the governor, voices.c): the voices are computed at half the sample rate and interpolated: ~45 % cheaper, nothing above 11 kHz, polyphony kept */

/* one voice over the block, added into out[] */
static void proph_voice(proph_t *m, pvoice_t *v, int32_t *out, int n, const int32_t *cutc, const int32_t *tua, const int32_t *tub)
{
    int i = 0, j, ch = 0, eco = pv_eco;
    int32_t rq = 65536 / n, vel0 = v->vel_s, fade = v->fade == 1 ? 65536 : 0, fin = v->fade == 2;
    v->drift = (int32_t)(((int64_t)(v->drift + (c_rand15() >> 11)) * 65503) >> 16);
    while (i < n) {
        int32_t base = (v->note - 60) << 16, xa, xb, xs, ef = c_adsr8(&v->f, m->fai, m->fdc8, m->fsus, m->frc8), lx0;
        uint32_t incA, incB, incS = 0, rdA, rdB, rdS = 0, g0;
        int cnt = n - i < 8 ? n - i : 8;
        int32_t velc = vel0 + (int32_t)(((int64_t)(v->vel - vel0) * ((i + cnt) * rq)) >> 16);
        int32_t vg = (int32_t)(((int64_t)velc * m->olvl) >> 15);
        xa = C_L2_MINC + (int32_t)(((int64_t)(base + v->drift + tua[ch]) * 21845) >> 18) + m->rnga;
        xb = C_L2_MINC + (int32_t)(((int64_t)(base - v->drift + tub[ch]) * 21845) >> 18) + m->rngb;
        incA = exp2_fx(xa, 0) << eco; incB = exp2_fx(xb, 0) << eco;
        rdA = c_rd(incA); rdB = c_rd(incB);
        if (m->usesub) {
            xs = C_L2_MINC + (int32_t)(((int64_t)base * 21845) >> 18) + m->subo;
            incS = exp2_fx(xs, 0) << eco; rdS = c_rd(incS);
        }
        lx0 = C_L2_FC0 + 92735 /* log2(80/30): 2pi * 40 / FS */ + cutc[ch] + (int32_t)(((int64_t)base * 4369) >> 16) +
              (int32_t)(((int64_t)m->env16 * ef) >> 30);               /* 2pi fc / FS with fc = 40 * ... */
        g0 = 0;
        if (!m->p[PR_PM]) {
            int32_t x16 = (int32_t)(exp2_fx(lx0, 16) << eco);
            if (x16 > 164707 || x16 < 0) x16 = 164707;                 /* fc <= .4 FS : 2 pi * .4 */
            g0 = c_expm((uint32_t)x16);
        }
        for (j = 0; j < cnt; j += 1 + eco, i += 1 + eco) {
            int32_t a = 0, b = 0, sub = 0, nz = 0, sg, ea, x, o, g, k = m->k16;
            v->pb += incB;
            if (v->pb < incB && m->sync) v->pa = 0;
            v->pa += incA;
            v->so += incS;
            if (m->useb) b = proph_osc(v->pb, incB, rdB, m->wb, m->wbm, m->pwb);
            if (m->usea) a = proph_osc(v->pa, incA, rdA, m->wa, m->wam, m->pwa);
            if (m->usesub) sub = m->subty == 0 ? sin_q15(v->so) : c_mosc(v->so, incS, rdS, m->subty == 1 ? 0 : 3, m->pws);
            if (m->usenz) nz = noise_step(&v->n, &m->nc);
            sg = (int32_t)(((int64_t)a * m->lva + (int64_t)b * m->lvb + (int64_t)sub * m->lvs + (int64_t)nz * m->lvn) >> 15);
            sg = (int32_t)(((int64_t)sg * 14746) >> 15);                 /* * .45 */
            ea = c_adsr(&v->a, m->ai, m->dc, m->sus, m->rc);
            if (eco) ea = c_adsr(&v->a, m->ai, m->dc, m->sus, m->rc);          /* half rate: the envelope still advances 2 samples */
            if (m->p[PR_PM]) {
                int32_t x16 = (int32_t)(exp2_fx(lx0 + (int32_t)(((int64_t)m->pm16 * b) >> 15), 16) << eco);
                if (x16 > 164707 || x16 < 0) x16 = 164707;
                g = (int32_t)c_expm((uint32_t)x16);
            } else
                g = (int32_t)g0;
            v->fbf += (v->z[3] - v->fbf) >> 1;
            x = c_tanh((int32_t)(((int64_t)sg * 72090) >> 16) - (int32_t)(((int64_t)k * (v->fbf >> 5)) >> 16));   /* sg * 1.1 - k fbf */
            v->z[0] += (int32_t)(((int64_t)(x - (v->z[0] >> 5)) * g) >> 11);
            v->z[1] += (int32_t)(((int64_t)((v->z[0] - v->z[1]) >> 5) * g) >> 11);
            v->z[2] += (int32_t)(((int64_t)((v->z[1] - v->z[2]) >> 5) * g) >> 11);
            v->z[3] += (int32_t)(((int64_t)((v->z[2] - v->z[3]) >> 5) * g) >> 11);
            o = (int32_t)(((int64_t)(v->z[3] >> 5) * (65536 + (k >> 1))) >> 16);      /* * (1 + k * .5) */
            o = (int32_t)(((int64_t)o * (int32_t)(((int64_t)(ea >> 15) * vg) >> 15)) >> 15);        /* amplitude envelope * (velocity * level, once per chunk): 1.8 * .6 = 1.08 * level */
            if (fade) { fade -= rq << eco; o = (int32_t)(((int64_t)o * (fade < 0 ? 0 : fade)) >> 16); }      /* the governor takes the voice: a linear fade over the block */
            else if (fin) o = (int32_t)(((int64_t)o * (i + 1) * rq) >> 16);                         /* ... or gives it back: a linear fade in */
            if (eco) { out[i] += (v->oq + o) >> 1; out[i + 1] += o; }
            else out[i] += o;
            v->oq = o;
        }
        ch++;
    }
    v->vel_s = v->vel;
    if (v->fade == 1) {
        if (v->kill) { v->a.st = 0; v->f.st = 0; v->a.e = 0; v->f.e = 0; v->gate = 0; v->kill = 0; }
        else v->sus = 1;
    }
    v->fade = 0;
}

void proph_render(int t, int32_t *out, int n)
{
    proph_t *m = &PR[t];
    int i, ch, nch = (n + 7) >> 3;
    int32_t cut0, ta0, tb0, rq = n > 0 ? 65536 / n : 0;                /* 1 / n, Q16: the ramp position of sample i is i * rq */
    int32_t cutc[16], tua[16], tub[16];
    if (m->dirty) proph_derive(m);
    if (!m->si) { m->si = 1; m->cut_s = m->cut16; m->tuna_s = m->tuna; m->tunb_s = m->tunb; }
    cut0 = m->cut_s; ta0 = m->tuna_s; tb0 = m->tunb_s;
    for (ch = 0; ch < nch; ch++) {                                     /* the ramps of cutoff and tuning, shared by the voices (chunks of 8 samples) */
        int e8 = (ch * 8 + 8 < n ? ch * 8 + 8 : n);
        cutc[ch] = cut0 + (int32_t)(((int64_t)(m->cut16 - cut0) * (e8 * rq)) >> 16);
        tua[ch] = ta0 + (int32_t)(((int64_t)(m->tuna - ta0) * (e8 * rq)) >> 16);
        tub[ch] = tb0 + (int32_t)(((int64_t)(m->tunb - tb0) * (e8 * rq)) >> 16);
    }
    for (i = 0; i < n; i++) out[i] = 0;
    for (i = 0; i < PR_NV; i++) if (m->V[i].a.st && !m->V[i].sus) proph_voice(m, &m->V[i], out, n, cutc, tua, tub);
    for (i = 0; i < n; i++) {                                           /* the voices' sum: the DC blocker (once, not per voice), then a soft limit (slope 1 near zero): no clipping whatever the chord */
        int32_t o = out[i];
        m->dcs += (int32_t)(((int64_t)((o << 8) - m->dcs) * 4294967) >> 32);          /* .001 */
        o -= m->dcs >> 8;
        out[i] = c_tanh(o);
    }
    m->cut_s = m->cut16; m->tuna_s = m->tuna; m->tunb_s = m->tunb;
}
