/* SPDX-License-Identifier: MPL-2.0 */
/* The band renderer. A screen is drawn in bands of a few rows: the UI describes the whole scene, every primitive
 * here clips itself to the band that is being drawn, so no frame buffer (115 KB) is needed. Pixels are RGB565 in
 * host order inside the band; main.c swaps the bytes just before the SPI DMA. All text and knob art are Chrome
 * renders of the prototype (assets_gen.c); strokes and discs are anti-aliased by distance. */
#include "octavio.h"

static uint16_t *gb;                       /* band buffer, stride SCR_W */
static int gy0, gy1;                       /* the band: rows [gy0, gy1) */

void gfx_band(uint16_t *buf, int y0, int y1) { gb = buf; gy0 = y0; gy1 = y1; }

static inline uint32_t ex565(uint16_t p)   /* RGB565 -> 0xRRGGBB (bits replicated) */
{
    uint32_t r = (p >> 8) & 0xF8u, g = (p >> 3) & 0xFCu, b = (p << 3) & 0xF8u;
    return ((r | (r >> 5)) << 16) | ((g | (g >> 6)) << 8) | (b | (b >> 5));
}
static inline uint16_t mix(uint16_t bg, uint32_t col, uint32_t a)      /* a: 0..256 */
{
    uint32_t b = ex565(bg);
    int32_t r = (int32_t)(b >> 16), g = (int32_t)((b >> 8) & 255u), bl = (int32_t)(b & 255u);
    r += (((int32_t)(col >> 16) - r) * (int32_t)a) >> 8;
    g += (((int32_t)((col >> 8) & 255u) - g) * (int32_t)a) >> 8;
    bl += (((int32_t)(col & 255u) - bl) * (int32_t)a) >> 8;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (bl >> 3));
}

void gfx_bg(void)
{
    static const uint16_t G3 = RGB565(0x030303u), G4 = RGB565(0x040404u), G5 = RGB565(0x050505u);
    int y, x;
    for (y = gy0; y < gy1; y++) {
        uint16_t *row = gb + (y - gy0) * SCR_W;
        int dy = y < SCR_H - 1 - y ? y : SCR_H - 1 - y;
        for (x = 0; x < SCR_W; x++) {
            int dx = x < SCR_W - 1 - x ? x : SCR_W - 1 - x, d = dx < dy ? dx : dy;
            uint16_t c = d <= 2 ? G3 : d <= 15 ? G4 : G5;
            if (dx < 7 && dy < 7) {                          /* the screen's rounded corners (radius 3 cqw = 7.2 px) */
                int ex = 7 - dx, ey = 7 - dy;
                if (ex * ex + ey * ey > 49)
                    c = 0;
            }
            row[x] = c;
        }
    }
}

void gfx_rect(int x, int y, int w, int h, uint32_t col, uint32_t a)
{
    int yy, xx, y0 = y > gy0 ? y : gy0, y1 = y + h < gy1 ? y + h : gy1, x0 = x < 0 ? 0 : x, x1 = x + w > SCR_W ? SCR_W : x + w;
    for (yy = y0; yy < y1; yy++) {
        uint16_t *row = gb + (yy - gy0) * SCR_W;
        for (xx = x0; xx < x1; xx++)
            row[xx] = a >= 256u ? RGB565(col) : mix(row[xx], col, a);
    }
}
void gfx_hline(int x0, int x1, int y, uint32_t col, uint32_t a) { gfx_rect(x0, y, x1 - x0, 1, col, a); }
void gfx_vline(int x, int y0, int y1, uint32_t col, uint32_t a) { gfx_rect(x, y0, 1, y1 - y0, col, a); }

int32_t gfx_text_w64(int f, const char *s, int32_t ls64)
{
    int32_t w = 0;
    for (; *s; s++)
        w += FONTS[f].adv64 + ls64;
    return w;
}

