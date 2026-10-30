/* ENV page graph: the prototype's denv() in fixed point.
 * The AD envelope and the vactrol response it drives are simulated at 8 kHz (int64, Q30) exactly as envCurves() does, only
 * when ATK / EDEC / DECAY change, and kept at the 315 columns the canvas draws.  Strokes are the union of discs along the
 * polyline (the same shape as canvas lineJoin "round"), filled with the coverage accumulator.  Included from ui.c. */
#define EC 315                                   /* canvas columns 0 .. 314 (x = 8 .. 322) */
#define EK_ATK0   (-653118)                       /* log2(.001) in Q16 */
#define EK_EDEC0  (-500948)                       /* log2(.005) */
static uint16_t eg_env[EC], eg_vac[EC];          /* Q15 */
static uint32_t eg_T24;                          /* total duration, seconds in Q24 */
static uint32_t eg_N;                            /* samples */
static int32_t eg_at, eg_open;                   /* attack column, vf offset Q15 */
static int32_t eg_key[5] = { -1, -1, -1, -1, -1 };
static int16_t eg_c_lo[NS], eg_c_hi[NS], eg_v_lo[NS], eg_v_hi[NS], eg_fy[NS];
static uint8_t eg_fy_ok[NS];
static uint32_t eg_t0, eg_now;

static uint64_t div64(uint64_t n, uint64_t d)       /* the target has no 64-bit divide */
{
    uint64_t q = 0, r = 0;
    int i;
    for (i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= d) { r -= d; q |= 1ull << i; }
    }
    return q;
}

static uint32_t em1_q30(uint64_t z)              /* 1 - exp(-z), z in Q30 and below about .5 */
{
    uint64_t z2 = (z * z) >> 30, z3 = (z2 * z) >> 30, z4 = (z3 * z) >> 30, z5 = (z4 * z) >> 30;
    return (uint32_t)(z - (z2 >> 1) + ((z3 * 2796203u) >> 24) - ((z4 * 699051u) >> 24) + ((z5 * 139810u) >> 24));
}

static void env_sim(void)
{
    uint32_t atk = exp2_fx(EK_ATK0 + lg(PV[P_ATK], K_L2000), 24), ed = exp2_fx(EK_EDEC0 + lg(PV[P_EDEC], K_L600), 24);
    uint32_t dc = exp2_fx(K_DEC0 + lg(PV[P_DECAY], K_L1000), 24);
    uint32_t T = atk + ed * 7 / 5 + (dc * 6 / 5 < ed * 4 / 5 ? dc * 6 / 5 : ed * 4 / 5);
    uint32_t m = dc * 2 / 5 < 16777u ? dc * 2 / 5 : 16777u, t2 = 20133u > dc * 3 / 25 ? 20133u - dc * 3 / 25 : 0;
    uint64_t one = 1ull << 30;
    int64_t aAtt, aDec, step, rdec, e = 0, fa = 0, sl = 0, t;
    uint32_t N = (uint32_t)(((uint64_t)T * 8000u + (1u << 23)) >> 24), i, col = 0;
    int st = 1;
    if (N < 2) N = 2;
    aAtt = em1_q30(div64(1ull << 54, (uint64_t)(m + t2) * 8000u));
    aDec = em1_q30(div64(1ull << 54, (uint64_t)dc * 8000u));
    step = (int64_t)div64(1ull << 54, (uint64_t)atk * 8000u);
    rdec = em1_q30(div64(5ull << 54, (uint64_t)ed * 8000u));
    for (i = 0; i < N; i++) {
        int64_t d, c, vac;
        if (st == 1) {
            e += step;
            if (e >= (int64_t)one) { e = (int64_t)one; st = 2; }
        } else
            e -= (e * rdec) >> 30;
        if (st == 1) {
            int64_t e2 = (e * e) >> 30;
            t = (e2 * (int64_t)((3ull << 30) - 2 * e)) >> 30;
        } else
            t = e;
        d = t - fa;
        c = d > 0 ? aAtt : (aDec * (429496730 + ((1717986918 * fa) >> 30))) >> 30;
        fa += (d * c) >> 30;
        d = t - sl;
        c = d > 0 ? aAtt / 2 : (aDec * 214748365) >> 30;
        sl += (d * c) >> 30;
        vac = (697932186 * fa + 375809638 * sl) >> 30;
        while (col < EC && col * (N - 1) / 314u <= i) {
            int64_t ee = t >> 15, vv = vac >> 15;
            eg_env[col] = (uint16_t)(ee > 32768 ? 32768 : ee < 0 ? 0 : ee);
            eg_vac[col] = (uint16_t)(vv > 32768 ? 32768 : vv < 0 ? 0 : vv);
            col++;
        }
    }
    while (col < EC) { eg_env[col] = eg_env[col - 1]; eg_vac[col] = eg_vac[col - 1]; col++; }
    eg_T24 = T;
    eg_N = N;
    {
        uint32_t a = (uint32_t)(((uint64_t)atk * 8000u + (1u << 23)) >> 24);
        if (a > N - 1) a = N - 1;
        eg_at = (int32_t)((a * 314u + (N - 1) / 2) / (N - 1));
    }
}

