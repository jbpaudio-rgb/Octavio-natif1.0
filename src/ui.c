/* SPDX-License-Identifier: MPL-2.0 */
/* The screen of the prototype: a title, four knob cells (icon, label, value) in a 2x2 grid and the scope graph
 * below. Layout numbers come from the prototype's own CSS (1 cqw = 2.4 px on the 240 px screen). */
#include "octavio.h"
#include "params.h"
#include "ui.h"
#include "ctrl.h"

enum { PG_GLO, PG_EDIT, PG_ENV, PG_COUNT };
typedef struct { const char *title; const char *names[4]; uint8_t p[4]; } page_t;
static const page_t PAGES[PG_COUNT] = {
    [PG_GLO]  = { "FM",     { "FM ENV", "MOD WAVE", "RATIO", "FM" },        { P_FENV, P_MWAVE, P_RATIO, P_FM } },
    [PG_EDIT] = { "GLOBAL", { "FOLD", "SYMM.", "FREQ", "WAVE" },            { P_FOLD, P_SYM, P_FREQ, P_WAVE } },
    [PG_ENV]  = { "AD ENV", { "ATTACK", "DECAY", "VACT", "MODE" },          { P_ATK, P_EDEC, P_DECAY, P_MODE } },
};
static int page = PG_GLO, oct;
static uint8_t dirty_top, dirty_all = 1;
static uint32_t ed_phase;                      /* the scope's scroll (edP), 32-bit turns */
static uint32_t last_ms;
static int32_t wheel[4];                       /* the knobs' unrounded positions (the prototype's `ws`) */
static int wheel_p[4] = { -1, -1, -1, -1 };

static int pid_of(int k)                       /* the parameter under KNOB k+1 on this page (ENV: cutoff in low-pass) */
{
    if (page == PG_ENV && k == 2 && PV[P_MODE] == 0)
        return P_OPEN;
    return PAGES[page].p[k];
}

void ui_init(void) { param_reset(); dirty_all = 1; }

void ui_octave(int d)
{
    int o = oct + d;
    oct = o < -2 ? -2 : o > 2 ? 2 : o;
}
int ui_note_of_key(int key) { return 53 + key + 12 * oct; }

void ui_encoder(int k, int steps)              /* one detent = 1/50 of the range, like the prototype's wheel */
{
    int i, n = steps < 0 ? -steps : steps;
    for (i = 0; i < n; i++) {
        int id = pid_of(k);
        int32_t g = param_n_q16(id);
        int32_t base = (wheel_p[k] == id && wheel[k] >= g - 1 && wheel[k] <= g + 1) ? wheel[k] : g;
        int32_t nv = base + (steps < 0 ? -1 : 1) * (65536 / 50);
        if (nv < 0) nv = 0;
        if (nv > 65536) nv = 65536;
        param_set_n(id, nv);
        wheel[k] = nv;
        wheel_p[k] = id;
    }
    dirty_top = 1;
}

void ui_button(int b)
{
    int np = page;
    if (b == BTN_ENV) np = PG_ENV;
    else if (b == BTN_EDIT) np = PG_GLO;       /* on the Vactrol: EDIT shows the FM page, GLO the global one */
    else if (b == BTN_GLO) np = PG_EDIT;
    if (np != page) {
        page = np;
        dirty_top = 1;
    }
}

void ui_tick(uint32_t ms)                      /* the scope scrolls: .25 + 1.5 * freq cycles per second */
{
    uint32_t dt = ms - last_ms;
    last_ms = ms;
    if (dt > 50u) dt = 50u;
    ed_phase += dt * (uint32_t)(250 + (3 * PV[P_FREQ]) / 2) * 4295u;      /* turns/ms in 2^32 / 1e6 units */
}

void ui_goto(int p)                            /* the controller's page -> this screen's (pages not drawn yet keep the current one) */
{
    int np = p == PGN_GLO ? PG_GLO : p == PGN_EDIT ? PG_EDIT : p == PGN_ENV ? PG_ENV : page;
    if (np != page) { page = np; dirty_top = 1; }
}