#ifndef TXT_ROUND
#define TXT_ROUND 8
#endif
void gfx_text(int f, const char *s, int32_t x64, int base, uint32_t col, int32_t ls64, int cx0, int cx1)
{
    const font_t *F = &FONTS[f];
    for (; *s; s++) {
        unsigned c = (uint8_t)*s;
        int gi = c < 128u ? F->map[c] : 255;
        if (gi != 255) {
            int32_t q = (x64 + TXT_ROUND) >> 4;                  /* quarter pixels: the glyph is rendered at 4 sub-pixel offsets */
            const glyph_t *g = &F->g[(q & 3) * F->n + gi];
            int px = (q >> 2) + g->dx, py = base + g->dy, r, c2;
            const uint8_t *d = F->d + g->off;
            for (r = 0; r < g->h; r++) {
                int y = py + r;
                uint16_t *row;
                if (y < gy0 || y >= gy1)
                    continue;
                row = gb + (y - gy0) * SCR_W;
                for (c2 = 0; c2 < g->w; c2++) {
                    int x = px + c2;
                    uint32_t a = d[r * g->w + c2];
                    if (a && x >= cx0 && x < cx1)
                        row[x] = mix(row[x], col, a + (a >> 7));
                }
            }
        }
        x64 += F->adv64 + ls64;
    }
}

/* an 8-bit alpha mask (a pre-rendered piece of static chrome) in one colour */
void gfx_mask(const uint8_t *m, int x0, int y0, int w, int h, uint32_t col)
{
    int r, c;
    for (r = 0; r < h; r++) {
        int y = y0 + r;
        uint16_t *row;
        if (y < gy0 || y >= gy1)
            continue;
        row = gb + (y - gy0) * SCR_W;
        for (c = 0; c < w; c++) {
            uint32_t a = m[r * w + c];
            int x = x0 + c;
            if (a && x >= 0 && x < SCR_W)
                row[x] = mix(row[x], col, a + (a >> 7));
        }
    }
}

/* anti-aliased capsule (a line with round caps), coordinates in Q8 pixels, radius Q8 */
void gfx_capsule(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t r, uint32_t col, uint32_t amul)
{
    int32_t mnx = (x0 < x1 ? x0 : x1) - r - 256, mxx = (x0 > x1 ? x0 : x1) + r + 256;
    int32_t mny = (y0 < y1 ? y0 : y1) - r - 256, mxy = (y0 > y1 ? y0 : y1) + r + 256;
    int32_t dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
    int px, py, ya = mny >> 8, yb = mxy >> 8, xa = mnx >> 8, xb = mxx >> 8;
    if (ya < gy0) ya = gy0;
    if (yb >= gy1) yb = gy1 - 1;
    if (xa < 0) xa = 0;
    if (xb >= SCR_W) xb = SCR_W - 1;
    for (py = ya; py <= yb; py++) {
        uint16_t *row = gb + (py - gy0) * SCR_W;
        for (px = xa; px <= xb; px++) {
            int32_t qx = (px << 8) + 128 - x0, qy = (py << 8) + 128 - y0, t = 0, ex, ey, d, cov;
            if (len2) {
                t = (qx * dx + qy * dy) / (int32_t)((len2 >> 8) | 1);        /* dot * 256 / len2 */
                t = t < 0 ? 0 : t > 256 ? 256 : t;
            }
            ex = qx - ((t * dx) >> 8);
            ey = qy - ((t * dy) >> 8);
            d = (int32_t)isqrt32((uint32_t)(ex * ex + ey * ey));
            cov = r - d + 128;
            if (cov <= 0)
                continue;
            if (cov > 256)
                cov = 256;
            cov = (cov * (int32_t)amul) >> 8;
            row[px] = mix(row[px], col, (uint32_t)cov);
        }
    }
}

