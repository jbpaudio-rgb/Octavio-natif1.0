/* SPDX-License-Identifier: MPL-2.0 */
/* The 16 effects of the prototype (mkFX), fixed point, one chain PER TRACK like the prototype (TRX[t].fxs, fxqT[t]):
 * FX_NSLOT slots in the order of the track's FXT[t].ord[], each slot an effect with that track's knobs (FXT[t].p, thousandths,
 * mix last; the effective values are FEFF[t] = the knobs + the mod matrix's offsets).  The engine output goes through the
 * chain, then the track volume, then the four tracks are summed (voices.c).
 * Included from main.c AFTER ui.c (it reads fxs / FX_FIRST[] / FX_P[] and bpm_v of the same translation unit).
 * Samples are Q15 in int32 (headroom), delay lines int16 Q14 in a pool of FXPOOL_N samples per track (fx_alloc: when the
 * chosen effects want more, the long ones are shortened in proportion; a track's chain only reallocates its own pool).
 * Each effect only runs (and only touches its buffer) while its Mix > 0, like the prototype's `if(m>.001)`.
 * CPU: only slots with Mix > 0 cost anything; the budget of fxstate.c admits them (fx_run) or bypasses them (fx_off); a track whose engine is silent and whose chain has
 * been silent for FX_IDLE_BLOCKS blocks sleeps (no processing at all). */

#define FXPOOL_N 36864u                 /* samples (72 KB) of delay-line pool PER TRACK */
static int16_t fxpool[FX_NTRK][FXPOOL_N] __attribute__((aligned(4)));
static const int16_t *FV;                /* the parameters (thousandths, FX_NP of them from P_F0DRIVE) of the track being processed */
#define FVAL(id) (FV[(id) - P_F0DRIVE])

typedef struct { int16_t *b; uint32_t n, i; } dl_t;
#define FXM(a, b) ((int32_t)(((int64_t)(a) * (b)) >> 15))
#define SAT16(x) ((x) > 32767 ? 32767 : (x) < -32768 ? -32768 : (x))
#define INC_MHZ(mhz) ((uint32_t)(((uint64_t)(uint32_t)(mhz) * 6382653u) >> 16))   /* turns per sample (Q32) of a frequency in milli-Hz: 2^32 / 44100 / 1000 in Q16 */
#define PVI(id) ((int32_t)FVAL(id))                                    /* knob in thousandths: the time-critical maths below start from it, not from the Q15 knob */
#define SMP(sec_q16) ((uint32_t)(((uint64_t)(sec_q16) * FS) >> 16))

static inline void dwr(dl_t *l, int32_t x) { x = (x + 1) >> 1; l->b[l->i] = (int16_t)SAT16(x); if (++l->i == l->n) l->i = 0; }     /* stored as Q14: headroom to +-2 */
static inline int32_t drd(const dl_t *l, uint32_t d)            /* linear-interpolated read `d` samples back (Q16, 1..n-4) */
{
    uint32_t mx = (l->n - 4u) << 16;
    int32_t j, j1, b0, b1, df;
    if (d > mx) d = mx;
    j = (int32_t)l->i - (int32_t)(d >> 16);
    if (j < 0) j += (int32_t)l->n;
    j1 = j - 1;
    if (j1 < 0) j1 += (int32_t)l->n;
    df = (int32_t)((d & 0xFFFFu) >> 1);
    b0 = l->b[j]; b1 = l->b[j1];
    return (b0 + (((b1 - b0) * df) >> 15)) << 1;
}
static uint32_t fx_rng = 0x2545F491u;
static inline uint32_t rnd32(void) { fx_rng ^= fx_rng << 13; fx_rng ^= fx_rng >> 17; fx_rng ^= fx_rng << 5; return fx_rng; }
static inline int32_t hann15(uint32_t ph) { int32_t s = sin_q15(ph >> 1); return (s * s) >> 15; }     /* .5 - .5 cos(2 pi ph) */
static uint32_t fx_exp2_24(int32_t x, int outq)                /* 2^(x / 2^24) * 2^outq: exp2_fx with a 24-bit fraction and the chord's curvature taken out (LFO rates, delay times and pitch ratios need ~1e-8) */
{
    int32_t n = x >> 24, sh;
    uint32_t fr = (uint32_t)x & 0xFFFFFFu, idx = fr >> 16, t = fr & 0xFFFFu;
    uint32_t a = EXP2_TAB[idx], b = EXP2_TAB[idx + 1];
    uint32_t m = a + (uint32_t)(((int64_t)(b - a) * (int32_t)t) >> 16);
    m -= (uint32_t)((((uint64_t)a * 15741u) >> 32) * ((t * (65536u - t)) >> 16) >> 16);
    sh = n + outq - 30;
    if (sh >= 0)
        return sh > 1 ? 0xFFFFFFFFu : (m > (0xFFFFFFFFu >> sh) ? 0xFFFFFFFFu : m << sh);
    return -sh >= 32 ? 0u : m >> -sh;
}
static inline uint32_t fx_exp2(int32_t x, int outq) { return fx_exp2_24(x * 256, outq); }
#define DIV100(v) ((int32_t)(((int64_t)(v) * 42949673ll) >> 32))
#define DIV1000(v) ((int32_t)(((int64_t)(v) * 4294967ll) >> 32))      /* v / 1000 without a 64-bit divide */
static inline int32_t fxk(int id) { return (int32_t)FVAL(id) * 32768 / 1000; }           /* knob, Q15 */
static inline int32_t lfo01(uint32_t ph) { return (32768 + sin_q15(ph)) >> 1; }           /* .5 + .5 sin */

