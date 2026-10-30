/* SPDX-License-Identifier: MPL-2.0
 * octavio-native: FM-1 firmware written from scratch after the "FM-1 Engine Test Bench" prototype.
 * Shared declarations. Everything here is freestanding C99, no float, no libc. */
#pragma once
#include <stdint.h>

#define SCR_W 240
#define SCR_H 240
#define FS 44100                    /* nominal sample rate of every engine's maths */

/* ---- colours: 0xRRGGBB in, RGB565 (host order) out; the band is byte-swapped just before it is sent ---- */
#define RGB565(c) ((uint16_t)((((c) >> 8) & 0xF800u) | (((c) >> 5) & 0x07E0u) | (((c) >> 3) & 0x001Fu)))
#define COL_BG      0x050505u
#define COL_TEXT    0xf2efe6u
#define COL_LABEL   0xa8a59du
#define COL_RED     0xff3b1fu
#define COL_DIMTICK 0x3a3a3au

/* ---- fonts (assets_gen.c: Chrome renders of the prototype's glyphs, 8-bit coverage) ---- */
typedef struct { uint16_t off; uint8_t w, h; int8_t dx, dy; } glyph_t;
typedef struct { const glyph_t *g; const uint8_t *d; const uint8_t *map; uint8_t n; uint16_t adv64, size64; } font_t;   /* g: [4 sub-pixel phases][n] */
enum { F_TITLE, F_LABEL, F_VALUE, F_GNUM, F_HEN, F_MBI, F_MBB, F_STT, F_TBIG, F_TSM, F_MCH, F_SQL, F_SQV, F_OV_PT, F_OV_ROW, F_OV_CH, F_OV_HINT, F_OV_PN, F_OV_TST, F_OV_SM, F_DQL, F_COUNT };     /* F_GNUM: the graphs' time label, proportional (envgraph.c) */
extern const font_t FONTS[F_COUNT];
extern const uint8_t KNOB_SPR0[], KNOB_SPR1[], KNOB_BIG[], KNOB_BIGM[], KNOB_BIGP[];

/* ---- gfx.c ---- */
void gfx_band(uint16_t *buf, int y0, int y1);          /* rows [y0,y1) of the screen are drawn into buf (stride SCR_W) */
void gfx_bg(void);
void gfx_text(int f, const char *s, int32_t x64, int base, uint32_t col, int32_t ls64, int clip_x0, int clip_x1);
int32_t gfx_text_w64(int f, const char *s, int32_t ls64);
void gfx_capsule(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t r, uint32_t col, uint32_t amul);  /* Q8 px */
void gfx_sprite(const uint8_t *spr, int x0, int y0, int n, uint32_t amul);
void gfx_frect16(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t col, uint32_t a);
void gfx_mask(const uint8_t *m, int x0, int y0, int w, int h, uint32_t col);
void gfx_hline(int x0, int x1, int y, uint32_t col, uint32_t a);
void gfx_vline(int x, int y0, int y1, uint32_t col, uint32_t a);
void gfx_rect(int x, int y, int w, int h, uint32_t col, uint32_t a);
void gfx_disc(int32_t cx, int32_t cy, int32_t rx, int32_t ry, uint32_t col);   /* Q8 px, anti-aliased ellipse */
void gfx_cov_begin(void);
void gfx_cov_add_span(int x, int32_t top16, int32_t bot16, int w);   /* coverage accumulator for the graph strokes */
void gfx_cov_flush(uint32_t col);
void gfx_cov_flush_a(uint32_t col, uint32_t alpha);
void gfx_band_rows(int *y0, int *y1);

/* ---- math (mathfx.c) ---- */
extern const int16_t SIN_TAB[1025];
int32_t sin_q15(uint32_t ph);                          /* ph: 32-bit phase, result Q15 */
uint32_t exp2_fx(int32_t x_q16, int outq);             /* 2^x * 2^outq, saturating */
uint32_t isqrt32(uint32_t v);
