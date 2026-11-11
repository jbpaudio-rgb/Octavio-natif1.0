/* SPDX-License-Identifier: MPL-2.0 */
/* The scope graphs of the engine pages, ported from the prototype's dengine() (mixer / oscillator waveforms, the filter's
 * response, the envelopes) and ddrum() (a drum voice's decay and pitch sweep) in fixed point, on the 330 x 120 canvas.
 * Strokes reuse the ENV page's two stroke buffers (eg_c, eg_v: only one page is on the screen at a time).
 * Included from ui.c after envgraph.c. */
#define EW 330
#define CXQ(xq8) ((int32_t)((((int32_t)(xq8) * 11202) >> 8) + 117760) >> 10)       /* canvas x (Q8) -> 1/16 px */
#define CYQ(yq8) GYC16(yq8)                                                        /* canvas y (Q8) -> 1/16 px */
static int32_t epx[EW + 2], epy[EW + 2];                                           /* a polyline, 1/16 px */

static int32_t sdiv(int64_t n, int64_t d)        /* the target has no 64-bit divide (envgraph.c: div64) */
{
    int neg = (n < 0) != (d < 0);
    uint64_t q = div64((uint64_t)(n < 0 ? -n : n), (uint64_t)(d < 0 ? -d : d));
    return neg ? -(int32_t)q : (int32_t)q;
}
static int32_t lg2q16(uint64_t x, int qin)       /* log2(x / 2^qin) in Q16, x > 0 */
{
    int n = 63, i;
    uint64_t m;
    int32_t r, fr = 0;
    while (n > 0 && !((x >> n) & 1u)) n--;
    m = n >= 30 ? x >> (n - 30) : x << (30 - n);              /* mantissa in [1, 2), Q30 */
    for (i = 15; i >= 0; i--) {
        m = (m * m) >> 30;
        if (m >= (2ull << 30)) { m >>= 1; fr |= 1 << i; }
    }
    r = (n - qin) * 65536 + fr;
    return r;
}
static int32_t ex15(int32_t neg_q16)             /* 2^x in Q15 */
{
    return (int32_t)exp2_fx(neg_q16, 15);
}
static int32_t expm_q15(int32_t z_q16)           /* e^-z, z >= 0 (Q16), Q15 */
{
    return (int32_t)exp2_fx(-(int32_t)(((int64_t)z_q16 * 94548) >> 16), 15);
}

