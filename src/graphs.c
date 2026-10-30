/* SPDX-License-Identifier: MPL-2.0 */
/* Shared helpers of the scope graphs (the prototype's 330 x 120 canvas shown at 225.63 x 76.8 px at (7.19, 126.91)).
 * Included from ui.c after NS is defined.  Units: x, y in 1/16 px; canvas px -> screen: x = 115 + xc * 10.94, y = 2032 + yc * 10.24. */
#define GX16(xc) ((((xc) * 11202) + 117760) >> 10)               /* canvas x -> 1/16 px */
#define GYC16(ycq8) (2032 + ((((int32_t)(ycq8)) * 2621 + 32768) >> 16))   /* canvas y in Q8 -> 1/16 px */
#define G_AXIS 2646                                              /* canvas y 60 */
typedef struct { int16_t lo[NS], hi[NS]; } stroke_t;
static int pc_clip;                                              /* 1: strokes are built per band, skip what cannot reach it */

/* stroke a polyline (round joins, radius r16) = union of discs along it; per sub-sample column (4 per px) the vertical extent */
static void poly_stroke(const int32_t *x16, const int32_t *y16, int n, int r16, stroke_t *s)
{
    int j, i;
    uint8_t hh[32];
    for (i = 0; i <= r16 && i < 32; i++)
        hh[i] = (uint8_t)isqrt32((uint32_t)(r16 * r16 - i * i));
    int by0 = -32768, by1 = 32767;
    if (pc_clip) { int a, b; gfx_band_rows(&a, &b); by0 = a * 16 - 32; by1 = b * 16 + 32; }
    for (i = 0; i < NS; i++) { s->lo[i] = 32767; s->hi[i] = -32768; }
    for (j = 0; j < n - 1; j++) {
        int32_t xa = x16[j], xb = x16[j + 1], ya = y16[j], yb = y16[j + 1];
        int32_t dy = yb - ya, ady = dy < 0 ? -dy : dy, np = (xb - xa + 2) / 3, k;
        if ((ady + 7) / 8 > np) np = (ady + 7) / 8;
        if (np < 1) np = 1;
        for (k = 0; k <= np; k++) {
            int32_t cx = xa + (xb - xa) * k / np, cy = ya + dy * k / np;
            if (cy + r16 < by0 || cy - r16 > by1)
                continue;
            int ia = (cx - r16 - 114) / 4, ib = (cx + r16 - 114) / 4 + 1;
            if (ia < 0) ia = 0;
            if (ib > NS - 1) ib = NS - 1;
            for (i = ia; i <= ib; i++) {
                int32_t ddx = 114 + 4 * i - cx, h;
                if (ddx < 0) ddx = -ddx;
                if (ddx >= r16) continue;
                h = hh[ddx];
                if (cy - h < s->lo[i]) s->lo[i] = (int16_t)(cy - h);
                if (cy + h > s->hi[i]) s->hi[i] = (int16_t)(cy + h);
            }
        }
    }
}
static void stroke_paint_a(const stroke_t *s, uint32_t col, uint32_t a256)
{
    int i;
    for (i = 0; i < NS; i++)
        if (s->lo[i] <= s->hi[i])
            gfx_cov_add_span(7 + i / 4, s->lo[i], s->hi[i] + 1, 64);
    if (a256 >= 256) gfx_cov_flush(col);
    else gfx_cov_flush_a(col, a256);
}
static void stroke_paint(const stroke_t *s, uint32_t col) { stroke_paint_a(s, col, 256); }
/* the graph's axis: #333, 1 canvas px, from canvas x xa to xb, at canvas y 60 (ycq8 = 15360) or any */
static void g_hline(int xa_px, int xb_px, int y16, uint32_t col)
{
    int x;
    gfx_cov_begin();
    for (x = xa_px; x <= xb_px; x++)
        gfx_cov_add_span(x, y16 - 5, y16 + 5, 256);
    gfx_cov_flush(col);
}
/* a vertical 1 canvas px line at canvas x xc over rows y0..y1 (1/16 px) */
static void g_vlinex(int xl16, int xr16, int y0, int y1, uint32_t col)     /* a vertical bar from x = xl16 to xr16 (1/16 px) */
{
    int x;
    gfx_cov_begin();
    for (x = xl16 >> 4; x <= (xr16 - 1) >> 4; x++) {
        int32_t l = xl16 > x * 16 ? xl16 : x * 16, h = xr16 < (x + 1) * 16 ? xr16 : (x + 1) * 16;
        if (h > l)
            gfx_cov_add_span(x, y0, y1, (h - l) * 16);
    }
    gfx_cov_flush(col);
}
static void g_vline(int xc16, int y0, int y1, uint32_t col) { g_vlinex(xc16 - 5, xc16 + 6, y0, y1, col); }