typedef struct {
    uint32_t lf[5];                      /* the prototype's lf[0..4]: phaser, flanger, chorus, tape wow, tape flutter */
    /* phaser */ int32_t px1[4], py1[4], pfb, pa; int32_t pa_c;
    /* flanger */ dl_t fl;
    /* chorus */ dl_t ch;
    /* micro pitch */ dl_t ps; uint32_t pp;
    /* delay */ dl_t dl; int32_t dlp;
    /* reverb (Q18 int32 buffers) */ struct { int32_t *b; uint32_t n, i; int32_t s; } cb[4]; struct { int32_t *b; uint32_t n, i; } ap[2];
    /* overdrive */ int32_t od;
    /* granular */ dl_t gb; int32_t gc; uint8_t gn; struct { int32_t a, n, d, dd; uint32_t ph, inc; } gr[24];
    /* feedback */ dl_t fbl; int32_t fvl;
    /* bitcrush */ int32_t bcH, bcC;
    /* ring */ uint32_t rmP;
    /* stutter */ dl_t sb; int32_t stOn, stK, stLen, stReps, stC, stIdx;
    /* reverse */ dl_t rv; uint32_t rvK;
    /* freeze */ dl_t fz; int32_t fzOn, fzI; uint32_t fzK;
    /* tape */ dl_t tp; int32_t tpL;
    /* shimmer */ dl_t tl, p2; int32_t shL; uint32_t shP;
} fxst_t;
static fxst_t XT[FX_NTRK];                /* the state of the effects of each track (an effect is in a track's chain at most once) */

/* ------------------------------------------------------------------ pool (one per track) */
static uint8_t fx_sig[FX_NTRK][FX_NSLOT];            /* the order the track's buffers were allocated for (255: none yet) */
static uint32_t fx_used[FX_NTRK];                    /* pool samples handed out */
static uint32_t fx_scale_q16[FX_NTRK];               /* the long buffers' share when the chain wants more than the pool */

static dl_t fx_take(int t, uint32_t n)
{
    dl_t l;
    if (fx_used[t] + n > FXPOOL_N) n = FXPOOL_N - fx_used[t];
    l.b = &fxpool[t][fx_used[t]]; l.n = n; l.i = 0;
    fx_used[t] += (n + 1u) & ~1u;
    return l;
}
static void fx_alloc(int t)
{
    /* wants (samples @ 44.1 kHz; the prototype's buffer lengths, trimmed where the maths never reads further back) */
    static const uint32_t FIXED[16] = { 0, 0, 449, 2213, 1772, 0, 0, 0, 1111, 0, 0, 0, 0, 0, 2648, 3532 };
    static const uint32_t LONG[16] = { 0, 0, 0, 0, 0, 55150, 0, 30878, 0, 0, 0, 26464, 44108, 26464, 0, 17644 };
    uint32_t fixed = 0, lng = 0, avail, scale;
    int q, f;
    uint32_t have = 0;
    fxst_t *s = &XT[t];
    for (q = 0; q < FX_NSLOT; q++) have |= 1u << (FXT[t].ord[q] & 15);
    for (f = 0; f < 16; f++) if (have >> f & 1u) { fixed += FIXED[f]; lng += LONG[f]; }
    if (have >> 6 & 1u) fixed += 2u * (556 + 441 + 1116 + 1188 + 1277 + 1356 + 24);       /* the reverb's Q18 buffers */
    avail = fixed < FXPOOL_N ? FXPOOL_N - fixed - 16 : 0;
    scale = lng <= avail ? 65536u : (uint32_t)(((uint64_t)avail << 16) / lng);
    fx_scale_q16[t] = scale;
    memset(s, 0, sizeof *s);
    memset(fxpool[t], 0, sizeof fxpool[t]);
    fx_used[t] = 0;
#define LN(f) ((uint32_t)(((uint64_t)LONG[f] * scale) >> 16) < 64 ? 64 : (uint32_t)(((uint64_t)LONG[f] * scale) >> 16))
    if (have & (1u << 2)) s->fl = fx_take(t, FIXED[2]);
    if (have & (1u << 3)) s->ch = fx_take(t, FIXED[3]);
    if (have & (1u << 4)) s->ps = fx_take(t, FIXED[4]);
    if (have & (1u << 5)) s->dl = fx_take(t, LN(5));
    if (have & (1u << 6)) {
        static const uint16_t CN[4] = { 1116, 1188, 1277, 1356 }, AN[2] = { 556, 441 };
        int k;
        for (k = 0; k < 4; k++) { dl_t b = fx_take(t, 2u * (CN[k] + 4u)); s->cb[k].b = (int32_t *)b.b; s->cb[k].n = CN[k] + 4u; }      /* the prototype's L(n) is n + 4 long */
        for (k = 0; k < 2; k++) { dl_t b = fx_take(t, 2u * (AN[k] + 4u)); s->ap[k].b = (int32_t *)b.b; s->ap[k].n = AN[k] + 4u; }
    }
    if (have & (1u << 7)) s->gb = fx_take(t, LN(7));
    if (have & (1u << 8)) s->fbl = fx_take(t, FIXED[8]);
    if (have & (1u << 11)) s->sb = fx_take(t, LN(11));
    if (have & (1u << 12)) s->rv = fx_take(t, LN(12));
    if (have & (1u << 13)) s->fz = fx_take(t, LN(13));
    if (have & (1u << 14)) s->tp = fx_take(t, FIXED[14]);
    if (have & (1u << 15)) { s->tl = fx_take(t, LN(15)); s->p2 = fx_take(t, 3532); }
    for (q = 0; q < FX_NSLOT; q++) fx_sig[t][q] = FXT[t].ord[q];
}

