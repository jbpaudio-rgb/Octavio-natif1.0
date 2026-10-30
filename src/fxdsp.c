/* SPDX-License-Identifier: MPL-2.0 */
/* The 16 effects of the prototype (mkFX), fixed point, as one bus: the six slots in the order the FX ORDER page sets
 * (fxs[]), each slot an effect with the knobs of the FX page (PV[FX_FIRST[fx] + i], thousandths, mix last).
 * Included from main.c AFTER ui.c (it reads fxs[], FX_FIRST[] and bpm_v of the same translation unit).
 * Samples are Q15 in int32 (headroom), delay lines int16 Q15 in one shared pool (see fx_alloc: the six slots share
 * FXPOOL_N samples; when the chosen effects want more, the long ones are shortened in proportion).
 * Each effect only runs (and only touches its buffer) while its Mix > 0, like the prototype's `if(m>.001)`.
 * The prototype has one FX chain per track; the native FX page edits one parameter set, so the chain runs once on
 * the sum of the four tracks (before the master tanh). */

#define FXPOOL_N 73728u
static int16_t fxpool[FXPOOL_N] __attribute__((aligned(4)));

typedef struct { int16_t *b; uint32_t n, i; } dl_t;
#define FXM(a, b) ((int32_t)(((int64_t)(a) * (b)) >> 15))
#define SAT16(x) ((x) > 32767 ? 32767 : (x) < -32768 ? -32768 : (x))
#define INC_MHZ(mhz) ((uint32_t)(((uint64_t)(uint32_t)(mhz) * 6382653u) >> 16))   /* turns per sample (Q32) of a frequency in milli-Hz: 2^32 / 44100 / 1000 in Q16 */
#define PVI(id) ((int32_t)PV[id])                                    /* knob in thousandths: the time-critical maths below start from it, not from the Q15 knob */
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
static inline int32_t fxk(int id) { return (int32_t)PV[id] * 32768 / 1000; }           /* knob, Q15 */
static inline int32_t lfo01(uint32_t ph) { return (32768 + sin_q15(ph)) >> 1; }           /* .5 + .5 sin */

static struct {
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
} X;

/* ------------------------------------------------------------------ pool */
static uint8_t fx_sig[6] = { 255, 255, 255, 255, 255, 255 };
static uint32_t fx_used;                 /* pool samples handed out */
static uint32_t fx_scale_q16 = 65536;    /* the long buffers' share when the six effects want more than the pool */

static dl_t fx_take(uint32_t n)
{
    dl_t l;
    if (fx_used + n > FXPOOL_N) n = FXPOOL_N - fx_used;
    l.b = &fxpool[fx_used]; l.n = n; l.i = 0;
    fx_used += (n + 1u) & ~1u;
    return l;
}
static void fx_alloc(void)
{
    /* wants (samples @ 44.1 kHz; the prototype's buffer lengths, trimmed where the maths never reads further back) */
    static const uint32_t FIXED[16] = { 0, 0, 449, 2213, 1772, 0, 0, 0, 1111, 0, 0, 0, 0, 0, 2648, 3532 };
    static const uint32_t LONG[16] = { 0, 0, 0, 0, 0, 55150, 0, 30878, 0, 0, 0, 26464, 44108, 26464, 0, 17644 };
    uint32_t fixed = 0, lng = 0, avail;
    int q, f;
    uint32_t have = 0;
    for (q = 0; q < 6; q++) have |= 1u << fxs[q];
    for (f = 0; f < 16; f++) if (have >> f & 1u) { fixed += FIXED[f]; lng += LONG[f]; }
    if (have >> 6 & 1u) fixed += 2u * (556 + 441 + 1116 + 1188 + 1277 + 1356 + 24);       /* the reverb's Q18 buffers */
    avail = fixed < FXPOOL_N ? FXPOOL_N - fixed - 16 : 0;
    fx_scale_q16 = lng <= avail ? 65536u : (uint32_t)(((uint64_t)avail << 16) / lng);
    memset(&X, 0, sizeof X);
    memset(fxpool, 0, sizeof fxpool);
    fx_used = 0;
#define LN(f) ((uint32_t)(((uint64_t)LONG[f] * fx_scale_q16) >> 16) < 64 ? 64 : (uint32_t)(((uint64_t)LONG[f] * fx_scale_q16) >> 16))
    if (have & (1u << 2)) X.fl = fx_take(FIXED[2]);
    if (have & (1u << 3)) X.ch = fx_take(FIXED[3]);
    if (have & (1u << 4)) X.ps = fx_take(FIXED[4]);
    if (have & (1u << 5)) X.dl = fx_take(LN(5));
    if (have & (1u << 6)) {
        static const uint16_t CN[4] = { 1116, 1188, 1277, 1356 }, AN[2] = { 556, 441 };
        int k;
        for (k = 0; k < 4; k++) { dl_t t = fx_take(2u * (CN[k] + 4u)); X.cb[k].b = (int32_t *)t.b; X.cb[k].n = CN[k] + 4u; }      /* the prototype's L(n) is n + 4 long */
        for (k = 0; k < 2; k++) { dl_t t = fx_take(2u * (AN[k] + 4u)); X.ap[k].b = (int32_t *)t.b; X.ap[k].n = AN[k] + 4u; }
    }
    if (have & (1u << 7)) X.gb = fx_take(LN(7));
    if (have & (1u << 8)) X.fbl = fx_take(FIXED[8]);
    if (have & (1u << 11)) X.sb = fx_take(LN(11));
    if (have & (1u << 12)) X.rv = fx_take(LN(12));
    if (have & (1u << 13)) X.fz = fx_take(LN(13));
    if (have & (1u << 14)) X.tp = fx_take(FIXED[14]);
    if (have & (1u << 15)) { X.tl = fx_take(LN(15)); X.p2 = fx_take(3532); }
    for (q = 0; q < 6; q++) fx_sig[q] = fxs[q];
}

