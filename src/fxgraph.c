/* SPDX-License-Identifier: MPL-2.0 */
/* FX page graphs: the prototype's dfx(), the 16 animated effect pictures (grey: dry signal, white: processed, intensity follows Mix),
 * in fixed point.  Drawn per band (strokes are rebuilt for the band at hand and clipped to it).  Included from ui.c.
 * Canvas 330 x 120, M = 60 the middle; x runs 0..330 in steps of 2 px (166 points); t is the wall clock. */

#define FXN 166
static int32_t fxpx[FXN + 80], fxpy[FXN + 80];
static stroke_t fx_tmp;

static uint32_t tph(uint32_t rate_q16) { return (uint32_t)(((uint64_t)fx_ms * rate_q16 * 4294967u) >> 16); }     /* phase of t * rate, turns */
static int32_t K16(int fx, int lab)                      /* the knob of the effect labelled `lab`, Q16 (.5 when the effect has none) */
{
    int i;
    for (i = 0; i < FX_P[fx].n; i++)
        if (FX_P[fx].l[i] == lab)
            return PV[FX_FIRST[fx] + i] * 65536 / 1000;
    return 32768;
}
#define K14_32 28999739u
#define K5_32 10357050u
#define K6_32 12428460u
#define K09_32 615208748u
#define INV330_32 13015052u
static int32_t sinx(int32_t x_q8, int32_t f_q16, uint32_t ph)     /* sin(2 pi (x / 330 * f) + ph), x in Q8 canvas px >= 0, f in Q16, Q15 out */
{
    return sin_q15((uint32_t)(((uint64_t)(uint32_t)x_q8 * (uint32_t)f_q16 * INV330_32) >> 24) + ph);
}
static int32_t cosq(uint32_t ph) { return sin_q15(ph + 0x40000000u); }
static uint32_t log2_q16(uint32_t v)                     /* log2(v) in Q16 for an integer v >= 1 */
{
    int e = 31, lo = 0, hi = 256;
    uint32_t m, mm;
    while (!(v >> e)) e--;
    m = e >= 30 ? v >> (e - 30) : v << (30 - e);        /* Q30 in [1,2) */
    while (hi - lo > 1) { int mid = (lo + hi) >> 1; if (EXP2_TAB[mid] <= m) lo = mid; else hi = mid; }
    mm = EXP2_TAB[lo + 1] - EXP2_TAB[lo];
    return (uint32_t)e * 65536u + (uint32_t)lo * 256u + (mm ? ((m - EXP2_TAB[lo]) * 256u) / mm : 0u);
}
static int32_t expn16(int32_t x_q16)                     /* exp(-x) in Q16, x >= 0 */
{
    if (x_q16 > 16 * 65536) return 0;
    return (int32_t)exp2_fx(-(int32_t)(((int64_t)x_q16 * 94548) >> 16), 16);
}
static int32_t tanh15(int32_t x_q16)                     /* tanh, Q16 in, Q15 out */
{
    int neg = x_q16 < 0;
    uint32_t e = (uint32_t)expn16(2 * (neg ? -x_q16 : x_q16));
    int32_t r = (int32_t)(((65536u - e) << 15) / (65536u + e));
    return neg ? -r : r;
}
static uint32_t hash32(uint32_t a, uint32_t b)
{
    uint32_t h = a * 2654435761u ^ (b + 0x9E3779B9u) * 2246822519u;
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13; h *= 3266489917u; h ^= h >> 16;
    return h;
}
static int32_t hrand16(uint32_t a, uint32_t b) { return (int32_t)(hash32(a, b) >> 16); }     /* [0,1) Q16 */

#define XC16(xq8) (115 + ((((int32_t)(xq8)) * 11202) >> 18))   /* canvas x (Q8) -> 1/16 px */
#define YCQ(yq8) GYC16(yq8)
static void fx_line(int n, uint32_t col, uint32_t a256, int r16)
{
    poly_stroke(fxpx, fxpy, n, r16, &fx_tmp);
    stroke_paint_a(&fx_tmp, col, a256);
}
static void fx_grid(void) { int i; for (i = 0; i < FXN; i++) fxpx[i] = GX16(2 * i); }
static void fx_rect(int32_t x_q8, int32_t y_q8, int32_t w_q8, int32_t h_q8, uint32_t col, uint32_t a)
{
    gfx_frect16(XC16(x_q8), YCQ(y_q8), XC16(x_q8 + w_q8), YCQ(y_q8 + h_q8), col, a);
}
#define Y16(v15, amp16) (G_AXIS - (((int32_t)(v15) * (amp16)) >> 15))
#define COL_G 0x3a3a3au