/* ------------------------------------------------------------------ effects (each: in place over n samples) */
static inline int32_t mixk(int f) { return fxk(FX_FIRST[f] + FX_P[f].n - 1); }

static void fx0(fxst_t *st, int32_t *x, int n)                              /* Overdrive */
{
    int32_t m = mixk(0), g = 32768 + 30 * fxk(P_F0DRIVE), c = 655 + (fxk(P_F0TONE) * 29491 >> 15), i;   /* .02 + tone * .9 */
    int32_t od = st->od;
    if (c > 31130) c = 31130;
    for (i = 0; i < n; i++) {
        int32_t y = x[i], a = (int32_t)(((int64_t)y * g) >> 15);
        a = a > 131071 ? 131071 : a < -131071 ? -131071 : a;
        od += FXM(FXM(c_tanh(a), 22938) - od, c);
        x[i] = y + FXM(od - y, m);
    }
    st->od = od;
}
static void fx1(fxst_t *st, int32_t *x, int n)                              /* Phaser */
{
    int32_t m = mixk(1) >> 1, fb = FXM(fxk(P_F1FB), 29491), dep = fxk(P_F1DEPTH), a = st->pa_c, i;
    uint32_t inc = INC_MHZ(100 + PVI(P_F1RATE) * 5);                   /* .1 + rate * 5 Hz */
    for (i = 0; i < n; i++) {
        int32_t y, v, s;
        st->lf[0] += inc;
        if ((i & 31) == 0) {                                    /* the allpass coefficient follows the LFO every 32 samples */
            int32_t l = lfo01(st->lf[0]), e = 65536 + (int32_t)(((int64_t)dep * l) >> 12), sn, cs;
            uint32_t f = 250u * fx_exp2(e, 8), ph;              /* Hz, Q8 */
            ph = (uint32_t)(((uint64_t)f * 48695u) >> 8);
            sn = sin_q15(ph); cs = sin_q15(ph + 0x40000000u);
            a = (int32_t)(((int64_t)(cs - sn) << 15) / (cs + sn));
            st->pa_c = a;
        }
        y = x[i];
        v = y + FXM(st->pfb, fb);
        for (s = 0; s < 4; s++) {
            int32_t o = FXM(a, v) + st->px1[s] - FXM(a, st->py1[s]);
            st->px1[s] = v; st->py1[s] = o; v = o;
        }
        st->pfb = v;
        x[i] = y + FXM(v - y, m);
    }
}

static void fx2(fxst_t *st, int32_t *x, int n)                              /* Flanger */
{
    int32_t m = mixk(2) >> 1, fb = FXM(fxk(P_F2FB), 27853), dep = fxk(P_F2DEPTH), i;
    uint32_t inc = INC_MHZ(50 + PVI(P_F2RATE) * 3);                    /* .05 + rate * 3 Hz */
    for (i = 0; i < n; i++) {
        int32_t y = x[i], l, w;
        uint32_t d;
        st->lf[1] += inc;
        l = lfo01(st->lf[1]);
        d = 2312110u + (uint32_t)(((int64_t)FXM(dep, l) * 11560550) >> 15);
        w = drd(&st->fl, d);
        dwr(&st->fl, y + FXM(w, fb));
        x[i] = y + FXM(w - y, m);
    }
}

static void fx3(fxst_t *st, int32_t *x, int n)                              /* Chorus */
{
    int32_t m = mixk(3) >> 1, i;
    uint32_t inc = INC_MHZ(200 + PVI(P_F3RATE) * 5 / 2);               /* .2 + rate * 2.5 Hz */
    uint32_t dd = (uint32_t)(((int64_t)fxk(P_F3DEPTH) * 17340826) >> 15);
    for (i = 0; i < n; i++) {
        int32_t y = x[i], a1, a2, w;
        st->lf[2] += inc;
        a1 = lfo01(st->lf[2]); a2 = lfo01(st->lf[2] + 1417339207u);
        dwr(&st->ch, y);
        w = (drd(&st->ch, 52022477u + (uint32_t)(((int64_t)dd * a1) >> 15)) + drd(&st->ch, 69363302u + (uint32_t)(((int64_t)dd * a2) >> 15))) >> 1;
        x[i] = y + FXM(w - y, m);
    }
}