int ui_scene(int *y0, int *y1)
{
    static uint32_t seen;
    uint32_t v = ctrl_version();
    if (v != seen || ctrl_overlay() != OV_NONE) { seen = v; dirty_all = 1; }
    if (dirty_all) { dirty_all = 0; dirty_top = 0; *y0 = 0; *y1 = SCR_H; return 1; }
    *y0 = dirty_top ? 0 : 124;
    *y1 = 206;
    dirty_top = 0;
    return 1;
}

/* ---- the scope: a 330 x 120 canvas shown at 225.63 x 76.8 px at (7.19, 126.91) ---- */
#define NS 904                                 /* 226 columns x 4 sub-samples */
static int16_t gy_w[NS], gh_w[NS], gy_g[NS], gh_g[NS];        /* centre and half thickness, 1/16 px */
static int32_t dot_y8;                         /* the red dot, Q8 px */

static int16_t half_thick(const int16_t *y, int i, int r_q4)   /* r * sqrt(1 + slope^2), slope from the neighbours */
{
    int a = i > 0 ? i - 1 : 0, b = i < NS - 1 ? i + 1 : NS - 1;
    int32_t m = ((int32_t)y[b] - y[a]) * 64 / (b - a > 0 ? b - a : 1);      /* dy16 / (0.25 px * n) as Q8 */
    int32_t s;
    if (m > 20000) m = 20000;
    if (m < -20000) m = -20000;
    s = (int32_t)isqrt32(65536u + (uint32_t)(m * m));                       /* sqrt(1 + m^2), Q8 */
    return (int16_t)((r_q4 * s) >> 8);
}

static void scope_prepare(void)
{
    int i;
    for (i = 0; i < NS; i++) {
        int32_t xq8 = (7 << 8) + i * 64 + 32 - 13393;            /* px from the dot (52.316), Q8 */
        uint32_t u = ed_phase + (uint32_t)xq8 * 148714u;          /* 2 / 225.63 turns per px */
        int32_t car, w = vac_scope(u, page != PG_GLO, &car);
        gy_w[i] = (int16_t)(2647 - ((w * 1032) >> 16));           /* canvas y = 60 - f * 50.4 -> px, 1/16 */
        gy_g[i] = (int16_t)(2647 - ((car * 1032) >> 16));
        if (i == (13393 - ((7 << 8) + 32)) / 64)
            dot_y8 = 42319 - ((w * 8257) >> 15);
    }
    for (i = 0; i < NS; i++) {
        gh_w[i] = half_thick(gy_w, i, 19);                        /* 3.5 canvas px * .66 / 2 = 1.16 px = 18.5/16 */
        gh_g[i] = half_thick(gy_g, i, 11);                        /* 2 canvas px */
    }
    {
        int32_t c0, car;
        uint32_t u = ed_phase;
        c0 = vac_scope(u, page != PG_GLO, &car);
        dot_y8 = 42319 - ((c0 * 8257) >> 15);
    }
}

static void draw_scope(void)
{
    int i;
    /* the middle line (#333, 1 canvas px = .64 px) */
    gfx_cov_begin();
    for (i = 0; i < NS; i += 4) {
        int x = 7 + i / 4;
        gfx_cov_add_span(x, 2645 - 5, 2645 + 5, 256);
        (void)x;
    }
    gfx_cov_flush(0x333333u);
    for (i = 0; i < NS; i++)
        gfx_cov_add_span(7 + i / 4, gy_g[i] - gh_g[i], gy_g[i] + gh_g[i], 64);
    gfx_cov_flush(0x3a3a3au);
    for (i = 0; i < NS; i++)
        gfx_cov_add_span(7 + i / 4, gy_w[i] - gh_w[i], gy_w[i] + gh_w[i], 64);
    gfx_cov_flush(COL_TEXT);
    gfx_rect(51, 127, 1, 77, 0x555555u, 51);                     /* the dot's vertical line (.68 px wide, over two columns) */
    gfx_rect(52, 127, 1, 77, 0x555555u, 125);
    gfx_disc(13393, dot_y8, 875, 819, COL_RED);
}