/* ------------------------------------------------------------------ effects (each: in place over n samples) */
static inline int32_t mixk(int f) { return fxk(FX_FIRST[f] + FX_P[f].n - 1); }

static void fx0(int32_t *x, int n)                              /* Overdrive */
{
    int32_t m = mixk(0), g = 32768 + 30 * fxk(P_F0DRIVE), c = 655 + (fxk(P_F0TONE) * 29491 >> 15), i;   /* .02 + tone * .9 */
    int32_t od = X.od;
    if (c > 31130) c = 31130;
    for (i = 0; i < n; i++) {
        int32_t y = x[i], a = (int32_t)(((int64_t)y * g) >> 15);
        a = a > 131071 ? 131071 : a < -131071 ? -131071 : a;
        od += FXM(FXM(c_tanh(a), 22938) - od, c);
        x[i] = y + FXM(od - y, m);
    }
    X.od = od;
}
static void fx1(int32_t *x, int n)                              /* Phaser */
{
    int32_t m = mixk(1) >> 1, fb = FXM(fxk(P_F1FB), 29491), dep = fxk(P_F1DEPTH), a = X.pa_c, i;
    uint32_t inc = INC_MHZ(100 + PVI(P_F1RATE) * 5);                   /* .1 + rate * 5 Hz */
    for (i = 0; i < n; i++) {
        int32_t y, v, s;
        X.lf[0] += inc;
        if ((i & 31) == 0) {                                    /* the allpass coefficient follows the LFO every 32 samples */
            int32_t l = lfo01(X.lf[0]), e = 65536 + (int32_t)(((int64_t)dep * l) >> 12), sn, cs;
            uint32_t f = 250u * fx_exp2(e, 8), ph;              /* Hz, Q8 */
            ph = (uint32_t)(((uint64_t)f * 48695u) >> 8);
            sn = sin_q15(ph); cs = sin_q15(ph + 0x40000000u);
            a = (int32_t)(((int64_t)(cs - sn) << 15) / (cs + sn));
            X.pa_c = a;
        }
        y = x[i];
        v = y + FXM(X.pfb, fb);
        for (s = 0; s < 4; s++) {
            int32_t o = FXM(a, v) + X.px1[s] - FXM(a, X.py1[s]);
            X.px1[s] = v; X.py1[s] = o; v = o;
        }
        X.pfb = v;
        x[i] = y + FXM(v - y, m);
    }
}

static void fx2(int32_t *x, int n)                              /* Flanger */
{
    int32_t m = mixk(2) >> 1, fb = FXM(fxk(P_F2FB), 27853), dep = fxk(P_F2DEPTH), i;
    uint32_t inc = INC_MHZ(50 + PVI(P_F2RATE) * 3);                    /* .05 + rate * 3 Hz */
    for (i = 0; i < n; i++) {
        int32_t y = x[i], l, w;
        uint32_t d;
        X.lf[1] += inc;
        l = lfo01(X.lf[1]);
        d = 2312110u + (uint32_t)(((int64_t)FXM(dep, l) * 11560550) >> 15);
        w = drd(&X.fl, d);
        dwr(&X.fl, y + FXM(w, fb));
        x[i] = y + FXM(w - y, m);
    }
}