static void fx4(fxst_t *st, int32_t *x, int n)                              /* Micro pitch */
{
    int32_t m = mixk(4) >> 1, i;
    uint32_t r = fx_exp2((PVI(P_F4CENTS) - 500) * 32768 / 10000, 30);      /* 2^((cents - .5) * 60 / 1200), Q30 */
    int32_t inc = (int32_t)(((int64_t)((1 << 30) - (int32_t)r) * (4294967296ull / 1323u)) >> 30);       /* (1 - r) / W turns per sample */
    for (i = 0; i < n; i++) {
        int32_t y = x[i], w;
        uint32_t q;
        dwr(&st->ps, y);
        st->pp += (uint32_t)inc;
        q = st->pp + 0x80000000u;
        w = FXM(drd(&st->ps, (uint32_t)(((uint64_t)st->pp * 1323u) >> 16) + 131072u), sin_q15(st->pp >> 1))
          + FXM(drd(&st->ps, (uint32_t)(((uint64_t)q * 1323u) >> 16) + 131072u), sin_q15(q >> 1));
        x[i] = y + FXM(w - y, m);
    }
}
static void fx5(fxst_t *st, int32_t *x, int n)                              /* Delay */
{
    int32_t m = mixk(5), fb = FXM(fxk(P_F5FB), 29491), i;
    uint32_t d;
    uint64_t dq;
    if (FVAL(P_F5SYNC)) {
        static const uint32_t R[14] = { 5461, 8192, 12288, 10923, 16384, 24576, 21845, 32768, 49152, 43691, 65536, 98304, 87381, 131072 };   /* note / quarter, Q16 */
        int k = FVAL(P_F5TIME) * 14 / 1000;
        dq = (uint64_t)R[k > 13 ? 13 : k] * FS * 60u / (uint32_t)bpm_v;                            /* Q16 samples */
    } else
        dq = (uint64_t)fx_exp2_24(DIV1000((int64_t)PVI(P_F5TIME) * 89287137), 16) * 1323u;    /* .03 * 40^time s: 40^time = 2^(time * 5.3219) */
    d = dq > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)dq;
    if (d < 131072u) d = 131072u;
    for (i = 0; i < n; i++) {
        int32_t y = x[i], w = drd(&st->dl, d);
        st->dlp += FXM(w - st->dlp, 19661);
        dwr(&st->dl, y + FXM(st->dlp, fb));
        x[i] = y + FXM(w, m);
    }
}

static void fx6(fxst_t *st, int32_t *x, int n)                              /* Reverb: 4 combs + 2 allpass (Q18 internally) */
{
    int32_t m = mixk(6), fk = 22938 + FXM(fxk(P_F6SIZE), 9175), dm = 3277 + FXM(fxk(P_F6DAMP), 22938), i;     /* .7 + size * .28 ; .1 + damp * .7 */
    for (i = 0; i < n; i++) {
        int32_t y = x[i], s = 0, k;
        int32_t inp = (int32_t)(((int64_t)y * 491) >> 12);  /* y * .015 * 8 (Q18: the buffers keep 3 bits more than the prototype's float needs) */
        for (k = 0; k < 4; k++) {
            int32_t o = st->cb[k].b[st->cb[k].i];
            st->cb[k].s = o - FXM(o, dm) + FXM(st->cb[k].s, dm);
            st->cb[k].b[st->cb[k].i] = inp + FXM(st->cb[k].s, fk);
            if (++st->cb[k].i == st->cb[k].n) st->cb[k].i = 0;
            s += o;
        }
        for (k = 0; k < 2; k++) {
            int32_t o = st->ap[k].b[st->ap[k].i], v = o - s;
            st->ap[k].b[st->ap[k].i] = s + (o >> 1);
            if (++st->ap[k].i == st->ap[k].n) st->ap[k].i = 0;
            s = v;
        }
        x[i] = y + (int32_t)(((int64_t)s * m * 3) >> 18);   /* s Q18 -> Q15, * 3 * mix */
    }
}
static void fx7(fxst_t *st, int32_t *x, int n)                              /* Granular */
{
    int32_t m = mixk(7), size = fxk(P_F7SIZE), dens = fxk(P_F7DENSITY), pit = fxk(P_F7PITCH), i;
    int32_t gl = 441 + (int32_t)(((int64_t)size * 8379) >> 15);                           /* (.01 + size * .19) * sr */
    int32_t iv = (int32_t)((uint64_t)FS * 32768u / (uint32_t)(2 * 32768 + 60 * dens));    /* sr / (2 + density * 60) */
    int32_t ratio = gl * 256 / iv, sc = ratio <= 256 ? 32768 : (int32_t)(32768ull * 16u / isqrt32((uint32_t)ratio));   /* 1 / sqrt(max(1, n / iv)) */
    int32_t dmax = (int32_t)st->gb.n - 6, mo = FXM(m, 45875), mi = FXM(m, 19661);
    uint32_t dspan;
    if (dmax > (int32_t)(1.1 * FS)) dmax = (int32_t)(1.1 * FS);
    dspan = 22050u;                                                                       /* where a grain starts: .02 .. .52 s back; a shortened buffer (the chain shares its pool) keeps the same density with a narrower spread */
    if (dmax < 882 + 1600 + 22050) dspan = dmax > 882 + 1600 ? (uint32_t)(dmax - 882 - 1600) : 0u;
    for (i = 0; i < n; i++) {
        int32_t y = x[i], o = 0, g;
        dwr(&st->gb, y);
        if (--st->gc <= 0) {
            st->gc += (int32_t)(((int64_t)iv * (16384 + (int32_t)(rnd32() >> 17))) >> 15);          /* iv * (.5 + random) */
            if (st->gn < 24) {
                int32_t e = ((pit - 16384) * 96 + (((int32_t)(rnd32() >> 17) - 16384) * 3) / 5) / 12;   /* octaves, Q16: ((pitch - .5) * 48 + (random - .5) * .3) / 12 */
                uint32_t r = fx_exp2(e, 16);
                int32_t k = st->gn++;
                st->gr[k].a = 0; st->gr[k].n = gl; st->gr[k].inc = (uint32_t)(4294967296ull / (uint32_t)gl); st->gr[k].ph = 0;
                st->gr[k].d = (882 + (int32_t)(((uint32_t)(rnd32() >> 17) * dspan) >> 15)) << 16;     /* (.02 + random * .5) * sr */
                st->gr[k].dd = 65536 - (int32_t)r;
            }
        }
        for (g = (int32_t)st->gn - 1; g >= 0; g--) {
            int32_t d = st->gr[g].d >> 16;
            o += FXM(drd(&st->gb, (uint32_t)st->gr[g].d), hann15(st->gr[g].ph));
            st->gr[g].ph += st->gr[g].inc; st->gr[g].a++; st->gr[g].d += st->gr[g].dd;
            if (st->gr[g].a >= st->gr[g].n || d < 2 || d > dmax) st->gr[g] = st->gr[--st->gn];
        }
        x[i] = y - FXM(y, mi) + FXM(FXM(o, sc), mo);
    }
}