/* anti-aliased ellipse, centre and radii in Q8 pixels */
void gfx_disc(int32_t cx, int32_t cy, int32_t rx, int32_t ry, uint32_t col)
{
    int px, py, ya = ((cy - ry) >> 8) - 1, yb = ((cy + ry) >> 8) + 1, xa = ((cx - rx) >> 8) - 1, xb = ((cx + rx) >> 8) + 1;
    int32_t rm = (rx + ry) >> 1;
    if (ya < gy0) ya = gy0;
    if (yb >= gy1) yb = gy1 - 1;
    if (xa < 0) xa = 0;
    if (xb >= SCR_W) xb = SCR_W - 1;
    for (py = ya; py <= yb; py++) {
        uint16_t *row = gb + (py - gy0) * SCR_W;
        for (px = xa; px <= xb; px++) {
            int32_t ux = (((px << 8) + 128 - cx) << 8) / rx, uy = (((py << 8) + 128 - cy) << 8) / ry;   /* Q8, 1.0 = edge */
            int32_t d = (int32_t)isqrt32((uint32_t)(ux * ux + uy * uy)), cov = (((256 - d) * rm) >> 8) + 128;
            if (cov <= 0)
                continue;
            row[px] = mix(row[px], col, (uint32_t)(cov > 256 ? 256 : cov));
        }
    }
}

/* a sprite of premultiplied RGBA (4 bytes a pixel), n x n, over whatever is in the band */
void gfx_sprite(const uint8_t *spr, int x0, int y0, int n, uint32_t amul)
{
    int r, c;
    for (r = 0; r < n; r++) {
        int y = y0 + r;
        uint16_t *row;
        if (y < gy0 || y >= gy1)
            continue;
        row = gb + (y - gy0) * SCR_W;
        for (c = 0; c < n; c++) {
            const uint8_t *p = spr + (r * n + c) * 4;
            int x = x0 + c;
            uint32_t a = (p[3] * amul) >> 8, b, rr, gg, bb;
            if (!a || x < 0 || x >= SCR_W)
                continue;
            b = ex565(row[x]);
            rr = (((p[0] * amul) >> 8) + ((((b >> 16) & 255u) * (255u - a) * 257u) >> 16));
            gg = (((p[1] * amul) >> 8) + ((((b >> 8) & 255u) * (255u - a) * 257u) >> 16));
            bb = (((p[2] * amul) >> 8) + (((b & 255u) * (255u - a) * 257u) >> 16));
            if (rr > 255) rr = 255;
            if (gg > 255) gg = 255;
            if (bb > 255) bb = 255;
            row[x] = (uint16_t)(((rr & 0xF8u) << 8) | ((gg & 0xFCu) << 3) | (bb >> 3));
        }
    }
}

/* coverage accumulator for stroked curves: spans per column and sub-sample, flushed with one colour */
#define COV_ROWS 16
static uint16_t cov[COV_ROWS * SCR_W];
static uint8_t cov_used;
void gfx_cov_begin(void)
{
    if (cov_used) {
        int i;
        for (i = 0; i < COV_ROWS * SCR_W; i++)
            cov[i] = 0;
        cov_used = 0;
    }
}
void gfx_cov_add_span(int x, int32_t top16, int32_t bot16, int w)     /* y in 1/16 px, w: weight out of 256 */
{
    int r, ra = (top16 >> 4), rb = (bot16 >> 4);
    if (x < 0 || x >= SCR_W || bot16 <= top16)
        return;
    if (ra < gy0) ra = gy0;
    if (rb >= gy1) rb = gy1 - 1;
    for (r = ra; r <= rb; r++) {
        int32_t lo = top16 > r * 16 ? top16 : r * 16, hi = bot16 < (r + 1) * 16 ? bot16 : (r + 1) * 16;
        if (hi > lo) {
            cov[(r - gy0) * SCR_W + x] += (uint16_t)(((hi - lo) * w) >> 4);
            cov_used = 1;
        }
    }
}
void gfx_cov_flush(uint32_t col)
{
    int y, x;
    if (!cov_used)
        return;
    for (y = 0; y < gy1 - gy0; y++)
        for (x = 0; x < SCR_W; x++) {
            uint16_t a = cov[y * SCR_W + x];
            if (a) {
                gb[y * SCR_W + x] = mix(gb[y * SCR_W + x], col, a > 256 ? 256 : a);
                cov[y * SCR_W + x] = 0;
            }
        }
    cov_used = 0;
}