static void fx3(int32_t *x, int n)                              /* Chorus */
{
    int32_t m = mixk(3) >> 1, i;
    uint32_t inc = INC_MHZ(200 + PVI(P_F3RATE) * 5 / 2);               /* .2 + rate * 2.5 Hz */
    uint32_t dd = (uint32_t)(((int64_t)fxk(P_F3DEPTH) * 17340826) >> 15);
    for (i = 0; i < n; i++) {
        int32_t y = x[i], a1, a2, w;
        X.lf[2] += inc;
        a1 = lfo01(X.lf[2]); a2 = lfo01(X.lf[2] + 1417339207u);
        dwr(&X.ch, y);
        w = (drd(&X.ch, 52022477u + (uint32_t)(((int64_t)dd * a1) >> 15)) + drd(&X.ch, 69363302u + (uint32_t)(((int64_t)dd * a2) >> 15))) >> 1;
        x[i] = y + FXM(w - y, m);
    }
}

static void fx4(int32_t *x, int n)                              /* Micro pitch */
{
    int32_t m = mixk(4) >> 1, i;
    uint32_t r = fx_exp2((PVI(P_F4CENTS) - 500) * 32768 / 10000, 30);      /* 2^((cents - .5) * 60 / 1200), Q30 */
    int32_t inc = (int32_t)(((int64_t)((1 << 30) - (int32_t)r) * (4294967296ull / 1323u)) >> 30);       /* (1 - r) / W turns per sample */
    for (i = 0; i < n; i++) {
        int32_t y = x[i], w;
        uint32_t q;
        dwr(&X.ps, y);
        X.pp += (uint32_t)inc;
        q = X.pp + 0x80000000u;
        w = FXM(drd(&X.ps, (uint32_t)(((uint64_t)X.pp * 1323u) >> 16) + 131072u), sin_q15(X.pp >> 1))
          + FXM(drd(&X.ps, (uint32_t)(((uint64_t)q * 1323u) >> 16) + 131072u), sin_q15(q >> 1));
        x[i] = y + FXM(w - y, m);
    }
}
static void fx5(int32_t *x, int n)                              /* Delay */
{
    int32_t m = mixk(5), fb = FXM(fxk(P_F5FB), 29491), i;
    uint32_t d;
    uint64_t dq;
    if (PV[P_F5SYNC]) {
        static const uint32_t R[14] = { 5461, 8192, 12288, 10923, 16384, 24576, 21845, 32768, 49152, 43691, 65536, 98304, 87381, 131072 };   /* note / quarter, Q16 */
        int k = PV[P_F5TIME] * 14 / 1000;
        dq = (uint64_t)R[k > 13 ? 13 : k] * FS * 60u / (uint32_t)bpm_v;                            /* Q16 samples */
    } else
        dq = (uint64_t)fx_exp2_24(DIV1000((int64_t)PVI(P_F5TIME) * 89287137), 16) * 1323u;    /* .03 * 40^time s: 40^time = 2^(time * 5.3219) */
    d = dq > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)dq;
    if (d < 131072u) d = 131072u;
    for (i = 0; i < n; i++) {
        int32_t y = x[i], w = drd(&X.dl, d);
        X.dlp += FXM(w - X.dlp, 19661);
        dwr(&X.dl, y + FXM(X.dlp, fb));
        x[i] = y + FXM(w, m);
    }
}