static void fx8(fxst_t *st, int32_t *x, int n)                              /* Feedback */
{
    int32_t m = mixk(8), tone = 3277 + FXM(fxk(P_F8TONE), 27853), fbk = 16384 + FXM(fxk(P_F8FB), 16056), mo = FXM(m, 39322), i;
    uint32_t D = (uint32_t)(((uint64_t)fx_exp2(-(PVI(P_F8FREQ) * 327680 / 1000), 16) * 2205u) >> 1);        /* sr / (40 * 2^(5 freq)), Q16 samples */
    if (D < 131072u) D = 131072u;
    for (i = 0; i < n; i++) {
        int32_t y = x[i], v = drd(&st->fbl, D);
        st->fvl += FXM(v - st->fvl, tone);
        dwr(&st->fbl, c_tanh(FXM(y, 19661) + FXM(st->fvl, fbk)));
        x[i] = y - FXM(y, m >> 1) + FXM(st->fvl, mo);
    }
}

static void fx9(fxst_t *st, int32_t *x, int n)                              /* Bitcrush */
{
    int32_t m = mixk(9), i;
    uint32_t step = fx_exp2(3 * 65536 + PVI(P_F9CRUSH) * 655360 / 1000, 0);       /* 2^(15 - (12 - 10 crush)) = 2^(3 + 10 crush): the step in Q15 */
    uint32_t r = 65536u + (uint32_t)fxk(P_F9RATE) * 80;               /* 1 + rate * 40, Q16 */
    uint32_t inv;
    if (step < 1) step = 1;
    inv = 0xFFFFFFFFu / step;
    for (i = 0; i < n; i++) {
        int32_t y = x[i];
        st->bcC += 65536;
        if ((uint32_t)st->bcC >= r) {
            int32_t t = y + (int32_t)(step >> 1);
            st->bcC -= (int32_t)r;
            if (t >= 0) st->bcH = (int32_t)(((uint64_t)(uint32_t)t * inv) >> 32) * (int32_t)step;
            else st->bcH = -(int32_t)(((uint64_t)(uint32_t)(-t + (int32_t)step - 1) * inv) >> 32) * (int32_t)step;
        }
        x[i] = y + FXM(st->bcH - y, m);
    }
}

static void fx10(fxst_t *st, int32_t *x, int n)                             /* Ring mod */
{
    int32_t m = mixk(10), sh = fxk(P_F10SHAPE), mo = FXM(m, 39322), i;
    uint32_t inc = (uint32_t)(((uint64_t)fx_exp2_24(DIV1000((int64_t)PVI(P_F10FREQ) * 117440512), 16) * 20u * 6382653000ull) >> 32);   /* 20 * 2^(7 freq) Hz */
    for (i = 0; i < n; i++) {
        int32_t y = x[i], s, c;
        st->rmP += inc;
        s = sin_q15(st->rmP); c = FXM(s, 32768 - sh) + (s > 0 ? sh : -sh);
        x[i] = y - FXM(y, m) + FXM(FXM(y, c), mo);
    }
}