#define EX16(col) ((((col) * 11202) + 207360) >> 10)           /* canvas column -> x, 1/16 px */
#define EY16(v) (3179 - ((((int32_t)(v)) * 1843) >> 16))      /* v in Q15 -> y, 1/16 px */

static void stroke_build(const int32_t *y16, int r16, int16_t *lo, int16_t *hi)
{
    int j, i;
    for (i = 0; i < NS; i++) { lo[i] = 32767; hi[i] = -32768; }
    for (j = 0; j < EC - 1; j++) {
        int32_t xa = EX16(j), xb = EX16(j + 1), ya = y16[j], yb = y16[j + 1];
        int32_t dy = yb - ya, ady = dy < 0 ? -dy : dy, n = (xb - xa + 2) / 3, k;
        if ((ady + 7) / 8 > n) n = (ady + 7) / 8;
        for (k = 0; k <= n; k++) {
            int32_t cx = xa + (xb - xa) * k / n, cy = ya + dy * k / n;
            int ia = (cx - r16 - 114) / 4, ib = (cx + r16 - 114) / 4 + 1;
            if (ia < 0) ia = 0;
            if (ib > NS - 1) ib = NS - 1;
            for (i = ia; i <= ib; i++) {
                int32_t ddx = 114 + 4 * i - cx, h;
                if (ddx < 0) ddx = -ddx;
                if (ddx >= r16) continue;
                h = (int32_t)isqrt32((uint32_t)(r16 * r16 - ddx * ddx));
                if (cy - h < lo[i]) lo[i] = (int16_t)(cy - h);
                if (cy + h > hi[i]) hi[i] = (int16_t)(cy + h);
            }
        }
    }
}

static void env_prepare(void)
{
    int32_t key[5] = { PV[P_ATK], PV[P_EDEC], PV[P_DECAY], PV[P_OPEN], PV[P_MODE] }, i;
    int32_t ye[EC], yv[EC];
    if (key[0] != eg_key[0] || key[1] != eg_key[1] || key[2] != eg_key[2])
        env_sim();
    if (key[0] == eg_key[0] && key[1] == eg_key[1] && key[2] == eg_key[2] && key[3] == eg_key[3] && key[4] == eg_key[4])
        return;
    for (i = 0; i < 5; i++) eg_key[i] = key[i];
    eg_open = PV[P_MODE] == 0 ? (PV[P_OPEN] * 32768 + 500) / 1000 : 0;
    for (i = 0; i < EC; i++) {
        int32_t v = eg_open + (((32768 - eg_open) * (int32_t)eg_vac[i]) >> 15);
        ye[i] = EY16(eg_env[i]);
        yv[i] = EY16(v);
    }
    stroke_build(ye, 13, eg_c_lo, eg_c_hi);
    stroke_build(yv, 19, eg_v_lo, eg_v_hi);
    for (i = 0; i < NS; i++) {                         /* the fill's upper edge: yv interpolated per sub-sample */
        int32_t q = ((114 + 4 * i) * 1024 - 207360) * 256 / 11202, c = q >> 8, f = q & 255;
        if (q < 0 || c >= EC - 1) { eg_fy_ok[i] = 0; continue; }
        eg_fy[i] = (int16_t)(yv[c] + (((yv[c + 1] - yv[c]) * f) >> 8));
        eg_fy_ok[i] = 1;
    }
}