static void fx6(int32_t *x, int n)                              /* Reverb: 4 combs + 2 allpass (Q18 internally) */
{
    int32_t m = mixk(6), fk = 22938 + FXM(fxk(P_F6SIZE), 9175), dm = 3277 + FXM(fxk(P_F6DAMP), 22938), i;     /* .7 + size * .28 ; .1 + damp * .7 */
    for (i = 0; i < n; i++) {
        int32_t y = x[i], s = 0, k;
        int32_t inp = (int32_t)(((int64_t)y * 491) >> 12);  /* y * .015 * 8 (Q18: the buffers keep 3 bits more than the prototype's float needs) */
        for (k = 0; k < 4; k++) {
            int32_t o = X.cb[k].b[X.cb[k].i];
            X.cb[k].s = o - FXM(o, dm) + FXM(X.cb[k].s, dm);
            X.cb[k].b[X.cb[k].i] = inp + FXM(X.cb[k].s, fk);
            if (++X.cb[k].i == X.cb[k].n) X.cb[k].i = 0;
            s += o;
        }
        for (k = 0; k < 2; k++) {
            int32_t o = X.ap[k].b[X.ap[k].i], v = o - s;
            X.ap[k].b[X.ap[k].i] = s + (o >> 1);
            if (++X.ap[k].i == X.ap[k].n) X.ap[k].i = 0;
            s = v;
        }
        x[i] = y + (int32_t)(((int64_t)s * m * 3) >> 18);   /* s Q18 -> Q15, * 3 * mix */
    }
}
static void fx7(int32_t *x, int n)                              /* Granular */
{
    int32_t m = mixk(7), size = fxk(P_F7SIZE), dens = fxk(P_F7DENSITY), pit = fxk(P_F7PITCH), i;
    int32_t gl = 441 + (int32_t)(((int64_t)size * 8379) >> 15);                           /* (.01 + size * .19) * sr */
    int32_t iv = (int32_t)((uint64_t)FS * 32768u / (uint32_t)(2 * 32768 + 60 * dens));    /* sr / (2 + density * 60) */
    int32_t ratio = gl * 256 / iv, sc = ratio <= 256 ? 32768 : (int32_t)(32768ull * 16u / isqrt32((uint32_t)ratio));   /* 1 / sqrt(max(1, n / iv)) */
    int32_t dmax = (int32_t)X.gb.n - 6, mo = FXM(m, 45875), mi = FXM(m, 19661);
    if (dmax > (int32_t)(1.1 * FS)) dmax = (int32_t)(1.1 * FS);
    for (i = 0; i < n; i++) {
        int32_t y = x[i], o = 0, g;
        dwr(&X.gb, y);
        if (--X.gc <= 0) {
            X.gc += (int32_t)(((int64_t)iv * (16384 + (int32_t)(rnd32() >> 17))) >> 15);          /* iv * (.5 + random) */
            if (X.gn < 24) {
                int32_t e = ((pit - 16384) * 96 + (((int32_t)(rnd32() >> 17) - 16384) * 3) / 5) / 12;   /* octaves, Q16: ((pitch - .5) * 48 + (random - .5) * .3) / 12 */
                uint32_t r = fx_exp2(e, 16);
                int32_t k = X.gn++;
                X.gr[k].a = 0; X.gr[k].n = gl; X.gr[k].inc = (uint32_t)(4294967296ull / (uint32_t)gl); X.gr[k].ph = 0;
                X.gr[k].d = (882 + (int32_t)(((uint32_t)(rnd32() >> 17) * 22050u) >> 15)) << 16;     /* (.02 + random * .5) * sr */
                X.gr[k].dd = 65536 - (int32_t)r;
            }
        }
        for (g = (int32_t)X.gn - 1; g >= 0; g--) {
            int32_t d = X.gr[g].d >> 16;
            o += FXM(drd(&X.gb, (uint32_t)X.gr[g].d), hann15(X.gr[g].ph));
            X.gr[g].ph += X.gr[g].inc; X.gr[g].a++; X.gr[g].d += X.gr[g].dd;
            if (X.gr[g].a >= X.gr[g].n || d < 2 || d > dmax) X.gr[g] = X.gr[--X.gn];
        }
        x[i] = y - FXM(y, mi) + FXM(FXM(o, sc), mo);
    }
}

static void fx8(int32_t *x, int n)                              /* Feedback */
{
    int32_t m = mixk(8), tone = 3277 + FXM(fxk(P_F8TONE), 27853), fbk = 16384 + FXM(fxk(P_F8FB), 16056), mo = FXM(m, 39322), i;
    uint32_t D = (uint32_t)(((uint64_t)fx_exp2(-(PVI(P_F8FREQ) * 327680 / 1000), 16) * 2205u) >> 1);        /* sr / (40 * 2^(5 freq)), Q16 samples */
    if (D < 131072u) D = 131072u;
    for (i = 0; i < n; i++) {
        int32_t y = x[i], v = drd(&X.fbl, D);
        X.fvl += FXM(v - X.fvl, tone);
        dwr(&X.fbl, c_tanh(FXM(y, 19661) + FXM(X.fvl, fbk)));
        x[i] = y - FXM(y, m >> 1) + FXM(X.fvl, mo);
    }
}