static void fx11(fxst_t *st, int32_t *x, int n)                             /* Stutter */
{
    int32_t m = mixk(11), sl = (int32_t)(((int64_t)(983 + FXM(fxk(P_F11LEN), 8847)) * FS) >> 15), i;       /* (.03 + len * .27) * sr */
    int32_t chance = FXM(fxk(P_F11CHANCE), 22938);
    int32_t mx = (int32_t)st->sb.n - 2;
    if (sl > mx) sl = mx;
    for (i = 0; i < n; i++) {
        int32_t y = x[i];
        if (!st->stOn) dwr(&st->sb, y);
        if (++st->stC >= sl) {
            st->stC = 0;
            if (st->stOn) { if (--st->stReps <= 0) st->stOn = 0; }
            else if ((int32_t)(rnd32() >> 17) < chance) {
                st->stOn = 1; st->stReps = 1 + (int32_t)((rnd32() >> 16) % 5u);
                st->stLen = (rnd32() >> 31) ? sl : sl >> 1;
                if (st->stLen < 8) st->stLen = 8;
                st->stK = 0; st->stIdx = (int32_t)st->sb.i;
            }
        }
        if (st->stOn) {
            int32_t p = (st->stIdx - st->stLen + (st->stK % st->stLen)) % (int32_t)st->sb.n;
            if (p < 0) p += (int32_t)st->sb.n;
            y += FXM((st->sb.b[p] << 1) - y, m);
            st->stK++;
        }
        x[i] = y;
    }
}
static void fx12(fxst_t *st, int32_t *x, int n)                             /* Reverse */
{
    int32_t m = mixk(12), fb = FXM(fxk(P_F12FB), 26214), mo = FXM(m, 29491), mh = m >> 1, i;
    uint32_t T = (uint32_t)DIV100((int64_t)(1000 + 9 * PVI(P_F12TIME)) * 112896), mx = (st->rv.n - 6u) << 8;     /* (.1 + time * .9) * sr, Q8 samples */
    uint32_t inc;
    if (T > mx) T = mx;
    if (T < 64 * 256) T = 64 * 256;
    inc = (uint32_t)((1ull << 41) / T);                         /* 1 / C, C = T / 2, turns per sample */
    for (i = 0; i < n; i++) {
        int32_t y = x[i], w = 0, t;
        st->rvK += inc;
        for (t = 0; t < 2; t++) {
            uint32_t ph = st->rvK + (t ? 0x80000000u : 0);
            w += FXM(drd(&st->rv, (uint32_t)(((uint64_t)ph * T) >> 24) + 131072u), hann15(ph));
        }
        dwr(&st->rv, y + FXM(w, fb));
        x[i] = y - FXM(y, mh) + FXM(w, mo);
    }
}
static void fx13(fxst_t *st, int32_t *x, int n)                             /* Freeze */
{
    int32_t m = mixk(13), i;
    uint32_t Lf = (uint32_t)DIV100((int64_t)(200 + 4 * PVI(P_F13SIZE)) * 112896), inc;        /* (.02 + size * .4) * sr, Q8 samples */
    uint32_t mx = (st->fz.n - 6u) << 8;
    if (Lf > mx) Lf = mx;
    if (Lf < 32 * 256) Lf = 32 * 256;
    inc = (uint32_t)((1ull << 40) / Lf);
    if (FVAL(P_F13HOLD) < 500) {
        for (i = 0; i < n; i++) dwr(&st->fz, x[i]);
        st->fzOn = 0;
        return;
    }
    if (!st->fzOn) { st->fzOn = 1; st->fzI = (int32_t)st->fz.i; st->fzK = 0; }
    for (i = 0; i < n; i++) {
        int32_t y = x[i], w = 0, t;
        st->fzK += inc;
        for (t = 0; t < 2; t++) {
            uint32_t ph = st->fzK + (t ? 0x80000000u : 0);
            /* p = fzI - Lf + ph * Lf (samples, Q16); the same interpolation as rd() but measured from the frozen index */
            int64_t pq = ((int64_t)st->fzI << 16) - ((int64_t)Lf << 8) + (int64_t)(((uint64_t)ph * Lf) >> 24);
            int64_t nq = (int64_t)st->fz.n << 16;
            int32_t a, f, b0, b1;
            if (pq < 0) pq += nq;
            if (pq >= nq) pq -= nq;
            a = (int32_t)(pq >> 16); f = (int32_t)((pq & 0xFFFF) >> 1);
            b0 = st->fz.b[a]; b1 = st->fz.b[a + 1 == (int32_t)st->fz.n ? 0 : a + 1];
            w += FXM((b0 + (((b1 - b0) * f) >> 15)) << 1, hann15(ph));
        }
        x[i] = y + FXM(w - y, m);
    }
}
static void fx14(fxst_t *st, int32_t *x, int n)                             /* Tape */
{
    int32_t m = mixk(14), wow = fxk(P_F14WOW), drv = fxk(P_F14DRIVE), age = fxk(P_F14AGE), i;
    int32_t g = 32768 + (drv << 2), inv = (int32_t)((1ull << 30) / (uint32_t)(32768 + (drv * 3 >> 1))), c = 1311 + FXM(32768 - age, 29491);   /* .04 + (1 - age) * .9 */
    uint32_t i3 = INC_MHZ(600), i4 = INC_MHZ(7300);                /* .6 Hz, 7.3 Hz */
    for (i = 0; i < n; i++) {
        int32_t y = x[i], s, a;
        uint32_t d;
        st->lf[3] += i3; st->lf[4] += i4;
        d = 34681651u + (uint32_t)(((int64_t)FXM(sin_q15(st->lf[3]), wow) * 17340000) >> 15) + (uint32_t)(((int64_t)FXM(sin_q15(st->lf[4]), wow) * 1734000) >> 15);
        dwr(&st->tp, y);
        s = drd(&st->tp, d);
        a = (int32_t)(((int64_t)s * g) >> 15);
        a = a > 131071 ? 131071 : a < -131071 ? -131071 : a;
        s = FXM(c_tanh(a), inv);
        st->tpL += FXM(s - st->tpL, c);
        x[i] = y + FXM(st->tpL - y, m);
    }
}
static void fx15(fxst_t *st, int32_t *x, int n)                             /* Shimmer */
{
    int32_t m = mixk(15), dec = 11469 + FXM(fxk(P_F15DECAY), 18022), i;                  /* .35 + decay * .55 */
    uint32_t r = fx_exp2(PVI(P_F15SHIFT) * 131072 / 1000, 28);             /* 2^(shift * 2), Q28 */
    int32_t inc = (int32_t)(((int64_t)((1 << 28) - (int32_t)r) * (4294967296ull / 2205u)) >> 28);
    int32_t mo = FXM(m, 26214), mi = FXM(m, 13107);
    const uint32_t d1 = (uint32_t)(FS * 21ull / 100) << 16, d2 = (uint32_t)(FS * 337ull * 65536u / 1000);
    for (i = 0; i < n; i++) {
        int32_t y = x[i], v, sh;
        uint32_t q;
        v = drd(&st->tl, d1) + FXM(drd(&st->tl, d2), 22938);
        st->shL += FXM(v - st->shL, 14746);
        dwr(&st->p2, st->shL);
        st->shP += (uint32_t)inc;
        q = st->shP + 0x80000000u;
        sh = FXM(drd(&st->p2, (uint32_t)(((uint64_t)st->shP * 2205u) >> 16) + 131072u), sin_q15(st->shP >> 1))
           + FXM(drd(&st->p2, (uint32_t)(((uint64_t)q * 2205u) >> 16) + 131072u), sin_q15(q >> 1));
        dwr(&st->tl, c_tanh((y >> 1) + FXM(sh, dec)));
        x[i] = y - FXM(y, mi) + FXM(st->shL + sh, mo);
    }
}
/* ------------------------------------------------------------------ the bus */
typedef void (*fxfn_t)(fxst_t *, int32_t *, int);
static const fxfn_t FXF[16] = { fx0, fx1, fx2, fx3, fx4, fx5, fx6, fx7, fx8, fx9, fx10, fx11, fx12, fx13, fx14, fx15 };