static int32_t f0q(int32_t x_q8, uint32_t tp)                         /* freeze: sin(x / W * 14 + t * 2) * (.7 + .3 sin(x / W * 5)), Q15 */
{
    uint32_t xx = (uint32_t)x_q8;
    int32_t w = 45875 + ((19661 * sin_q15((uint32_t)(((uint64_t)xx * K5_32) >> 8))) >> 15);
    return (int32_t)(((int64_t)sin_q15((uint32_t)(((uint64_t)xx * K14_32) >> 8) + tp) * w) >> 16);
}
static void fx_draw(void)
{
    int fx = fxs[fxo], i, k;
    int32_t mix = K16(fx, L_MIX), al = 77 + (int32_t)(((int64_t)179 * mix) >> 16);               /* .3 + .7 mix, /256 */
    uint32_t t4 = tph(26214);
    pc_clip = 1;
    fx_grid();
#define AX() g_hline(7, 232, G_AXIS, 0x333333u)
    switch (fx) {
    case 0: {                                                                   /* Overdrive */
        int32_t d = 65536 + 14 * K16(0, L_DRIVE), tn = K16(0, L_TONE), c = 16384 + (int32_t)(((int64_t)49152 * tn) >> 16), lp = 0, f = 45875 + (int32_t)(((int64_t)19661 * mix) >> 16);
        AX();
        for (i = 0; i < FXN; i++) fxpy[i] = Y16(sinx(2 * i * 256, 3 << 16, t4), 491);
        fx_line(FXN, COL_G, 256, 11);
        for (i = 0; i < FXN; i++) {
            int32_t y = tanh15((int32_t)(((int64_t)sinx(2 * i * 256, 3 << 16, t4) * d) >> 15));
            lp += (int32_t)(((int64_t)(y - lp) * c) >> 16);
            fxpy[i] = Y16(lp, (491 * f) >> 16);
        }
        fx_line(FXN, COL_TEXT, al, 19);
        break;
    }
    case 1: case 2: {                                                           /* Phaser, Flanger */
        int fl = fx == 2;
        int32_t rt = 9830 + (int32_t)(((int64_t)163840 * K16(fx, L_RATE)) >> 16), dp = K16(fx, L_DEPTH), fb = K16(fx, L_FB), pw = 65536 + 3 * fb;
        int32_t sw = sin_q15(tph((uint32_t)rt)) + 32768;                       /* (sin + 1) / 2, Q16 */
        int32_t swdp = (int32_t)(((int64_t)sw * dp) >> 16);
        int32_t sp = 4260 + (int32_t)(((int64_t)14746 * swdp) >> 16);          /* flanger: .065 + .225 dp sw */
        g_hline(7, 232, 2339, COL_G);
        for (i = 0; i < FXN; i++) {
            int32_t x = 2 * i, a, sh, in1, in2, y;
            if (fl) {
                a = cosq((uint32_t)((x * 3253764) / sp) << 16) * 2;           /* Q16 */
            } else {
                int32_t u = (x << 16) / 330, sum = 0;
                for (k = 0; k < 3; k++) {
                    int32_t cn = (k * 65536 + 32768 + (int32_t)(((int64_t)swdp * 58982) >> 16)) / 3;
                    int32_t z = (int32_t)(((int64_t)(u - cn) * (655360 + 40 * fb)) >> 16), z2 = (int32_t)(((int64_t)z * z) >> 16);
                    sum += expn16(z2);
                }
                if (sum > 65536) sum = 65536;
                a = 65536 - (int32_t)(((int64_t)sum * 117965) >> 16);
            }
            {                                                                    /* |a| ^ (1 + 3 fb) */
                int32_t aa = a < 0 ? -a : a;
                if (aa < 2) sh = 0;
                else sh = (int32_t)exp2_fx((int32_t)(((int64_t)pw * ((int32_t)log2_q16((uint32_t)aa) - 16 * 65536)) >> 16), 16);
                if (sh > 65536) sh = 65536;
            }
            in1 = 65536 - (int32_t)(((int64_t)(int32_t)(((int64_t)mix * 58982) >> 16) * (65536 - sh)) >> 16);
            in2 = fl ? 32768 + (sh >> 1) : 26214 + (int32_t)(((int64_t)39322 * (a > 0 ? a : 0)) >> 16);
            y = (int32_t)(((int64_t)in1 * in2) >> 16);
            fxpy[i] = 3113 - (int32_t)(((int64_t)762 * y) >> 16);
        }
        fx_line(FXN, COL_TEXT, al, 19);
        break;
    }
    case 3: {                                                                   /* Chorus */
        int32_t rt = 13107 + (int32_t)(((int64_t)163840 * K16(3, L_RATE)) >> 16), dp = K16(3, L_DEPTH);
        uint32_t tr = tph((uint32_t)rt);
        AX();
        for (i = 0; i < FXN; i++) fxpy[i] = Y16(sinx(2 * i * 256, 3 << 16, t4), 442);
        fx_line(FXN, COL_G, 256, 11);
        for (k = 0; k < 3; k++) {
            int32_t dv = (int32_t)(((int64_t)(((int64_t)sin_q15(tr + (uint32_t)k * 1435487079u) * dp) >> 15) * 9175) >> 16);       /* turns, Q16 */
            uint32_t p2 = t4 + ((uint32_t)dv << 16);
            for (i = 0; i < FXN; i++) fxpy[i] = Y16(sinx(2 * i * 256, 3 << 16, p2), 369);
            fx_line(FXN, COL_TEXT, (uint32_t)((al * 179) >> 8), 13);
        }
        break;
    }
    case 4: {                                                                   /* Micro pitch */
        int32_t f2 = 327680 + (int32_t)(((int64_t)68813 * K16(4, L_DETUNE)) >> 16), f = 39322 + (int32_t)(((int64_t)26214 * mix) >> 16);
        uint32_t t5 = tph(32768);
        AX();
        for (i = 0; i < FXN; i++) fxpy[i] = Y16(sinx(2 * i * 256, 5 << 16, t5), 246);
        fx_line(FXN, COL_G, 256, 8);
        for (i = 0; i < FXN; i++)
            fxpy[i] = Y16((sinx(2 * i * 256, 5 << 16, t5) + sinx(2 * i * 256, f2, t5)) >> 1, (491 * f) >> 16);
        fx_line(FXN, COL_TEXT, al, 19);
        break;
    }
    case 5: {                                                                   /* Delay */
        int32_t tm = 7864 + (int32_t)(((int64_t)45875 * K16(5, L_TIME)) >> 16), fb = K16(5, L_FB), fb95 = (int32_t)(((int64_t)fb * 62259) >> 16), hq = 65536, n, f = 22938 + (int32_t)(((int64_t)42598 * mix) >> 16);
        n = (58982 + tm - 1) / tm + 1;                                          /* ceil(.9 / tm) + 1 */
        if (n > 9) n = 9;
        fx_rect(8 * 256, 3686, 5 * 256, 23347, COL_G, 256);                      /* M - H*.38 = 14.4, h 91.2 */
        for (i = 1; i < n; i++) {
            int32_t x = 2048 + ((i * tm * 306) >> 8), h = (int32_t)(((int64_t)23347 * hq >> 16) * f >> 16);
            if (x > 330 * 256) break;
            fx_rect(x, 60 * 256 - h / 2, 5 * 256, h < 512 ? 512 : h, COL_TEXT, (uint32_t)(al * (65536 - 4588 * i) >> 16));
            hq = (int32_t)(((int64_t)hq * fb95) >> 16);
        }
        break;
    }
    case 6: {                                                                   /* Reverb */
        int32_t dm = K16(6, L_DAMP), coef = 280494 - (int32_t)(((int64_t)188744 * K16(6, L_SIZE)) >> 16), f = 13107 + (int32_t)(((int64_t)52429 * mix) >> 16);
        uint32_t ep = (fx_ms * 6u) / 1000u;
        fx_rect(8 * 256, 3686, 5 * 256, 23347, COL_G, 256);
        for (k = 18; k < 330; k += 3) {
            int32_t u = ((k - 18) << 16) / 312, e = expn16((int32_t)(((int64_t)u * coef) >> 16)), j = hrand16((uint32_t)k, ep) - 32768, off;
            if (j < 0) j = -j;
            j = (int32_t)(((int64_t)(j * 2) * (65536 - (int32_t)(((int64_t)39322 * dm) >> 16))) >> 16);
            e = (int32_t)(((int64_t)e * f) >> 16);
            off = (int32_t)(((int64_t)(516 * e >> 16) * j) >> 16);
            gfx_capsule(GX16(k) * 16, (G_AXIS - off) * 16, GX16(k) * 16, (G_AXIS + off) * 16, 170, COL_TEXT, (uint32_t)al);
        }
        break;
    }
    case 7: {                                                                   /* Granular */
        int32_t dens = K16(7, L_DENS), size = K16(7, L_SIZE), pitch = K16(7, L_PITCH);
        int n = 3 + (int)((dens * 18) >> 16);
        int32_t wd = 20 * 256 + (int32_t)(((int64_t)size * 90 * 256) >> 16);
        uint32_t tq8 = ((fx_ms % 100000u) * 256u) / 1000u;
        AX();
        for (i = 0; i < n; i++) {
            uint32_t period = (uint32_t)(330 * 256 + wd);
            int32_t cx0 = (int32_t)(((uint32_t)(i * 24909) + tq8 * (uint32_t)(20 + i % 5 * 8)) % period) - wd / 2;     /* i * 97.3 * 256 = 24909 */
            int32_t hh = 221 + 270 * ((i * 53) % 7) / 7, fr = 3 * 65536 + 14 * pitch + (i % 3) * 65536, oy = ((i * 37) % 5 - 2) * 123;
            int32_t xs = cx0 - wd / 2, x, np = 0;
            uint32_t ai = (uint32_t)(al * (128 + 128 * ((i * 29) % 5) / 5) >> 8);
            for (x = xs; x <= cx0 + wd / 2 && np < FXN; x += 512, np++) {
                int32_t u = (int32_t)(((int64_t)(x - xs) << 16) / wd), env = 16384 - (cosq((uint32_t)u << 16) >> 1);
                uint32_t pq = (uint32_t)(((int64_t)u * fr) >> 16) << 16;                                            /* u * fr turns */
                int32_t v = (int32_t)(((int64_t)sin_q15(pq + (uint32_t)i * 683565276u) * env) >> 15);               /* + i rad */
                fxpx[np] = XC16(x);
                fxpy[np] = G_AXIS + oy - ((v * hh) >> 15);
            }
            if (np > 1) fx_line(np, COL_TEXT, ai, 11);
        }
        break;
    }
    case 8: {                                                                   /* Feedback */
        int32_t fq = 6 * 65536 + 30 * K16(8, L_FREQ), dc = 3 * 65536 + 22 * (65536 - K16(8, L_FB)), tn = 98304 + 2 * K16(8, L_TONE);
        uint32_t pt = 0u - tph(31291);
        AX();
        fx_rect(8 * 256, 6144, 4 * 256, 18432, COL_G, 256);
        for (i = 0; i < FXN; i++) {
            int32_t x = 2 * i, arg = (int32_t)(((int64_t)sinx(x * 256, fq, pt) * tn) >> 15), xu = (x << 16) / 330;
            int32_t e = expn16((int32_t)(((int64_t)((int64_t)xu * dc >> 16) * 39322) >> 16));
            int32_t v = (int32_t)(((int64_t)tanh15(arg) * e) >> 16);
            fxpy[i] = Y16(v, x > 14 ? 491 : 197);
        }
        fx_line(FXN, COL_TEXT, al, 19);
        break;
    }
    case 9: {                                                                   /* Bitcrush */
        int32_t lv = 2 * 65536 + 14 * (65536 - K16(9, L_CRUSH)), hw = 2 * 65536 + 34 * K16(9, L_RATE);
        uint32_t t2 = tph(26214);
        AX();
        for (i = 0; i < FXN; i++) fxpy[i] = Y16(sinx(2 * i * 256, 2 << 16, t2), 491);
        fx_line(FXN, COL_G, 256, 8);
        for (i = 0; i < FXN; i++) {
            int32_t kq = ((2 * i) << 16) / hw, xq = (kq * hw) >> 8;
            int32_t sv = sinx(xq, 2 << 16, t2), n = (int32_t)(((int64_t)sv * lv + (1 << 30)) >> 31);
            fxpy[i] = Y16(n * (int32_t)(0x80000000u / (uint32_t)lv), 491);
        }
        fx_line(FXN, COL_TEXT, al, 16);
        break;
    }
    case 10: {                                                                  /* Ring mod */
        int32_t fc = 2 * 65536 + 14 * K16(10, L_FREQ), sh = K16(10, L_SHAPE);
        uint32_t pa = tph(20861), pb = tph(19661);
        AX();
        for (i = 0; i < FXN; i++) fxpy[i] = Y16(sinx(2 * i * 256, fc, pa), 147);
        fx_line(FXN, COL_G, 256, 8);
        for (i = 0; i < FXN; i++) {
            int32_t c = sinx(2 * i * 256, fc, pa), cc = (int32_t)(((int64_t)c * (65536 - sh)) >> 16) + (int32_t)(((int64_t)(c > 0 ? 32768 : -32768) * sh) >> 16);
            fxpy[i] = Y16((sinx(2 * i * 256, 2 << 16, pb) * cc) >> 15, 491);
        }
        fx_line(FXN, COL_TEXT, al, 19);
        break;
    }
    case 11: {                                                                  /* Stutter */
        int gs = 2 + (int)((K16(11, L_LEN) * 4) >> 16);
        int32_t ch = K16(11, L_CHANCE);
        uint32_t ep = (fx_ms * 7u) / 10000u;
        for (i = 0; i < 22; i++) {
            int sg = i / gs, rp = hrand16((uint32_t)sg * 77u, ep) < (int32_t)(((int64_t)ch * 52429) >> 16);
            int32_t h = ((hrand16(rp ? (uint32_t)(i % gs) + 100u : (uint32_t)i, ep) * 52429) >> 16) + 13107, hq8 = (int32_t)(((int64_t)h * (96 * 256)) >> 16);
            fx_rect((i * 15 + 1) * 256, 60 * 256 - hq8 / 2, 13 * 256, hq8, rp ? COL_TEXT : COL_G, rp ? (uint32_t)al : 256u);
        }
        break;
    }
    case 12: {                                                                  /* Reverse */
        int32_t R = 16896 + (int32_t)(((int64_t)K16(12, L_TIME) * 42240) >> 16), fb = K16(12, L_FB), f = 19661 + (int32_t)(((int64_t)45875 * mix) >> 16), fb85 = (int32_t)(((int64_t)fb * 55706) >> 16);
        AX();
        for (i = 0; i < FXN; i++) {
            int32_t x = 2 * i, u = x * 256 - 6758, e = 0;                       /* st = W * .08 = 26.4 */
            if (u >= 0 && u < 18586)                                            /* W * .22 = 72.6 */
                e = expn16((u << 16) / 5914);                                   /* exp(-u / (W * .07)) */
            fxpy[i] = Y16(((int64_t)sin_q15((uint32_t)x * K09_32) * e) >> 16, 491);
        }
        fx_line(FXN, COL_G, 256, 8);
        for (i = 0; i < FXN; i++) {
            int32_t x = 2 * i, e = 0, pwr = 65536;
            for (k = 0; k < 3; k++) {
                int32_t d = x * 256 - (6758 + 20275 + k * R);                   /* s0 = st + W * .24 + k R */
                if (d >= 0 && d < R) {
                    int32_t uq = (int32_t)(((uint32_t)d << 16) / (uint32_t)R), v = (int32_t)(((int64_t)(((int64_t)uq * uq) >> 16) * pwr >> 16) * f >> 16);
                    if (v > e) e = v;
                }
                pwr = (int32_t)(((int64_t)pwr * fb85) >> 16);
            }
            fxpy[i] = Y16(((int64_t)sin_q15((uint32_t)x * K09_32) * e) >> 16, 516);
        }
        fx_line(FXN, COL_TEXT, al, 13);
        break;
    }
    case 13: {                                                                  /* Freeze */
        int32_t Lf = 8448 + (int32_t)(((int64_t)K16(13, L_SIZE) * 25344) >> 16), hd = K16(13, L_HOLD) >= 32768, x0 = 29568;     /* W * (.1 + .3 size); x0 = W * .35 */
        uint32_t tp = tph(20861);
        AX();
        for (i = 0; i < FXN; i++) {
            int32_t xq = 2 * i * 256, xe = (hd && xq > x0) ? x0 : xq;
            fxpy[i] = Y16(f0q(xe, tp), 467);
        }
        fx_line(FXN, COL_G, 256, 8);
        if (hd) {
            for (i = 0; i < FXN; i++) {
                int32_t xq = 2 * i * 256, xs = x0 - Lf + ((xq - x0) % Lf);
                fxpy[i] = xq < x0 ? G_AXIS : Y16(f0q(xs, tp), 467);
            }
            fx_line(FXN, COL_TEXT, al, 19);
            g_vlinex(XC16(x0 - Lf) - 10, XC16(x0 - Lf) + 11, YCQ(6 * 256), YCQ(114 * 256), COL_RED);
            g_vlinex(XC16(x0) - 10, XC16(x0) + 11, YCQ(6 * 256), YCQ(114 * 256), COL_RED);
        } else {
            for (i = 0; i < FXN; i++) fxpy[i] = Y16(f0q(2 * i * 256, tp), 467);
            fx_line(FXN, COL_TEXT, (uint32_t)(al >> 1), 11);
        }
        break;
    }
    case 14: {                                                                  /* Tape */
        int32_t wo = K16(14, L_WOW), dr = K16(14, L_DRIVE), ag = K16(14, L_AGE), g = 65536 + 5 * dr, amp = (467 * (65536 - (int32_t)(((int64_t)22938 * ag) >> 16))) >> 16;
        uint32_t t4b = tph(26214), t3 = tph(31291);
        AX();
        for (i = 0; i < FXN; i++) fxpy[i] = Y16(sinx(2 * i * 256, 3 << 16, t4b), 467);
        fx_line(FXN, COL_G, 256, 8);
        for (i = 0; i < FXN; i++) {
            uint32_t x = (uint32_t)(2 * i * 256);
            int32_t w = (int32_t)(((int64_t)(((int64_t)sin_q15((uint32_t)(((uint64_t)x * K6_32) >> 8) + t3) * wo) >> 15) * 22938) >> 16);   /* wow * .35 * sin(..), turns Q16 */
            uint32_t pp = (uint32_t)(((uint64_t)x * 39045156u) >> 8) + t4b + ((uint32_t)w << 16);
            fxpy[i] = G_AXIS - (int32_t)(((int64_t)tanh15((int32_t)(((int64_t)sin_q15(pp) * g) >> 15)) * amp) >> 15);
        }
        fx_line(FXN, COL_TEXT, al, 19);
        break;
    }
    default: {                                                                  /* Shimmer */
        int32_t sh2 = 2 * K16(15, L_SHIFT), dc = K16(15, L_DECAY), a0 = 26214 + (int32_t)(((int64_t)32768 * dc) >> 16), aq = 65536, ap = 65536;
        static const int RK[4] = { 0, 15, 12, 9 };
        AX();
        for (i = 0; i < FXN; i++) fxpy[i] = Y16(sinx(2 * i * 256, 3 << 16, t4), 418);
        fx_line(FXN, COL_G, 256, 11);
        for (k = 1; k <= 3; k++) {
            int32_t fq = (int32_t)exp2_fx(k * sh2 + 103872, 16);
            uint32_t pk = tph((uint32_t)(26214 * k));
            if (fq > 60 * 65536 || fq < 0) fq = 60 * 65536;
            for (i = 0; i < FXN; i++) fxpy[i] = Y16(sinx(2 * i * 256, fq, pk), (418 * ap) >> 16);
            fx_line(FXN, COL_TEXT, (uint32_t)(((int64_t)al * aq) >> 16), RK[k]);
            aq = (int32_t)(((int64_t)aq * a0) >> 16);
            ap = (int32_t)(((int64_t)ap * 40632) >> 16);
        }
        break;
    }
    }
    pc_clip = 0;
}