static void fx9(int32_t *x, int n)                              /* Bitcrush */
{
    int32_t m = mixk(9), i;
    uint32_t step = fx_exp2(3 * 65536 + PVI(P_F9CRUSH) * 655360 / 1000, 0);       /* 2^(15 - (12 - 10 crush)) = 2^(3 + 10 crush): the step in Q15 */
    uint32_t r = 65536u + (uint32_t)fxk(P_F9RATE) * 80;               /* 1 + rate * 40, Q16 */
    uint32_t inv;
    if (step < 1) step = 1;
    inv = 0xFFFFFFFFu / step;
    for (i = 0; i < n; i++) {
        int32_t y = x[i];
        X.bcC += 65536;
        if ((uint32_t)X.bcC >= r) {
            int32_t t = y + (int32_t)(step >> 1);
            X.bcC -= (int32_t)r;
            if (t >= 0) X.bcH = (int32_t)(((uint64_t)(uint32_t)t * inv) >> 32) * (int32_t)step;
            else X.bcH = -(int32_t)(((uint64_t)(uint32_t)(-t + (int32_t)step - 1) * inv) >> 32) * (int32_t)step;
        }
        x[i] = y + FXM(X.bcH - y, m);
    }
}

static void fx10(int32_t *x, int n)                             /* Ring mod */
{
    int32_t m = mixk(10), sh = fxk(P_F10SHAPE), mo = FXM(m, 39322), i;
    uint32_t inc = (uint32_t)(((uint64_t)fx_exp2_24(DIV1000((int64_t)PVI(P_F10FREQ) * 117440512), 16) * 20u * 6382653000ull) >> 32);   /* 20 * 2^(7 freq) Hz */
    for (i = 0; i < n; i++) {
        int32_t y = x[i], s, c;
        X.rmP += inc;
        s = sin_q15(X.rmP); c = FXM(s, 32768 - sh) + (s > 0 ? sh : -sh);
        x[i] = y - FXM(y, m) + FXM(FXM(y, c), mo);
    }
}