static void env_tick(uint32_t ms) { eg_now = ms; }
void ui_env_retrigger(void) { eg_t0 = eg_now; }               /* a note-on restarts the red dot */

static void env_draw(void)
{
    int i, k, x;
    uint64_t period_ms = ((uint64_t)eg_T24 * 1000u >> 24) + 600u, tm = (eg_now - eg_t0) % (uint32_t)period_ms;
    /* axis: #333, 1 canvas px, x 8 .. 322 */
    gfx_cov_begin();
    for (x = 12; x <= 227; x++)
        gfx_cov_add_span(x, 3179 - 5, 3179 + 5, 256);
    gfx_cov_flush(0x333333u);
    /* attack marker: dashed #555, 4 on / 4 off from y = 8 */
    {
        int32_t xc = EX16(eg_at);
        for (x = (xc - 6) >> 4; x <= (xc + 6) >> 4; x++) {
            int32_t l = xc - 5 > x * 16 ? xc - 5 : x * 16, h = xc + 6 < (x + 1) * 16 ? xc + 6 : (x + 1) * 16;
            if (h <= l) continue;
            for (k = 0; k < 13; k++)
                gfx_cov_add_span(x, 2032 + (8 + 8 * k) * 1024 / 100, 2032 + (12 + 8 * k) * 1024 / 100, (h - l) * 16);
        }
        gfx_cov_flush(0x555555u);
    }
    for (i = 0; i < NS; i++)
        if (eg_fy_ok[i])
            gfx_cov_add_span(7 + i / 4, eg_fy[i], 3179, 64);
    gfx_cov_flush(0x212120u);
    for (i = 0; i < NS; i++)
        gfx_cov_add_span(7 + i / 4, eg_c_lo[i], eg_c_hi[i] + 1, 64);
    gfx_cov_flush(0x4a4a4au);
    for (i = 0; i < NS; i++)
        gfx_cov_add_span(7 + i / 4, eg_v_lo[i], eg_v_hi[i] + 1, 64);
    gfx_cov_flush(COL_TEXT);
    if ((uint64_t)tm * 1000u < ((uint64_t)eg_T24 * 1000u >> 24) * 1000u) {          /* the red dot, 8 samples a ms */
        uint32_t idx = tm * 8u;
        int32_t cq = (int32_t)div64((uint64_t)idx * 314u * 256u, eg_N - 1), c = cq >> 8, f = cq & 255, v;
        if (c >= EC - 1) { c = EC - 2; f = 255; }
        v = eg_vac[c] + ((((int32_t)eg_vac[c + 1] - eg_vac[c]) * f) >> 8);
        v = eg_open + (((32768 - eg_open) * v) >> 15);
        gfx_disc(3240 + ((cq * 175) >> 8), 50864 - ((v * 14746) >> 15), 963, 901, COL_RED);
    }
    {                                                   /* "T s" right-aligned at x = 322, baseline y = 22 */
        uint32_t cs = (uint32_t)(((uint64_t)eg_T24 * 100u + (1u << 23)) >> 24);
        char s[12], one[2] = { 0, 0 };
        int n = 0, p;
        int32_t w = 0, x64;
        if (cs >= 1000u) s[n++] = (char)('0' + cs / 1000u % 10u);
        s[n++] = (char)('0' + cs / 100u % 10u);
        s[n++] = '.';
        s[n++] = (char)('0' + cs / 10u % 10u);
        s[n++] = (char)('0' + cs % 10u);
        s[n++] = ' ';
        s[n++] = 's';
        s[n] = 0;
        for (p = 0; p < n; p++) w += (s[p] == '.' || s[p] == ' ') ? 268 : 535;
        x64 = 14550 - w;
        for (p = 0; p < n; p++) {
            one[0] = s[p];
            gfx_text(F_GNUM, one, x64, 141, COL_LABEL, 0, 0, SCR_W);
            x64 += (s[p] == '.' || s[p] == ' ') ? 268 : 535;
        }
    }
}