/* perf.fx_max / fx_last: timer ticks spent in the four chains of one audio block (worst, last) */
static int16_t FEFF[FX_NTRK][FX_NP];                    /* effective parameters: knobs + mod offsets, rebuilt each block */
static uint8_t fx_ready[FX_NTRK], fx_any[FX_NTRK];
static uint16_t fx_idle[FX_NTRK];                       /* consecutive silent blocks (engine off, chain output ~ 0) */
#define FX_IDLE_BLOCKS 720                              /* 2.1 s: longer than the longest delay line (1.25 s) */
static uint32_t fx_blk_ticks;

#ifdef FX_TEST                                          /* test builds: -DFX_TEST=<effect> -DFX_TP="{k0,k1,..,mix}" (thousandths), -DFX_TS=1 for the delay's sync */
static void fx_test_init(void)                          /* track 1's chain: the effect under test first */
{
    static const int16_t tp[] = FX_TP;
    int i, j = 1;
    fxs[0] = FX_TEST;
    for (i = 0; i < 16; i++) if (i != FX_TEST && j < FX_NSLOT) fxs[j++] = (uint8_t)i;
    for (i = 0; i < (int)(sizeof tp / sizeof tp[0]); i++) PV[FX_FIRST[FX_TEST] + i] = tp[i];
#ifdef FX_TS
    PV[P_F5SYNC] = 1;
#endif
}
int32_t fx_dry_tap[128], fx_wet_tap[128];                                 /* test builds: track 1's chain before / after the effects, written to the right / left channel */
#endif

#ifndef BENCH_ACTIVE
#define BENCH_ACTIVE 15                          /* bit s: slot s has Mix > 0 */
#endif
#ifndef BENCH_KNOB
#define BENCH_KNOB 600
#endif
#ifndef BENCH_MIX
#define BENCH_MIX 1000
#endif
#ifdef BENCH                                            /* worst-case bench: every track plays and has the chain BENCH_FX (see NOTES-H) */
static void fx_bench_init(void)
{
    static const uint8_t ord[FX_NTRK][FX_NSLOT] = BENCH_FX;
    int t, s;
    for (t = 0; t < FX_NTRK; t++) {
        for (s = 0; s < FX_NSLOT; s++) {
            int f = ord[t][s], k;
            FXT[t].ord[s] = (uint8_t)f;
            for (k = 0; k < FX_P[f].n; k++) { int id = FX_FIRST[f] + k - P_F0DRIVE; FXT[t].p[id] = (FX_P[f].l[k] == L_MIX || FX_P[f].l[k] == L_HOLD) ? ((BENCH_ACTIVE >> s & 1) ? BENCH_MIX : 0) : BENCH_KNOB; }
        }
    }
    memcpy(&PV[P_F0DRIVE], FXT[fx_view].p, sizeof FXT[0].p);
    PV[P_F5SYNC] = 0;
}
#endif