static void fx11(int32_t *x, int n)                             /* Stutter */
{
    int32_t m = mixk(11), sl = (int32_t)(((int64_t)(983 + FXM(fxk(P_F11LEN), 8847)) * FS) >> 15), i;       /* (.03 + len * .27) * sr */
    int32_t chance = FXM(fxk(P_F11CHANCE), 22938);
    int32_t mx = (int32_t)X.sb.n - 2;
    if (sl > mx) sl = mx;
    for (i = 0; i < n; i++) {
        int32_t y = x[i];
        if (!X.stOn) dwr(&X.sb, y);
        if (++X.stC >= sl) {
            X.stC = 0;
            if (X.stOn) { if (--X.stReps <= 0) X.stOn = 0; }
            else if ((int32_t)(rnd32() >> 17) < chance) {
                X.stOn = 1; X.stReps = 1 + (int32_t)((rnd32() >> 16) % 5u);
                X.stLen = (rnd32() >> 31) ? sl : sl >> 1;
                if (X.stLen < 8) X.stLen = 8;
                X.stK = 0; X.stIdx = (int32_t)X.sb.i;
            }
        }
        if (X.stOn) {
            int32_t p = (X.stIdx - X.stLen + (X.stK % X.stLen)) % (int32_t)X.sb.n;
            if (p < 0) p += (int32_t)X.sb.n;
            y += FXM((X.sb.b[p] << 1) - y, m);
            X.stK++;
        }
        x[i] = y;
    }
}
static void fx12(int32_t *x, int n)                             /* Reverse */
{
    int32_t m = mixk(12), fb = FXM(fxk(P_F12FB), 26214), mo = FXM(m, 29491), mh = m >> 1, i;
    uint32_t T = (uint32_t)DIV100((int64_t)(1000 + 9 * PVI(P_F12TIME)) * 112896), mx = (X.rv.n - 6u) << 8;     /* (.1 + time * .9) * sr, Q8 samples */
    uint32_t inc;
    if (T > mx) T = mx;
    if (T < 64 * 256) T = 64 * 256;
    inc = (uint32_t)((1ull << 41) / T);                         /* 1 / C, C = T / 2, turns per sample */
    for (i = 0; i < n; i++) {
        int32_t y = x[i], w = 0, t;
        X.rvK += inc;
        for (t = 0; t < 2; t++) {
            uint32_t ph = X.rvK + (t ? 0x80000000u : 0);
            w += FXM(drd(&X.rv, (uint32_t)(((uint64_t)ph * T) >> 24) + 131072u), hann15(ph));
        }
        dwr(&X.rv, y + FXM(w, fb));
        x[i] = y - FXM(y, mh) + FXM(w, mo);
    }
}
static void fx13(int32_t *x, int n)                             /* Freeze */
{
    int32_t m = mixk(13), i;
    uint32_t Lf = (uint32_t)DIV100((int64_t)(200 + 4 * PVI(P_F13SIZE)) * 112896), inc;        /* (.02 + size * .4) * sr, Q8 samples */
    uint32_t mx = (X.fz.n - 6u) << 8;
    if (Lf > mx) Lf = mx;
    if (Lf < 32 * 256) Lf = 32 * 256;
    inc = (uint32_t)((1ull << 40) / Lf);
    if (PV[P_F13HOLD] < 500) {
        for (i = 0; i < n; i++) dwr(&X.fz, x[i]);
        X.fzOn = 0;
        return;
    }
    if (!X.fzOn) { X.fzOn = 1; X.fzI = (int32_t)X.fz.i; X.fzK = 0; }
    for (i = 0; i < n; i++) {
        int32_t y = x[i], w = 0, t;
        X.fzK += inc;
        for (t = 0; t < 2; t++) {
            uint32_t ph = X.fzK + (t ? 0x80000000u : 0);
            /* p = fzI - Lf + ph * Lf (samples, Q16); the same interpolation as rd() but measured from the frozen index */
            int64_t pq = ((int64_t)X.fzI << 16) - ((int64_t)Lf << 8) + (int64_t)(((uint64_t)ph * Lf) >> 24);
            int64_t nq = (int64_t)X.fz.n << 16;
            int32_t a, f, b0, b1;
            if (pq < 0) pq += nq;
            if (pq >= nq) pq -= nq;
            a = (int32_t)(pq >> 16); f = (int32_t)((pq & 0xFFFF) >> 1);
            b0 = X.fz.b[a]; b1 = X.fz.b[a + 1 == (int32_t)X.fz.n ? 0 : a + 1];
            w += FXM((b0 + (((b1 - b0) * f) >> 15)) << 1, hann15(ph));
        }
        x[i] = y + FXM(w - y, m);
    }
}
static void fx14(int32_t *x, int n)                             /* Tape */
{
    int32_t m = mixk(14), wow = fxk(P_F14WOW), drv = fxk(P_F14DRIVE), age = fxk(P_F14AGE), i;
    int32_t g = 32768 + (drv << 2), inv = (int32_t)((1ull << 30) / (uint32_t)(32768 + (drv * 3 >> 1))), c = 1311 + FXM(32768 - age, 29491);   /* .04 + (1 - age) * .9 */
    uint32_t i3 = INC_MHZ(600), i4 = INC_MHZ(7300);                /* .6 Hz, 7.3 Hz */
    for (i = 0; i < n; i++) {
        int32_t y = x[i], s, a;
        uint32_t d;
        X.lf[3] += i3; X.lf[4] += i4;
        d = 34681651u + (uint32_t)(((int64_t)FXM(sin_q15(X.lf[3]), wow) * 17340000) >> 15) + (uint32_t)(((int64_t)FXM(sin_q15(X.lf[4]), wow) * 1734000) >> 15);
        dwr(&X.tp, y);
        s = drd(&X.tp, d);
        a = (int32_t)(((int64_t)s * g) >> 15);
        a = a > 131071 ? 131071 : a < -131071 ? -131071 : a;
        s = FXM(c_tanh(a), inv);
        X.tpL += FXM(s - X.tpL, c);
        x[i] = y + FXM(X.tpL - y, m);
    }
}
static void fx15(int32_t *x, int n)                             /* Shimmer */
{
    int32_t m = mixk(15), dec = 11469 + FXM(fxk(P_F15DECAY), 18022), i;                  /* .35 + decay * .55 */
    uint32_t r = fx_exp2(PVI(P_F15SHIFT) * 131072 / 1000, 28);             /* 2^(shift * 2), Q28 */
    int32_t inc = (int32_t)(((int64_t)((1 << 28) - (int32_t)r) * (4294967296ull / 2205u)) >> 28);
    int32_t mo = FXM(m, 26214), mi = FXM(m, 13107);
    const uint32_t d1 = (uint32_t)(FS * 21ull / 100) << 16, d2 = (uint32_t)(FS * 337ull * 65536u / 1000);
    for (i = 0; i < n; i++) {
        int32_t y = x[i], v, sh;
        uint32_t q;
        v = drd(&X.tl, d1) + FXM(drd(&X.tl, d2), 22938);
        X.shL += FXM(v - X.shL, 14746);
        dwr(&X.p2, X.shL);
        X.shP += (uint32_t)inc;
        q = X.shP + 0x80000000u;
        sh = FXM(drd(&X.p2, (uint32_t)(((uint64_t)X.shP * 2205u) >> 16) + 131072u), sin_q15(X.shP >> 1))
           + FXM(drd(&X.p2, (uint32_t)(((uint64_t)q * 2205u) >> 16) + 131072u), sin_q15(q >> 1));
        dwr(&X.tl, c_tanh((y >> 1) + FXM(sh, dec)));
        x[i] = y - FXM(y, mi) + FXM(X.shL + sh, mo);
    }
}
/* ------------------------------------------------------------------ the bus */
typedef void (*fxfn_t)(int32_t *, int);
static const fxfn_t FXF[16] = { fx0, fx1, fx2, fx3, fx4, fx5, fx6, fx7, fx8, fx9, fx10, fx11, fx12, fx13, fx14, fx15 };

