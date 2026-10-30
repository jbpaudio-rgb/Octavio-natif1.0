/* SPDX-License-Identifier: MPL-2.0 */
/* LFO page graph (the prototype's dlfo): the selected LFO's wave scrolling past the red dot (grey: full scale, white: scaled by Amount),
 * or for S&H sources the sampled-and-smoothed value as a rolling history.  Included from ui.c. */
#include "masks_gen.h"
#define LC 331                                    /* canvas columns 0 .. 330 */
static uint32_t lfo_ph[4];                        /* LFO 1..4 phase, turns */
static uint32_t lfo_acc;                          /* ms accumulated towards the next 1/60 s step */
static uint32_t lfo_rng = 12345u;
static int16_t sh_hist[110];                      /* S&H history, Q15 */
static int sh_n, sh_src = -1;
static int32_t sh_t, sh_v, sh_s;                  /* time left (Q16 s), target, smoothed value (Q15) */
static stroke_t lg_grey, lg_white;

static int lfo_pid(int off) { return LFO_BASE[lfo_src] + off; }          /* 0 dest, 1 shape|smooth, 2 rate, 3 amount, 4 sync */
static uint32_t lfo_rate_q16(int src)                                    /* Hz * 65536, as dlfo(): synced -> bpm / 60 * division */
{
    int rid = LFO_BASE[src] + 2, s = PV[rid];
    if (PV[rid + 2]) {
        static const uint16_t DIVQ[8] = { 4096, 8192, 16384, 32768, 0, 0, 0, 0 };       /* x 65536: .0625 .125 .25 .5 1 2 4 8 */
        int di = s * 8 / 1000;
        uint32_t d = di > 7 ? 7 : di;
        uint32_t d16 = d < 4 ? DIVQ[d] : 65536u << (d - 4);
        return ((uint32_t)PV[P_BPM] * d16) / 60u;
    }
    return exp2_fx(-K_LOG2_20 + ((s * 632020) / 1000), 16);
}
static int32_t wf_q15(uint32_t ph, int t)         /* the prototype's wf(p, t) */
{
    int32_t q = (int32_t)(ph >> 17);              /* 0..32767 */
    if (t == 0) return sin_q15(ph);
    if (t == 1) { int32_t d = q - 16384; return 32768 - 4 * (d < 0 ? -d : d); }
    if (t == 2) return 2 * q - 32768;
    return q < 16384 ? 32767 : -32768;
}
static int32_t amp64(void)                         /* the white wave's amplitude, 1/64 px: 48 canvas px * max(.05, amount) */
{
    int32_t a = PV[lfo_pid(3)];
    return 1966 * (a < 50 ? 50 : a) / 1000;
}
static void lfo_tick(uint32_t dt)
{
    if (page != PG_LFO)
        return;
    if (lfo_src < 4) {
        uint32_t r = lfo_rate_q16(lfo_src);
        if (r > 12u * 65536u) r = 12u * 65536u;
        lfo_ph[lfo_src] += (uint32_t)(((uint64_t)r * dt * 4294967u) >> 16);
        return;
    }
    if (sh_src != lfo_src) { sh_src = lfo_src; sh_n = 0; }
    lfo_acc += dt;
    while (lfo_acc >= 17u) {                      /* a frame of the prototype: 1/60 s */
        int64_t k;
        int32_t x;
        lfo_acc -= 17u;
        sh_t -= 1092;                             /* 1/60 s in Q16 */
        if (sh_t <= 0) {
            uint32_t r = lfo_rate_q16(lfo_src), per;
            lfo_rng ^= lfo_rng << 13; lfo_rng ^= lfo_rng >> 17; lfo_rng ^= lfo_rng << 5;
            per = r < 3277u ? 65536u * 20u : 0xFFFFFFFFu / r;                    /* 1 / max(.05, rate) s, Q16 */
            sh_t += (int32_t)per;
            sh_v = (int32_t)(lfo_rng >> 17) * 2 - 32768;
        }
        x = 65536000 / (120 + 24 * PV[lfo_pid(1)]);                      /* dt / tau, Q16 */
        k = 65536 - (int64_t)exp2_fx(-(int32_t)(((int64_t)x * 94548) >> 16), 16);                   /* 1 - exp(-x), Q16 */
        sh_s += (int32_t)(((int64_t)(sh_v - sh_s) * k) >> 16);
        if (sh_n == 110) { int i; for (i = 1; i < 110; i++) sh_hist[i - 1] = sh_hist[i]; sh_n--; }
        sh_hist[sh_n++] = (int16_t)sh_s;
    }
}
static void lfo_prepare(void)
{
    int32_t x[LC], y[LC], i, kw = amp64();
    if (lfo_src >= 4) {
        for (i = 0; i < sh_n; i++) {
            x[i] = GX16(330 - (sh_n - 1 - i) * 3);
            y[i] = G_AXIS - ((sh_hist[i] * kw) >> 17);
        }
        if (sh_n > 1) poly_stroke(x, y, sh_n, 16, &lg_white);
        else { int j; for (j = 0; j < NS; j++) { lg_white.lo[j] = 32767; lg_white.hi[j] = -32768; } }
        return;
    }
    {
        int shp = PV[lfo_pid(1)];
        uint32_t ph = lfo_ph[lfo_src];
        for (i = 0; i < LC; i++) {
            uint32_t u = ph + (uint32_t)(i - 66) * 39045158u;
            int32_t w = wf_q15(u, shp);
            x[i] = GX16(i);
            y[i] = G_AXIS - ((w * 491) >> 15);
        }
        poly_stroke(x, y, LC, 11, &lg_grey);
        for (i = 0; i < LC; i++) {
            uint32_t u = ph + (uint32_t)(i - 66) * 39045158u;
            y[i] = G_AXIS - ((wf_q15(u, shp) * kw) >> 17);
        }
        poly_stroke(x, y, LC, 16, &lg_white);
        i = 66;
        sh_v = y[66];                                                         /* (borrowed) the dot's y, 1/16 px */
    }
}
static void sync_box(int on)
{
    gfx_rect(202, 9, 8, 7, on ? COL_RED : 0x8d8a83u, 256);
    if (!on)
        gfx_rect(203, 10, 6, 5, COL_BG, 256);
    gfx_mask(MK_SYNC, 212, 10, 18, 6, on ? COL_TEXT : 0x8d8a83u);
}
static void lfo_draw(void)
{
    g_hline(7, 232, G_AXIS, 0x333333u);
    if (lfo_src >= 4) {
        if (sh_n > 1) {
            int32_t yl = G_AXIS - ((sh_hist[sh_n - 1] * amp64()) >> 17);
            stroke_paint(&lg_white, COL_TEXT);
            gfx_disc(1841 + 328 * 175, yl * 16, 875, 819, COL_RED);
        }
    } else {
        stroke_paint(&lg_grey, 0x3a3a3au);
        stroke_paint(&lg_white, COL_TEXT);
        g_vline(GX16(66), 2032, 3261, 0x555555u);
        gfx_disc(13393, sh_v * 16, 875, 819, COL_RED);
    }
}