/* ---- the knob cells ---- */
#define KS 0.71983                              /* knob px per svg unit: 43.19 / 60 */
static const int32_t KX8[2] = { 1841, 31639 }, KY8[2] = { 8228, 20513 };           /* svg origins, Q8 px */

static void sin_cos_deg(int32_t deg_q8, int32_t *s, int32_t *c)     /* degrees (Q8) -> sin, cos in Q15 */
{
    uint32_t ph = (uint32_t)(((int64_t)deg_q8 * 11930465) >> 8);    /* 2^32 / 360 per degree */
    *s = sin_q15(ph);
    *c = sin_q15(ph + 0x40000000u);
}

#ifndef PTR_A
#define PTR_A 2304
#define PTR_B 368
#define PTR_R 203
#endif
static void draw_knob(int cx, int cy, int32_t n_q16)
{
    int32_t ox = KX8[cx], oy = KY8[cy], cxq = ox + 5528, cyq = oy + 5528;       /* centre = origin + 30 * 0.71983 px */
    int i;
    for (i = 0; i < 11; i++) {                                                  /* the 11 ticks */
        int32_t s, c;
        int lit = (int32_t)i * 6554 <= n_q16 + 66;
        sin_cos_deg((-135 + 27 * i) * 256, &s, &c);
        gfx_capsule(cxq + ((s * 4237) >> 15), cyq - ((c * 4237) >> 15), cxq + ((s * 5159) >> 15), cyq - ((c * 5159) >> 15),
                    184, lit ? COL_TEXT : COL_DIMTICK, 256);                    /* r 23 .. 28 svg units, stroke 2 */
    }
    gfx_sprite(cx ? KNOB_SPR1 : KNOB_SPR0, ox >> 8, oy >> 8, 44, 256);
    {
        int32_t s, c, deg = -135 * 256 + (int32_t)(((int64_t)n_q16 * 270 * 256) >> 16);
        sin_cos_deg(deg, &s, &c);
        gfx_capsule(cxq + ((s * PTR_A) >> 15), cyq - ((c * PTR_A) >> 15), cxq + ((s * PTR_B) >> 15), cyq - ((c * PTR_B) >> 15),
                    PTR_R, 0x15181bu, 256);                                       /* the pointer, (30,17.5)-(30,28) */
    }
}

static const int32_t TXT_X64[2] = { 3531, 10981 };                              /* 55.17 / 171.58 px, Q6 */
static const int LAB_BASE[2] = { 47, 95 }, VAL_BASE[2] = { 68, 115 };
#define LS_CELL 42                                                              /* letter-spacing .66 px, Q6 */

static void draw_cell(int k)
{
    int cx = k & 1, cy = k >> 1, id = pid_of(k);
    char v[16];
    draw_knob(cx, cy, param_n_q16(id));
    gfx_text(F_LABEL, id == P_OPEN ? "CUTOFF" : PAGES[page].names[k], TXT_X64[cx], LAB_BASE[cy], COL_LABEL, LS_CELL, 0, 233);
    param_text(id, v);
    if (page == PG_ENV && id == P_MODE && v[0] == 'L')                         /* ENV: "Low pass" -> "LP" */
        v[1] = 'P', v[2] = 0;
    gfx_text(F_VALUE, v, TXT_X64[cx], VAL_BASE[cy], COL_TEXT, LS_CELL, 0, 233);
}

void ui_prepare(void) { scope_prepare(); }

void ui_draw(int y0, int y1)                    /* the current band is [y0, y1) */
{
    int k;
    gfx_bg();
    {
        const char *t = PAGES[page].title;
        int32_t w = gfx_text_w64(F_TITLE, t, 65);
        gfx_text(F_TITLE, t, 460 + ((14440 - w) >> 1), 23, COL_TEXT, 65, 0, 233);   /* centred in 7.19 .. 232.82 */
    }
    for (k = 0; k < 4; k++)
        draw_cell(k);
    if (y1 > 124 && y0 < 206)
        draw_scope();
    overlay_draw();
}