void fx_begin(void)                                     /* once per audio block, before mod_apply: the knobs -> FEFF, buffers follow the orders */
{
    int t;
#ifdef FX_TEST
    static uint8_t once;
    if (!once) { once = 1; fx_test_init(); }
#endif
#ifdef BENCH
    { static uint8_t once_b; if (!once_b) { once_b = 1; fx_bench_init(); } }
#endif
    fx_commit();
    fx_blk_ticks = 0;
    for (t = 0; t < FX_NTRK; t++) {
        memcpy(FEFF[t], FXT[t].p, sizeof FEFF[t]);
        if (!fx_ready[t] || memcmp(fx_sig[t], FXT[t].ord, FX_NSLOT)) { fx_alloc(t); fx_ready[t] = 1; fx_idle[t] = 0; fx_run[t] = 0; }
    }
}
void fx_admit(void)                                     /* once per audio block, after mod_apply: which active slots (Mix > 0, modulated) the CPU budget lets run */
{
    int t, q;
    {                                                   /* CPU budget: an active slot (Mix > 0) runs once admitted; the ones already running keep their place, new ones are admitted in track / slot order while they fit */
        uint32_t used = 0;
        for (t = 0; t < FX_NTRK; t++)
            for (q = 0; q < FX_NSLOT; q++) {
                int f = FXT[t].ord[q];
                if (FEFF[t][FX_FIRST[f] + FX_P[f].n - 1 - P_F0DRIVE] > 0) { if (fx_run[t] >> q & 1) used += FX_COST[f]; }
                else { fx_run[t] &= (uint8_t)~(1u << q); fx_off[t] &= (uint8_t)~(1u << q); }
            }
        for (t = 0; t < FX_NTRK; t++)
            for (q = 0; q < FX_NSLOT; q++) {
                int f = FXT[t].ord[q];
                if (FEFF[t][FX_FIRST[f] + FX_P[f].n - 1 - P_F0DRIVE] <= 0 || (fx_run[t] >> q & 1)) continue;
                if (used + FX_COST[f] <= FX_BUDGET) { used += FX_COST[f]; fx_run[t] |= (uint8_t)(1u << q); fx_off[t] &= (uint8_t)~(1u << q); }
                else { if (!(fx_off[t] >> q & 1)) fx_refused++; fx_off[t] |= (uint8_t)(1u << q); }
            }
    }
    for (t = 0; t < FX_NTRK; t++) {
        int any = 0;
        for (q = 0; q < FX_NSLOT; q++) if (fx_run[t] >> q & 1) any = 1;
        fx_any[t] = (uint8_t)any;
        if (!any) fx_idle[t] = FX_IDLE_BLOCKS;
    }
}
void fx_mod(int k, int32_t xq)                          /* the mod matrix, destination knob k (0..FX_NP-2), offset xq (Q15 of the knob's range): on every track's own value */
{
    const pdesc_t *d = &PD[P_F0DRIVE + k];
    int mn = d->min, mx = d->max, t;
    if (mx == mn) return;
    for (t = 0; t < FX_NTRK; t++) {
        int32_t nn = (int32_t)(((int64_t)(FEFF[t][k] - mn) << 15) / (mx - mn)) + xq;
        nn = nn < 0 ? 0 : nn > 32768 ? 32768 : nn;
        FEFF[t][k] = (int16_t)(mn + (int)(((int64_t)nn * (mx - mn)) >> 15));
    }
}
int fx_awake(int t)
{
#ifdef FX_TEST
    if (t == 0) return 1;
#endif
    return fx_any[t] && fx_idle[t] < FX_IDLE_BLOCKS;      /* 0: nothing to render for a silent track */
}

void fx_track(int t, int32_t *x, int n, int act)        /* the chain of track t, in place over n samples; act: the engine gave a block (else x is zeros) */
{
    int q, i;
    uint32_t t0;
    fxst_t *s = &XT[t];
#ifdef FX_TEST
    if (t == 0) {
#ifdef FX_SINE
        static uint32_t sph;
        for (i = 0; i < n; i++) { x[i] = (sin_q15(sph) * FX_SINE) >> 10; sph += INC_MHZ(441000); }     /* test: a sine of FX_SINE/1024 instead of the voices */
#endif
        for (i = 0; i < n; i++) fx_dry_tap[i] = x[i];
    }
#endif
    if (!fx_any[t]) {
#ifdef FX_TEST
        if (t == 0) for (i = 0; i < n; i++) fx_wet_tap[i] = x[i];
#endif
        return;
    }
    t0 = fm1_ticks();
    FV = FEFF[t];
    for (q = 0; q < FX_NSLOT; q++) {
        int f = FXT[t].ord[q];
        if ((fx_run[t] >> q & 1) && FEFF[t][FX_FIRST[f] + FX_P[f].n - 1 - P_F0DRIVE] > 0) FXF[f](s, x, n);
    }
    if (act) fx_idle[t] = 0;
    else {
        int32_t pk = 0;
        for (i = 0; i < n; i++) { int32_t a = x[i] < 0 ? -x[i] : x[i]; if (a > pk) pk = a; }
        if (pk <= 2) { if (fx_idle[t] < FX_IDLE_BLOCKS) fx_idle[t]++; } else fx_idle[t] = 0;
    }
#ifdef FX_TEST
    if (t == 0) for (i = 0; i < n; i++) fx_wet_tap[i] = x[i];
#endif
    fx_blk_ticks += fm1_ticks() - t0;
    fx_ticks_last = fx_blk_ticks;
    if (fx_ticks_last > fx_ticks_max) fx_ticks_max = fx_ticks_last;
}