uint32_t fx_ticks_max, fx_ticks_last;                   /* CPU measure: timer ticks spent in fx_bus (worst block, last block) */

#ifdef FX_TEST                                          /* test builds: -DFX_TEST=<effect> -DFX_TP="{k0,k1,..,mix}" (thousandths), -DFX_TS=1 for the delay's sync */
static void fx_test_init(void)
{
    static const int16_t tp[] = FX_TP;
    int i, j = 1;
    fxs[0] = FX_TEST;
    for (i = 0; i < 16; i++) if (i != FX_TEST && j < 6) fxs[j++] = (uint8_t)i;
    for (i = 0; i < (int)(sizeof tp / sizeof tp[0]); i++) PV[FX_FIRST[FX_TEST] + i] = tp[i];
#ifdef FX_TS
    PV[P_F5SYNC] = 1;
#endif
}
#endif

#ifdef FX_TEST
int32_t fx_dry_tap[128], fx_wet_tap[128];                                 /* test builds: the bus before the effects, written to the right channel */
#endif
void fx_bus(int32_t *x, int n)
{
    int q, same = 1, any = 0, i;
#ifdef FX_TEST
    static uint8_t once;
    if (!once) { once = 1; fx_test_init(); }
#endif
    for (q = 0; q < 6; q++) if (fx_sig[q] != fxs[q]) same = 0;
#ifdef FX_TEST
#ifdef FX_SINE
    { static uint32_t sph; for (i = 0; i < n; i++) { x[i] = (sin_q15(sph) * FX_SINE) >> 10; sph += INC_MHZ(441000); } }     /* test: a sine of FX_SINE/1024 instead of the voices */
#endif
    for (i = 0; i < n; i++) fx_dry_tap[i] = x[i];
#endif
    if (!same) fx_alloc();
    for (q = 0; q < 6; q++) if (PV[FX_FIRST[fxs[q]] + FX_P[fxs[q]].n - 1] > 0) any = 1;
    if (!any) {
#ifdef FX_TEST
        for (i = 0; i < n; i++) fx_wet_tap[i] = x[i];
#endif
        return;
    }
    {
        uint32_t t0 = fm1_ticks();
        for (i = 0; i < n; i++) x[i] += x[i] >> 2;                  /* the prototype runs the effects before the .8 track volume */
        for (q = 0; q < 6; q++) {
            int f = fxs[q];
            if (PV[FX_FIRST[f] + FX_P[f].n - 1] > 0) FXF[f](x, n);
        }
        for (i = 0; i < n; i++) x[i] = FXM(x[i], 26214);
#ifdef FX_TEST
        for (i = 0; i < n; i++) fx_wet_tap[i] = x[i];
#endif
        fx_ticks_last = fm1_ticks() - t0;
        if (fx_ticks_last > fx_ticks_max) fx_ticks_max = fx_ticks_last;
    }
}