/* ---------------------------------------------------------------- waveforms ---- */
static int32_t wv(uint32_t ph, int ty, uint32_t w16)        /* wvf(): ph, w in Q16 turns; Q15 */
{
    if (ty == 0) { int32_t d = (int32_t)ph - 32768; if (d < 0) d = -d; return 32768 - 2 * d; }
    if (ty == 1) return (int32_t)ph - 32768;
    return ph < (ty == 2 ? 32768u : w16) ? 32767 : -32768;
}
static int32_t hsh(int32_t i)                    /* the prototype's hs(): a pseudo-random value in [-1, 1), Q15 */
{
    uint32_t z = (uint32_t)i * 2654435761u;
    z ^= z >> 15; z *= 2246822519u; z ^= z >> 13; z *= 3266489917u; z ^= z >> 16;
    return (int32_t)(z & 0xFFFFu) - 32768;
}
static int32_t fdiv_i(int32_t a, int32_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
static int32_t nz_b0(int ty, int32_t i, int32_t sd)
{
    int32_t a = hsh(i + sd);
    switch (ty) {
    case 0: return a;
    case 1: return (a >> 1) + (hsh(fdiv_i(i, 4) + sd + 11) >> 1);
    case 2: return (hsh(fdiv_i(i, 8) + sd + 5) * 7 / 10) + (hsh(fdiv_i(i, 3) + sd + 9) * 3 / 10);
    case 3: return (a - hsh(i - 1 + sd)) >> 1;
    }
    return hsh(i + sd * 3) > 31457 ? a : 0;                   /* .96 */
}
static int32_t nz_at(int ty, int tone, int crush, int32_t sd, int x)        /* mkNz(): the noise at canvas x, Q15 */
{
    int32_t cs8 = 768 + ((crush * crush / 1000) * 7680) / 1000;     /* 3 + crush^2 * 30 canvas px, Q8 */
    int32_t i = (x * 256) / cs8, a = nz_b0(ty, i, sd), j;
    if (tone < 500) {
        int n = 1 + ((500 - tone) * 10) / 1000;
        a = 0;
        for (j = 0; j < n; j++) a += nz_b0(ty, i - j, sd);
        a /= n;
    } else if (tone > 500) {
        int32_t m = 0, k = (tone - 500) * 2 > 1000 ? 1000 : (tone - 500) * 2;
        for (j = 1; j <= 8; j++) m += nz_b0(ty, i - j, sd);
        a -= (m / 8) * k / 1000;
    }
    return a > 32767 ? 32767 : a < -32768 ? -32768 : a;
}
static uint32_t ep_time_u(void)                  /* t * .4 turns, Q16, wrapped (the scope scrolls .4 cycles a second) */
{
    uint32_t ms = arp_now;
    return (ms / 1000u) * 26214u + ((ms % 1000u) * 26214u) / 1000u;
}
static uint32_t ep_m(int oct_q16) { return exp2_fx(oct_q16, 16); }          /* 2^oct as Q16 */
static int32_t ep_sq_q16(int d1000, int k_q16) { return (int32_t)(((int64_t)d1000 * d1000 * k_q16) >> 16); }     /* d^2 * k, d in thousandths */
static uint32_t ep_frac(uint32_t u, uint32_t m) { return (uint32_t)(((uint64_t)u * m) >> 16) & 0xFFFFu; }

static const int32_t PST_Q15[7][4] = { { 32768, 0, 0, 0 }, { 22938, 22938, 0, 0 }, { 0, 32768, 0, 0 }, { 0, 22938, 22938, 0 },
                                       { 0, 0, 32768, 0 }, { 22938, 0, 22938, 0 }, { 32768, 0, 0, 32768 } };
static void ep_pws(int v1000, int32_t *w)        /* pws(): the shape's weights (saw, tri, pulse, sync), Q15 */
{
    int32_t x = ((v1000 < 0 ? 0 : v1000) * 6 * 65536) / 1000, f, k;
    int i;
    if (x > 393215) x = 393215;                                    /* 5.9999 */
    i = x >> 16; f = x & 0xFFFF;
    for (k = 0; k < 4; k++) w[k] = PST_Q15[i][k] + (((PST_Q15[i + 1][k] - PST_Q15[i][k]) * f) >> 16);
}

/* the mixer / oscillator graph: the sum (MIXER) or one oscillator over a grey sum (OSCn); 166 points, x every 2 canvas px */
static int ep_gy(int32_t q15, int32_t amp)       /* canvas y (Q8) of 60 - f * amp, amp in canvas px Q8 */
{
    return 15360 - (int32_t)(((int64_t)q15 * amp) >> 15);
}
static void epg_scope(void)
{
    int x, n = 0, e = EP.eng, oi = (EP.idx >= EPI_OSC1 && EP.idx <= EPI_OSC4) ? EP.idx - EPI_OSC1 : -1, t = voices_sel(), nosc = 4, j;
    uint32_t ut = ep_time_u(), pw[3], m[3];
    int32_t sd = (int32_t)((arp_now / 40u) * 7919u), lvl[4], ty[3] = { 0, 0, 0 }, W3[4], WB[4];
    int nty, ntone, ncr, sync = 0, sw = 0;
    uint32_t mS = 0, rAB = 0;
    int32_t sumL;
    static int32_t gy[EW / 2 + 2], cy[EW / 2 + 2];
    if (e == ENG_MOOG) {
        for (j = 0; j < 3; j++) {
            int d = (voices_param_get(t, e, MG_T1 + 5 * j) - 500) * 2, sh = ep_sq_q16(d, 2505);
            ty[j] = voices_param_get(t, e, MG_W1 + 5 * j);
            m[j] = ep_m((voices_param_get(t, e, MG_R1 + 5 * j) - 2) * 65536 + (d < 0 ? -sh : sh));
            pw[j] = 32768u - (uint32_t)voices_param_get(t, e, MG_P1 + 5 * j) * 1932735u / 65536u;
            lvl[j] = voices_param_get(t, e, MG_L1 + 5 * j);
        }
        nty = voices_param_get(t, e, MG_NTYPE); ntone = voices_param_get(t, e, MG_NTONE); ncr = voices_param_get(t, e, MG_NCRUSH);
        lvl[3] = voices_param_get(t, e, MG_NLEV);
    } else {
        int d, sh, ts;
        ep_pws(voices_param_get(t, e, PR_WAVE), W3);
        ep_pws(voices_param_get(t, e, PR_BWAVE) * 5 / 6, WB);
        d = (voices_param_get(t, e, PR_TUNE) - 500) * 2; sh = ep_sq_q16(d, 4295);
        m[0] = ep_m((voices_param_get(t, e, PR_RNG) - 2) * 65536 + (d < 0 ? -sh : sh));
        d = (voices_param_get(t, e, PR_BTUNE) - 500) * 2; sh = ep_sq_q16(d, 4295);
        m[1] = ep_m((voices_param_get(t, e, PR_BRNG) - 2) * 65536 + (d < 0 ? -sh : sh));
        rAB = (uint32_t)div64((uint64_t)m[0] << 16, m[1] ? m[1] : 1);          /* mA / mB: the synced A restarts with B */
        pw[0] = 32768u - (uint32_t)voices_param_get(t, e, PR_PW) * 1932735u / 65536u;
        pw[1] = 32768u - (uint32_t)voices_param_get(t, e, PR_BPW) * 1932735u / 65536u;
        pw[2] = 32768u - (uint32_t)voices_param_get(t, e, PR_SP) * 1932735u / 65536u;
        sync = W3[3] > 16384;
        sw = voices_param_get(t, e, PR_SW);
        ts = (voices_param_get(t, e, PR_ST) - 500) * 2; sh = ep_sq_q16(ts, 2505);
        mS = ep_m(-voices_param_get(t, e, PR_SO) * 65536 - 65536 + (ts < 0 ? -sh : sh));
        lvl[0] = voices_param_get(t, e, PR_LA); lvl[1] = voices_param_get(t, e, PR_LB); lvl[2] = voices_param_get(t, e, PR_SL);
        lvl[3] = voices_param_get(t, e, PR_NLEV);
        nty = voices_param_get(t, e, PR_NTYPE); ntone = voices_param_get(t, e, PR_NTONE); ncr = voices_param_get(t, e, PR_NCRUSH);
    }
    sumL = lvl[0] + lvl[1] + lvl[2] + lvl[3];
    if (sumL < 500) sumL = 500;
    for (x = 0; x <= EW; x += 2, n++) {
        uint32_t u = (uint32_t)(((uint32_t)x * 131072u) / 330u) + ut;
        int32_t o[4], acc = 0;
        if (e == ENG_MOOG) {
            for (j = 0; j < 3; j++) o[j] = wv(ep_frac(u, m[j]), (int)ty[j], pw[j]);
        } else {
            uint32_t pa, pb, ps;
            if (sync) {
                uint32_t ub = (uint32_t)(((uint64_t)u * m[1]) >> 16) & 0xFFFFu;
                pa = (uint32_t)(((uint64_t)ub * rAB) >> 16) & 0xFFFFu;
            } else
                pa = ep_frac(u, m[0]);
            pb = ep_frac(u, m[1]);
            ps = ep_frac(u, mS);
            o[0] = (int32_t)(((int64_t)wv(pa, 1, 0) * W3[0] + (int64_t)wv(pa, 0, 0) * W3[1] + (int64_t)wv(pa, 3, pw[0]) * W3[2]) >> 15);
            o[1] = (int32_t)(((int64_t)wv(pb, 1, 0) * WB[0] + (int64_t)wv(pb, 0, 0) * WB[1] + (int64_t)wv(pb, 3, pw[1]) * WB[2]) >> 15);
            o[2] = sw == 0 ? sin_q15(ps << 16) : wv(ps, sw == 1 ? 0 : 3, pw[2]);
        }
        o[3] = nz_at(nty, ntone, ncr, sd, x);
#ifdef EPDBG
        if (x % 40 == 0) fprintf(stderr, "x=%d u=%u m0=%u m1=%u o=%d %d %d %d W3=%d %d %d %d WB=%d %d %d %d lvl=%d %d\n", x, u, m[0], m[1], o[0], o[1], o[2], o[3], W3[0], W3[1], W3[2], W3[3], WB[0], WB[1], WB[2], WB[3], lvl[0], lvl[1]);
#endif
        for (j = 0; j < nosc; j++) acc += o[j] * lvl[j] / 1;
        {
            int32_t mix = acc / sumL;
            if (oi < 0)
                cy[n] = ep_gy(mix, 12386);                       /* H * .42 * .96 = 48.384 canvas px */
            else {
                gy[n] = ep_gy(mix, 6144);                       /* H * .2 */
                cy[n] = ep_gy(o[oi], 12902);                    /* H * .42 */
            }
        }
    }
    if (oi >= 0) {
        for (x = 0; x < n; x++) { epx[x] = CXQ(x * 2 * 256); epy[x] = CYQ(gy[x]); }
        poly_stroke(epx, epy, n, 11, &eg_c);
    }
    for (x = 0; x < n; x++) { epx[x] = CXQ(x * 2 * 256); epy[x] = CYQ(cy[x]); }
    poly_stroke(epx, epy, n, 19, &eg_v);
}
static void epg_scope_draw(void)
{
    int i, oi = (EP.idx >= EPI_OSC1 && EP.idx <= EPI_OSC4);
    gfx_cov_begin();
    for (i = 0; i < NS; i += 4)
        gfx_cov_add_span(7 + i / 4, 2645 - 5, 2645 + 5, 256);
    gfx_cov_flush(0x333333u);
    if (oi) stroke_paint(&eg_c, 0x3a3a3au);
    stroke_paint(&eg_v, COL_TEXT);
}

/* ---------------------------------------------------------------- the filter's response ---- */
static int32_t ep_db_ymap(int32_t log2mag_q16)     /* -20 log10 |H| -> canvas y (Q8): H - 8 - clamp((d + 54) / 78) * (H - 18) */
{
    int32_t d = -(int32_t)(((int64_t)log2mag_q16 * 394577) >> 16);      /* -6.0206 * log2 |H|, Q16 dB (394577 = 6.0206 * 65536) */
    int32_t t = ((d + 54 * 65536) / 78);                                 /* Q16 of (d + 54) / 78 */
    if (t < 0) t = 0;
    if (t > 65536) t = 65536;
    return 28672 - (int32_t)(((int64_t)t * 102 * 256) >> 16);          /* (112 - t * 102) canvas px, Q8 */
}
static int32_t ep_log2mag(int32_t lz_q16, int32_t k_q16)      /* log2 |(1 + jz)^4 + k|, z = 2^lz */
{
    int64_t z = exp2_fx(lz_q16, 16);                              /* Q16 */
    int64_t a = 65536 + ((z * z) >> 16);                          /* 1 + z^2 */
    int32_t l2s = 2 * (lg2q16((uint64_t)a, 16));                  /* log2 S, S = (1 + z^2)^2 */
    int64_t u, re2, im2, T, S, S2, N, r;
    if (z > 16 * 65536) return l2s;
    u = (z * z) >> 16;
    re2 = 65536 - u; im2 = 2 * z;
    T = (re2 * re2 - im2 * im2) >> 16;                            /* Re (1 + jz)^4, Q16 */
    S = (a * a) >> 16;                                            /* S */
    S2 = ((S >> 4) * (S >> 4)) >> 8;                              /* S^2 */
    N = ((2 * (int64_t)k_q16 * T) >> 16) + (((int64_t)k_q16 * k_q16) >> 16);
    if (S2 < 1) return l2s;
    r = div64((uint64_t)(N < 0 ? -N : N) << 16, (uint64_t)S2);   /* |N| / S^2, Q16 */
    if (N < 0) r = -r;
    if (r < -60000) r = -60000;
    return l2s + (lg2q16((uint64_t)(65536 + r), 16) >> 1);
}
static void epg_filter(void)
{
    int t = voices_sel(), e = EP.eng, x, n = 0;
    int cut = e == ENG_MOOG ? voices_param_get(t, e, MG_CUT) : e == ENG_TB3 ? voices_param_get(t, e, TB_CUT) : e == ENG_SSAW ? voices_param_get(t, e, SS_CUT) : e == ENG_GREM ? voices_param_get(t, e, GM_CUT) : voices_param_get(t, e, PR_CUT);
    int res = e == ENG_MOOG ? voices_param_get(t, e, MG_RES) : e == ENG_TB3 ? voices_param_get(t, e, TB_RES) : e == ENG_SSAW ? voices_param_get(t, e, SS_RES) : e == ENG_GREM ? voices_param_get(t, e, GM_RES) : voices_param_get(t, e, PR_RES);
    int env = e == ENG_MOOG ? voices_param_get(t, e, MG_ENV) : e == ENG_TB3 ? voices_param_get(t, e, TB_ENV) : e == ENG_SSAW ? voices_param_get(t, e, SS_ENV) : e == ENG_GREM ? 0 : voices_param_get(t, e, PR_ENV);
    int32_t l2fc, l2fc2, k_q16, l2f20 = lg2q16(20, 0), l2k = lg2q16(1000, 0), cap = lg2q16(18000, 0);
    static int32_t ya[EW / 2 + 2], yb[EW / 2 + 2];
    if (e == ENG_MOOG || e == ENG_SSAW || e == ENG_GREM) {                    /* the new engines: the same cutoff law as the Minimoog's (their own resonance scale; a 4-pole curve is drawn for their 2-pole filter: indicative) */
        l2fc = lg2q16(30, 0) + (cut * 602931) / 1000;                         /* 30 * 2^(cut * 9.2) */
        k_q16 = (int32_t)(res * (e == ENG_MOOG ? 267 : 196));                 /* 4 * res * 1.02 */
        l2fc2 = l2fc + (env * 327680) / 1000;                                 /* * 2^(env * 5) */
    } else if (e == ENG_TB3) {
        l2fc = lg2q16(150, 0) + (cut * 283100) / 1000;                        /* 150 Hz .. 3 kHz */
        k_q16 = (int32_t)(res * 262);                                         /* the ladder's k up to ~4 (the loop's high-pass lets it ring a bit beyond) */
        l2fc2 = l2fc + (env * 163840) / 1000;                                 /* up to 2.5 octaves of sweep */
    } else {
        l2fc = lg2q16(40, 0) + (cut * 576717) / 1000;                         /* 40 * 2^(cut * 8.8) */
        k_q16 = (int32_t)(res * 257);                                         /* 4 * res * .98 */
        l2fc2 = l2fc + (env * 360448) / 1000;                                 /* * 2^(penv * 5.5) */
    }
    if (l2fc2 > cap) l2fc2 = cap;
    for (x = 0; x <= EW; x += 2, n++) {
        int32_t lf = l2f20 + sdiv((int64_t)l2k * x, EW);              /* log2 of the frequency: 20 * 1000^(x / W) */
        ya[n] = CYQ(ep_db_ymap(ep_log2mag(lf - l2fc2, k_q16)));
        yb[n] = CYQ(ep_db_ymap(ep_log2mag(lf - l2fc, k_q16)));
        epx[n] = CXQ(x * 256);
    }
    poly_stroke(epx, ya, n, 11, &eg_c);
    poly_stroke(epx, yb, n, 19, &eg_v);
}
static void epg_filter_draw(void)
{
    static const int32_t GX[3] = { 19685, 47846, 76008 };                      /* canvas x (Q8) of 100 Hz, 1 kHz, 10 kHz */
    int i;
    for (i = 0; i < 3; i++)
        g_vline(CXQ(GX[i]), CYQ(4 * 256), CYQ(116 * 256), 0x333333u);
    stroke_paint(&eg_c, 0x3a3a3au);
    stroke_paint(&eg_v, COL_TEXT);
}

/* ---------------------------------------------------------------- the envelopes ---- */
static int ep_env_a, ep_env_d, ep_env_s, ep_env_r, ep_env_h, ep_env_T;       /* attack, decay, release, hold, total: seconds in Q16; sustain: thousandths */
static int32_t ep_env_at(int32_t u)               /* env(u), u in the same units; Q15 */
{
    int a = ep_env_a, d = ep_env_d, s = ep_env_s * 32768 / 1000, r = ep_env_r;
    int32_t z;
    if (u < a) return sdiv((int64_t)u * 32768, a ? a : 1);
    if (u < a + d) {
        z = sdiv((int64_t)(u - a) * 4 * 65536, d);                  /* 4 (u - a) / d, Q16 */
        return s + (int32_t)(((int64_t)(32768 - s) * expm_q15(z)) >> 15);
    }
    if (u < a + d + ep_env_h) return s + (int32_t)(((int64_t)(32768 - s) * 600) >> 15);     /* + (1 - s) e^-4 (= 600 / 32768) */
    if (r < 1) return 0;
    z = sdiv((int64_t)(u - a - d - ep_env_h) * 4 * 65536, r);
    return (int32_t)(((int64_t)s * expm_q15(z)) >> 15);
}
static void epg_env(void)
{
    int t = voices_sel(), e = EP.eng, e2 = EP.idx == EPI_ENV2, x, n = 0, sa, sd2, sr, rel_dec = 0;
    if (e == ENG_SSAW) {
        sa = voices_param_get(t, e, e2 ? SS_FATK : SS_ATK); sd2 = voices_param_get(t, e, e2 ? SS_FDEC : SS_DEC);
        ep_env_s = voices_param_get(t, e, SS_SUS); sr = voices_param_get(t, e, SS_REL);
    } else if (e == ENG_GREM) {
        sa = voices_param_get(t, e, GM_ATK); sd2 = voices_param_get(t, e, GM_DEC);
        ep_env_s = 600; sr = voices_param_get(t, e, GM_REL);                    /* its sustain is fixed at .6 */
    } else if (e == ENG_MOOG) {
        sa = voices_param_get(t, e, e2 ? MG_FATK : MG_ATK); sd2 = voices_param_get(t, e, e2 ? MG_FDEC : MG_DEC);
        ep_env_s = voices_param_get(t, e, e2 ? MG_FSUS : MG_SUS);
        sr = sd2; rel_dec = voices_param_get(t, e, MG_REL);
    } else {
        sa = voices_param_get(t, e, e2 ? PR_FATK : PR_ATK); sd2 = voices_param_get(t, e, e2 ? PR_FDEC : PR_DEC);
        ep_env_s = voices_param_get(t, e, e2 ? PR_FSUS : PR_SUS); sr = voices_param_get(t, e, e2 ? PR_FREL : PR_REL);
    }
    /* the slider -> seconds (Q16): attack .001 * 2000^s, the other times .005 * 600^s */
    ep_env_a = (int)exp2_fx(EK_ATK0 + lg(sa, K_L2000), 16);
    ep_env_d = (int)exp2_fx(EK_EDEC0 + lg(sd2, K_L600), 16);
    ep_env_r = e == ENG_MOOG ? (rel_dec ? ep_env_d : 524) : (int)exp2_fx(EK_EDEC0 + lg(sr, K_L600), 16);       /* Moog: release = decay, or .008 s */
    ep_env_h = sdiv(35 * (int64_t)(ep_env_a + ep_env_d + ep_env_r), 100) + 1311;                           /* .35 * (a + d + r) + .02 */
    ep_env_T = ep_env_a + ep_env_d + ep_env_h + ep_env_r;
    for (x = 0; x <= EW; x += 2, n++) {
        int32_t u = x < 4 ? 0 : sdiv((int64_t)(x - 4) * ep_env_T, EW - 8);
        int32_t v = ep_env_at(u);
        epx[n] = CXQ(x * 256);
        epy[n] = CYQ(28672 - (int32_t)(((int64_t)v * 102 * 256) >> 15));    /* H - 8 - env * (H - 18) */
    }
    poly_stroke(epx, epy, n, 19, &eg_v);
}
static void epg_env_draw(void)
{
    int i;
    int32_t us[3] = { ep_env_a, ep_env_a + ep_env_d, ep_env_a + ep_env_d + ep_env_h };
    int32_t qx = (int32_t)(((arp_now % 2000u) * 322u * 256u) / 2000u), u, v;              /* the red dot: 2 s a pass, canvas x = ph / T * (W - 8) + 4 */
    for (i = 0; i < 3; i++) {
        int32_t xq8 = sdiv((int64_t)us[i] * (EW - 8) * 256, ep_env_T ? ep_env_T : 1) + 4 * 256;
        g_vline(CXQ(xq8), CYQ(4 * 256), CYQ(116 * 256), 0x333333u);
    }
    stroke_paint(&eg_v, COL_TEXT);
    u = sdiv((int64_t)qx * ep_env_T, 322 * 256);
    v = ep_env_at(u);
    gfx_disc(CXQ(qx + 4 * 256) * 16, CYQ(28672 - (int32_t)(((int64_t)v * 102 * 256) >> 15)) * 16, 875, 819, COL_RED);
}

/* ---------------------------------------------------------------- a drum voice (ddrum) ---- */
static const uint32_t DBASE[3][8] = {                                           /* base decay, Q16 s */
    { 32768, 15729, 7209, 32768, 22938, 7864, 19661, 26214 },
    { 15729, 14418, 7864, 32768, 10486, 65536, 19661, 26214 },
    { 13107, 13107, 6554, 29491, 19661, 91750, 19661, 26214 } };
static void epg_drum(void)
{
    int t = voices_sel(), e = ENG_DRUM, v = ctrl_drum_voice(), x, k, ty;
    int tu = voices_param_get(t, e, DR_ID(v, DR_TUNE)), dc = voices_param_get(t, e, DR_ID(v, DR_DECAY));
    int hdv = voices_param_get(t, e, DR_ID(v, DR_HOLD)), atv = voices_param_get(t, e, DR_ID(v, DR_ATK)), pev = voices_param_get(t, e, DR_ID(v, DR_PEAK));
    int32_t hd = (hdv * 16384) / 1000, at = (atv * 2621) / 1000, pe = ((pev - 500) * 11796) / 100, base, T, tf, k1;
    uint32_t invb, invat = 0;
    k = voices_param_get(t, e, DR_ID(v, DR_KV)); ty = voices_param_get(t, e, DR_ID(v, DR_TYPE));
    k = k < 0 ? 0 : k > 2 ? 2 : k; ty = ty < 0 ? 0 : ty > 7 ? 7 : ty;
    base = (int32_t)(((int64_t)DBASE[k][ty] * exp2_fx(((dc - 500) * 262144) / 1000, 16)) >> 16);
    T = (int32_t)(((int64_t)base * 4301) >> 10);                                /* base * 4.2 */
    if (T < 7864) T = 7864;                                                      /* .12 s */
    T += hd;
    invb = (uint32_t)div64(1ull << 32, (uint64_t)(base ? base : 1));
    if (at > 0) invat = (uint32_t)div64(1ull << 32, (uint64_t)at);
    tf = (int32_t)exp2_fx(((tu - 500) * 2 * 65536) / 1000, 16);                  /* 2^((tune - .5) * 2) */
    k1 = (ty == 0 || ty > 5 || (ty == 4 && k == 2)) ? 65536 : 9830;             /* 1 or .15 */
    for (x = 0; x < EW; x++) {
        int32_t u = sdiv((int64_t)x * T, EW), uh = u > hd ? u - hd : 0, a, e1, e2, e3 = 32768, y;
        e1 = expm_q15((int32_t)(((int64_t)uh * invb) >> 16));
        e2 = 32768 - expm_q15((int32_t)(((int64_t)u * 500)));
        if (at > 0) { int64_t r = ((int64_t)u * invat) >> 16; e3 = r >= 65536 ? 32768 : (int32_t)(r >> 1); }
        a = (int32_t)(((int64_t)e1 * e2 >> 15) * e3 >> 15);
        epx[x] = CXQ(x * 256);
        y = 28416 - (int32_t)(((int64_t)a * 98 * 256) >> 15);
        epy[x] = CYQ(y);
    }
    poly_stroke(epx, epy, EW, 16, &eg_v);
    for (x = 0; x < EW; x++) {
        int32_t u = sdiv((int64_t)x * T, EW), uh = u > hd ? u - hd : 0, f, e4, e5 = 0, m, r;
        e4 = expm_q15((int32_t)(((int64_t)uh * 25)));
        m = 65536 + (int32_t)(((int64_t)pe * e4) >> 15);
        if (ty == 0) { e5 = expm_q15((int32_t)(((int64_t)u * 20))); m += (int32_t)(((int64_t)3 * e5 * 2) >> 0); }
        f = (int32_t)(((int64_t)k1 * m) >> 16);                                  /* Q16 */
        f = (int32_t)(((int64_t)f * tf) >> 16);
        f = (int32_t)(((int64_t)f * 13107) >> 16);                               /* * .2 */
        r = sdiv((int64_t)f * 65536, 85197);                             /* f / 1.3 */
        if (r > 65536) r = 65536;
        epy[x] = CYQ(28416 - (int32_t)(((int64_t)r * 94 * 256) >> 16));         /* H - 9 - r * (H - 26) = 111 - r * 94 canvas px */
    }
    poly_stroke(epx, epy, EW, 11, &eg_c);
}
static void epg_drum_draw(void)
{
    uint32_t tm = arp_now % 1600u;
    int32_t hit = (int32_t)exp2_fx(-(int32_t)(tm * 236u), 15);                   /* e^(-t / .4) */
    g_hline(7, 232, CYQ(28544), 0x2a2a2au);
    stroke_paint(&eg_v, COL_TEXT);
    stroke_paint(&eg_c, COL_RED);
    gfx_frect16(CXQ(0), 2032, CXQ(EW * 256), CYQ(4 * 256), COL_TEXT, (uint32_t)((38 * hit) >> 15));
}

/* ---------------------------------------------------------------- entry points ---- */
static void epg_prepare(void)
{
    eg_key[0] = -1;                                                              /* the ENV page's strokes are being reused */
    if (EP.eng >= ENG_TB3) {                                                     /* TB-303 / Supersaw / Gremlin: the filter curve on FILTER, the envelope on the Supersaw's and the Gremlin's envelope pages, nothing else */
        if (EP.idx == EPI_EDIT) epg_filter();
        else if ((EP.idx == EPI_ENV || EP.idx == EPI_ENV2) && EP.eng != ENG_TB3) epg_env();
        return;
    }
    if (EP.eng == ENG_DRUM && EP.idx == EPI_ARP) return;                         /* the prototype draws nothing there (dgrey() is not defined) */
    if (EP.eng == ENG_DRUM) epg_drum();
    else if (EP.idx == EPI_EDIT) epg_filter();
    else if (EP.idx == EPI_ENV || EP.idx == EPI_ENV2) epg_env();
    else epg_scope();
}
static void epg_draw(void)
{
    if (EP.eng >= ENG_TB3) {
        if (EP.idx == EPI_EDIT) epg_filter_draw();
        else if ((EP.idx == EPI_ENV || EP.idx == EPI_ENV2) && EP.eng != ENG_TB3) epg_env_draw();
        return;
    }
    if (EP.eng == ENG_DRUM && EP.idx == EPI_ARP) return;
    if (EP.eng == ENG_DRUM) epg_drum_draw();
    else if (EP.idx == EPI_EDIT) epg_filter_draw();
    else if (EP.idx == EPI_ENV || EP.idx == EPI_ENV2) epg_env_draw();
    else epg_scope_draw();
}

/* ---------------------------------------------------------------- the drum mixer: six strips (MIXER 1/2 shows 1..4 on the knobs, 2/2 shows 5, 6) ---------------- */
static const char *epm_name(int v)
{
    int t = voices_sel(), k = voices_param_get(t, ENG_DRUM, DR_ID(v, DR_KV)), y = voices_param_get(t, ENG_DRUM, DR_ID(v, DR_TYPE));
    k = k < 0 ? 0 : k > 2 ? 2 : k; y = y < 0 ? 0 : y > 7 ? 7 : y;
    return DVN[k][y];
}
static void epm_draw(void)
{
    int i, j, a0 = page == PG_GLO ? 0 : 4, na = page == PG_GLO ? 4 : 2, t = voices_sel();
    for (i = 0; i < 6; i++) {
        int32_t xl16 = 115 + (i * 3056) / 5;                                       /* the strip's left edge: 7.19 + i * 38.2 px */
        int x0 = (xl16 + 8) >> 4, x1 = (xl16 + 554 + 8) >> 4, vol = voices_param_get(t, ENG_DRUM, DR_ID(i, DR_LEVEL)), n, cy;
        int hw0 = (xl16 + 114 + 8) >> 4, hw1 = (xl16 + 440 + 8) >> 4, hc0 = (xl16 + 153 + 8) >> 4, hc1 = (xl16 + 401 + 8) >> 4, hk = (xl16 - 11 + 8) >> 4;
        const char *nm = epm_name(i);
        int32_t w;
        uint32_t bc = (i >= a0 && i < a0 + na) ? COL_RED : 0x2a2a2au;
        gfx_rect(x0, 7, x1 - x0, 192, bc, 256);                                    /* the strip: 1 px border, #0d0d0d */
        gfx_rect(x0 + 1, 8, x1 - x0 - 2, 190, 0x0d0d0du, 256);
        gfx_rect(x0 + 1, 8, x1 - x0 - 2, 24, COL_TEXT, 256);                       /* the name plate */
        gfx_rect(x0 + 1, 8, x1 - x0 - 2, 1, 0x333333u, 256);
        w = (int32_t)ui_strlen(nm) * FONTS[F_HEN].adv64;
        gfx_text(F_HEN, nm, (xl16 + 277) * 4 - (w >> 1), 24, 0x000000u, 0, 0, 240);
        gfx_rect(x0 + 1, 38, x1 - x0 - 2, 1, 0x333333u, 256);                      /* the fader's top rule */
        gfx_rect(hw0, 39, hw1 - hw0, 153, 0x333333u, 256);                         /* the well */
        gfx_rect(hw0 + 1, 40, hw1 - hw0 - 2, 151, 0x050505u, 256);
        n = (vol * 14 + 500) / 1000;                                               /* Math.round(level * 14) cells on */
        for (j = 0; j < 14; j++)
            gfx_rect(hc0, HC_Y0[j], hc1 - hc0, HC_Y1[j] - HC_Y0[j], j < n ? COL_TEXT : 0x1c1c1cu, 256);
        cy = 67 + ((800 - vol) * 141 + (vol > 800 ? -500 : 500)) / 1000;           /* the cap: top row */
        gfx_rect(hk, cy, 36, 11, 0x000000u, 256);
        gfx_frect16(hk * 16, (cy + 11) * 16, (hk + 36) * 16, (cy + 11) * 16 + 24, 0x000000u, 256);
        gfx_frect16(hk * 16, (cy + 11) * 16 + 24, (hk + 36) * 16, (cy + 11) * 16 + 32, 0x000000u, 118);
        for (j = 0; j < 9; j++) {
            uint32_t tt = (uint32_t)(j * 2 + 1) * 128u / 9u;
            uint32_t r = 242u - (53u * tt >> 8), g = 239u - (54u * tt >> 8), b = 230u - (58u * tt >> 8);
            gfx_rect(hk + 1, cy + 1 + j, 34, 1, (r << 16) | (g << 8) | b, 256);
        }
        gfx_rect(hk + 1, cy + 5, 34, 2, COL_RED, 256);
    }
}
static void epm_encoder(int k, int steps)       /* knob k edits the level of strip a0 + k (MIXER 1/2: 1..4, 2/2: 5, 6) */
{
    int a0 = page == PG_GLO ? 0 : 4, na = page == PG_GLO ? 4 : 2, v = a0 + k, i, n = steps < 0 ? -steps : steps, t = voices_sel();
    if (k >= na) return;
    for (i = 0; i < n; i++) {
        int id = DR_ID(v, DR_LEVEL), cur = voices_param_get(t, ENG_DRUM, id);
        int32_t g = (cur * 65536 + 500) / 1000, base, nv, raw;
        base = (ep_wheel_id[k] == 5000 + id && g == ep_wg[k]) ? ep_wheel[k] : g;
        nv = base + (steps < 0 ? -1 : 1) * (65536 / 50);
        if (nv < 0) nv = 0;
        if (nv > 65536) nv = 65536;
        raw = nv * 1000;
        voices_param(t, ENG_DRUM, id, ((raw + (10 << 15)) / (10 << 16)) * 10);
        ep_wheel[k] = nv;
        ep_wg[k] = (voices_param_get(t, ENG_DRUM, id) * 65536 + 500) / 1000;
        ep_wheel_id[k] = 5000 + id;
    }
}
