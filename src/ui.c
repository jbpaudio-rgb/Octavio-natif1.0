/* SPDX-License-Identifier: MPL-2.0 */
/* The screen of the prototype: a title, four knob cells (icon, label, value) in a 2x2 grid and the scope graph
 * below. Layout numbers come from the prototype's own CSS (1 cqw = 2.4 px on the 240 px screen). */
#include "octavio.h"
#include "params.h"
#include "ui.h"
#include "ctrl.h"

typedef struct { const char *title; const char *names[4]; uint8_t p[4]; } page_t;
static const page_t PAGES[PG_COUNT] = {
    [PG_GLO]  = { "FM",     { "FM ENV", "MOD WAVE", "RATIO", "FM" },        { P_FENV, P_MWAVE, P_RATIO, P_FM } },
    [PG_EDIT] = { "GLOBAL", { "FOLD", "SYMM.", "FREQ", "WAVE" },            { P_FOLD, P_SYM, P_FREQ, P_WAVE } },
    [PG_ENV]  = { "AD ENV", { "ATTACK", "DECAY", "VACT", "MODE" },          { P_ATK, P_EDEC, P_DECAY, P_MODE } },
    [PG_ARP]  = { "ARPEGGIATOR", { "MODE", "OCTAVES", "RATE", "GATE" },     { P_ARPMODE, P_ARPOCT, P_ARPRATE, P_ARPGATE } },
    [PG_ARP2] = { "ARP 2", { "SWING", "LATCH", "ACCENT", "CHANCE" },        { P_ARPSW, P_ARPLATCH, P_ARPACC, P_ARPCHANCE } },
    [PG_ARP3] = { "ARP 3", { "TRANSP.", "REPEAT", "HUMANIZE", "PATTERN" },  { P_ARPTR, P_ARPREP, P_ARPHUM, P_ARPPAT } },
    [PG_TEMPO] = { "INTERNAL CLOCK", { "TEMPO", "\x03", "\x03", "\x03" },  { P_BPM, 255, 255, 255 } },
    [PG_MENU] = { "MIDI CHANNELS", { "T1", "T2", "T3", "T4" },              { P_CHS0, P_CHS1, P_CHS2, P_CHS3 } },
    [PG_MENU2] = { "MIDI ROUTING", { "NOTES", "CLOCK", "START", "SEND" },   { P_IONOTE, P_IOCLK, P_IOTRN, P_IOSND } },
    [PG_FXO] = { "FX ORDER", { "", "", "", "" }, { 255, 255, 255, 255 } },
    [PG_HOME] = { "", { "T1", "T2", "T3", "T4" },                           { P_VOL0, P_VOL1, P_VOL2, P_VOL3 } },
    [PG_DSEQ] = { "", { "", "", "", "" },                                   { 255, 255, 255, 255 } },
};
static void env_tick(uint32_t ms);
static void lfo_tick(uint32_t dt);
static uint32_t arp_now, fx_ms;
static void home_tick(uint32_t dt);
static int sq_line;
static const int8_t ARP_OFS[12] = { 0, 1, 0, 0, 0, 0, 0, 0, -12, 1, 0, 0 };      /* the arpeggiator's value = the page's value + this (octaves 1..4, transpose -12..12, repeat 1..4) */
static int lfo_pid(int off);                   /* lfograph.c */
static void sin_cos_deg(int32_t deg_q8, int32_t *s, int32_t *c);
static void seq_commit(void);
static int page = PG_GLO, oct;
static uint8_t dirty_top, dirty_all = 1;
static uint32_t ed_phase;                      /* the scope's scroll (edP), 32-bit turns */
static uint32_t last_ms;
static int32_t wheel[4], wheel_g[4];           /* the knobs' unrounded positions (the prototype's `ws`) and the rounded position the last step left (`wq`) */
static int wheel_p[4] = { -1, -1, -1, -1 };
#include "pages.c"                             /* dynamic pages (LFO, FX), page state */
#include "engine_pages.c"                      /* the Minimoog / Prophet / drum settings pages */

/* a destination knob follows the engine of the selected track (dst.c): its position is the place in that engine's list */
static int32_t pn_q16(int id) { return dst_kind_of(id) ? dst_n_q16(id, voices_engine(voices_sel())) : param_n_q16(id); }
static void pset_n(int id, int32_t n) { if (dst_kind_of(id)) dst_set_n(id, voices_engine(voices_sel()), n); else param_set_n(id, n); }
static int pid_of(int k)                       /* the parameter under KNOB k+1 on this page (ENV: cutoff in low-pass) */
{
    if (page == PG_ENV && k == 2 && PV[P_MODE] == 0)
        return P_OPEN;
    if (page == PG_LFO || page == PG_FX)
        return dyn_cell(k, 0, 0);
    if (page == PG_SEQ) {
        static const uint8_t o[4] = { 0, 1, 255, 2 };            /* destination, key, (step value), amount: P_M4T + 4 * line + o */
        return k == 2 ? P_STEPV : P_M4T + 4 * sq_line + o[k] ;
    }
    return PAGES[page].p[k] == 255 ? -1 : PAGES[page].p[k];
}

void ui_set_page(int p) { if (p >= 0 && p < PG_COUNT && p != page) { page = p; dirty_all = 1; } }
int ui_get_page(void) { return page; }
void ui_lfo_sync_toggle(void)                  /* LFO held + OP8 (toggleSync("LFO")): the selected source's tempo sync switch */
{
    int id;
    lfo_src = ctrl_src();
    id = lfo_pid(4);
    PV[id] = !PV[id];
    ui_params_changed();
    dirty_all = 1;
}

static void seq_sync(void);
void ui_init(void)
{
    param_reset(); seq_sync(); dirty_all = 1;
#ifdef BOOT_PAGE                                /* test builds: start on a page (XCFLAGS=-DBOOT_PAGE=PG_SEQ) */
    page = BOOT_PAGE;
#endif
}

void ui_octave(int d)
{
    int o = oct + d;
    oct = o < -2 ? -2 : o > 2 ? 2 : o;
}
int ui_note_of_key(int key) { return 53 + key + 12 * oct; }

static int enc_cnt(int id)                     /* number of values of a parameter that is a short list (0: a continuous one) */
{
    const pdesc_t *d = &PD[id];
    if (dst_kind_of(id)) { int c, sq = dst_kind_of(id) == 2; dst_ft = (int8_t)mod_view; c = dst_count(voices_engine(voices_sel()), sq); dst_ft = -1; return c; }
    if (d->kind == PK_SELECT) return d->nopt;
    if (d->kind == PK_CHECK) return 2;
    if (d->step > 0) { int c = (d->max - d->min) / d->step + 1; return c <= 32 ? c : 0; }
    return 0;
}
void ui_encoder(int k, int steps)              /* one detent = 1/50 of the range, like the prototype's wheel */
{
    int i, n = steps < 0 ? -steps : steps;
    if (page == PG_FXO) {                      /* FX ORDER: K1..K4 = the Mix of slots 1..4; SEL held + K1 moves the selected effect along the chain */
        if (ctrl_sel_held()) { if (k == 0) ctrl_fxo_knob(steps); }
        else if (k < FX_NSLOT) {
            int id = fx_mix_pid(k);
            for (i = 0; i < n; i++) param_set_n(id, param_n_q16(id) + (steps < 0 ? -1 : 1) * (65536 / 40));
            ui_params_changed();
        }
        dirty_all = 1; return;
    }     /* FX ORDER: K1 = the hidden "fxpos" slider (SEL held: move the effect) */
    if (ep_on()) { ep_encoder(k, steps); dirty_top = 1; return; }
    if (ep_strips()) { epm_encoder(k, steps); dirty_top = 1; return; }
    for (i = 0; i < n; i++) {
        int id = pid_of(k);
        int32_t g;
        if (id < 0)
            return;
        g = pn_q16(id);
        int32_t base = (wheel_p[k] == id && g == wheel_g[k]) ? wheel[k] : g;      /* Math.abs(get() - wq) < 1e-9 ? ws : get() */
        int32_t nv = base + (steps < 0 ? -1 : 1) * (65536 / 50);
        {   /* a parameter with few values (a wave, an octave, a mode, a destination list ...): one detent = one value.  The prototype's 1/50 of the range per detent needs 17 to 50 clicks to leave a 2..4 value choice (first test on the real FM-1: those knobs "did not change") */
            int cnt = enc_cnt(id);
            if (cnt >= 2) {
                int idx = (int)(((int64_t)g * (cnt - 1) + 32768) >> 16) + (steps < 0 ? -1 : 1);
                idx = idx < 0 ? 0 : idx > cnt - 1 ? cnt - 1 : idx;
                nv = (int32_t)(((int64_t)idx * 65536 + (cnt - 1) / 2) / (cnt - 1));
            }
        }
        if (nv < 0) nv = 0;
        if (nv > 65536) nv = 65536;
        pset_n(id, nv);
        wheel[k] = nv;
        wheel_g[k] = pn_q16(id);
        wheel_p[k] = id;
        if (id == P_STEPV) seq_commit();
        if (id >= P_ARPMODE && id <= P_ARPPAT) voices_arp(voices_sel(), id - P_ARPMODE, PV[id] + ARP_OFS[id - P_ARPMODE]);      /* the ARP pages edit the selected track's arpeggiator */
    }
    dirty_top = 1;
}

void ui_button(int b)
{
    int np = page;
    if (b == BTN_ENV) np = PG_ENV;
    else if (b == BTN_EDIT) np = PG_GLO;       /* on the Vactrol: EDIT shows the FM page, GLO the global one */
    else if (b == BTN_GLO) np = PG_EDIT;
    else if (b == BTN_LFO) { if (page == PG_LFO) lfo_src = (lfo_src + 1) % 6; np = PG_LFO; }
    else if (b == BTN_FX) { if (page == PG_FX) fxo = (fxo + 1) % FX_NSLOT; np = PG_FX; }
    else if (b == BTN_ARP) np = PG_ARP;
    else if (b == BTN_HOME) np = PG_HOME;
    else if (b == BTN_SEQ) np = PG_SEQ;
    else if (b == BTN_SEL && page == PG_FXO) fxo = (fxo + 1) % FX_NSLOT;
    if (np != page || b == BTN_LFO || b == BTN_FX || b == BTN_SEL) {
        page = np;
        dirty_all = 1;
    }
}

void ui_tick(uint32_t ms)                      /* the scope scrolls: .25 + 1.5 * freq cycles per second */
{
    uint32_t dt = ms - last_ms;
    last_ms = ms;
    if (dt > 50u) dt = 50u;
    env_tick(ms);
    arp_now = ms;
    fx_ms = ms % 1000000u;
    lfo_tick(dt);
    home_tick(dt);
    ui_seq_cur(voices_seq_cur());                  /* the step clock's playing step (PLAY): the SEQ / DSEQ cursor */
    ed_phase += dt * (uint32_t)(250 + (3 * PV[P_FREQ]) / 2) * 4295u;      /* turns/ms in 2^32 / 1e6 units */
}

static int page_of(int pgn)                    /* the controller's PGN_* -> this screen's PG_* */
{
    static const int8_t M[PGN_COUNT] = { PG_GLO, PG_EDIT, PG_ENV, PG_LFO, PG_FX, PG_FXO, PG_ARP, PG_SEQ, PG_HOME, PG_MENU, PG_MENU2, PG_TEMPO,
                                         PG_OSC1, PG_OSC2, PG_OSC3, PG_OSC4, PG_ENV2, PG_GLO2, PG_DSEQ };
    return pgn >= 0 && pgn < PGN_COUNT ? M[pgn] : page;
}
static void ctrl_sync(void)                    /* the controller's state -> the pages' (page, LFO source, FX slot, HOME's track) */
{
    int np = page_of(ctrl_page());
    if (np == PG_ARP) np = PG_ARP + ctrl_arp_sub();                   /* ARP held + OP1..OP3: ARP, ARP 2, ARP 3 */
    page = np;
    lfo_src = ctrl_src();
    fxo = ctrl_fx_slot();
    ui_set_track(ctrl_track());
    voices_select(ctrl_track());
    { int i; for (i = 0; i <= P_ARPPAT - P_ARPMODE; i++) PV[P_ARPMODE + i] = (int16_t)(voices_arp_get(ctrl_track(), i) - ARP_OFS[i]); }    /* the ARP pages show the selected track's arpeggiator */
    fx_select(ctrl_track());                   /* the FX knobs in PV[] are the selected track's */
    mod_select(ctrl_track());                  /* so are its modulation matrix and sequencer lines (dst.c) */
    seq_sync();                                /* ... and its step values: the STEP VALUE knob shows the selected step of this track */
}
void ui_goto(int p)                            /* the controller shows page PGN_p (its state follows through ctrl_sync) */
{
    (void)p;
    ctrl_sync();
    dirty_all = 1;
}

int ui_scene(int *y0, int *y1)
{
    static uint32_t seen;
    uint32_t v = ctrl_version();
    { static uint32_t hseen; static uint8_t hon; if (hud.on && (hud.ver != hseen || !hon)) dirty_all = 1; hseen = hud.ver; hon = hud.on; if (!hud.on && !hon) { } }
    if (v != seen || ctrl_overlay() != OV_NONE) { if (v != seen) ctrl_sync(); seen = v; dirty_all = 1; }
    if (dirty_all) { dirty_all = 0; dirty_top = 0; *y0 = 0; *y1 = SCR_H; return 1; }
    if (page == PG_DSEQ) { *y0 = 0; *y1 = 1; dirty_top = 0; return 1; }         /* nothing changed: one row (the cursor, the gates and the lane mark redraw everything through dirty_all) */
    *y0 = dirty_top || page == PG_HOME ? 0 : 124;
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

#include "graphs.c"
#include "envgraph.c"
#include "lfograph.c"
#include "arpgraph.c"
#include "fxgraph.c"
#include "screens.c"
#include "engine_graphs.c"

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
static uint32_t dimmix(uint32_t c);
extern const uint8_t KNOB_SPRM0[], KNOB_SPRM1[], KNOB_SPRP0[], KNOB_SPRP1[], KNOB_SPRD0[], KNOB_SPRD1[];
static void draw_knob(int cx, int cy, int32_t n_q16, uint32_t am)       /* am: opacity 0..256 (a dimmed cell); the skin follows the track's engine */
{
    int32_t ox = KX8[cx], oy = KY8[cy], cxq = ox + 5528, cyq = oy + 5528;       /* centre = origin + 30 * 0.71983 px */
    int i, skin = voices_engine(voices_sel());
    const uint8_t *spr = skin == ENG_MOOG ? (cx ? KNOB_SPRM1 : KNOB_SPRM0) : skin == ENG_PROPH ? (cx ? KNOB_SPRP1 : KNOB_SPRP0)
                         : skin == ENG_DRUM ? (cx ? KNOB_SPRD1 : KNOB_SPRD0) : (cx ? KNOB_SPR1 : KNOB_SPR0);
    for (i = 0; i < 11; i++) {                                                  /* the 11 ticks */
        int32_t s, c;
        int lit = (int32_t)i * 6554 <= n_q16 + 66;
        sin_cos_deg((-135 + 27 * i) * 256, &s, &c);
        gfx_capsule(cxq + ((s * 4237) >> 15), cyq - ((c * 4237) >> 15), cxq + ((s * 5159) >> 15), cyq - ((c * 5159) >> 15),
                    184, lit ? COL_TEXT : COL_DIMTICK, am);                    /* r 23 .. 28 svg units, stroke 2 */
    }
    gfx_sprite(spr, ox >> 8, oy >> 8, 44, am);
    {
        int32_t s, c, deg = -135 * 256 + (int32_t)(((int64_t)n_q16 * 270 * 256) >> 16);
        sin_cos_deg(deg, &s, &c);
        if (skin == ENG_MOOG) {                                                  /* a cream dot on the rim: circle (30, 11) r 3, stroke .5 */
            int32_t dx = cxq + ((s * 3502) >> 15), dy = cyq - ((c * 3502) >> 15);
            gfx_disc(dx, dy, 599, 599, am < 256 ? dimmix(0x000000u) : 0x000000u);
            gfx_disc(dx, dy, 507, 507, am < 256 ? dimmix(0xf4f1e6u) : 0xf4f1e6u);
        } else if (skin == ENG_PROPH)                                            /* a thin light line from 3.5 to 14.5 units out */
            gfx_capsule(cxq + ((s * 2672) >> 15), cyq - ((c * 2672) >> 15), cxq + ((s * 645) >> 15), cyq - ((c * 645) >> 15), 166, 0xf4f4f0u, am);
        else if (skin == ENG_DRUM)                                               /* a dark bar, 2.8 x 7 units, 7 .. 14 out */
            gfx_capsule(cxq + ((s * 2322) >> 15), cyq - ((c * 2322) >> 15), cxq + ((s * 1548) >> 15), cyq - ((c * 1548) >> 15), 258, 0x141414u, am);
        else
            gfx_capsule(cxq + ((s * PTR_A) >> 15), cyq - ((c * PTR_A) >> 15), cxq + ((s * PTR_B) >> 15), cyq - ((c * PTR_B) >> 15),
                        PTR_R, 0x15181bu, am);                                   /* the pointer, (30,17.5)-(30,28) */
    }
}

static const int32_t TXT_X64[2] = { 3531, 10981 };                              /* 55.17 / 171.58 px, Q6 */
static const int LAB_BASE[2] = { 47, 95 }, VAL_BASE[2] = { 68, 115 };
#define LS_CELL 42                                                              /* letter-spacing .66 px, Q6 */

#define DIM_A 97                                                              /* a dimmed cell: .38 (measured on the prototype) */
static uint32_t dimmix(uint32_t c)                                            /* a colour at .3 over the background */
{
    return (((c >> 16 & 255u) * 97u + 5u * 159u) >> 8 << 16) | (((c >> 8 & 255u) * 97u + 5u * 159u) >> 8 << 8) | (((c & 255u) * 97u + 5u * 159u) >> 8);
}
static void draw_cell(int k)
{
    int cx = k & 1, cy = k >> 1, id = pid_of(k), dim = id < 0, eon = ep_on(), clip = cx ? 233 : 116;      /* a cell clips its text at its own edge */
    const char *lab = PAGES[page].names[k];
    char v[16];
    if (page == PG_LFO || page == PG_FX)
        dyn_cell(k, &lab, &dim);
    if (eon) { id = EP.id[k]; dim = id < 0; lab = EP.nm[k]; }
    draw_knob(cx, cy, dim ? -1000 : eon ? ep_n_q16(id) : pn_q16(id), dim ? DIM_A : 256);
    gfx_text(F_LABEL, id == P_OPEN && !eon ? "CUTOFF" : lab, TXT_X64[cx], LAB_BASE[cy], COL_LABEL, LS_CELL, 0, clip);
    if (dim)
        return;
    if (eon) ep_text(id, v); else cell_text(id, v);
    gfx_text(F_VALUE, v, TXT_X64[cx], VAL_BASE[cy], COL_TEXT, LS_CELL, 0, clip);
}

void ui_prepare(void)
{
    if (ep_on()) epg_prepare();
    else if (ep_strips() || page == PG_DSEQ) { }
    else if (page == PG_ENV) env_prepare();
    else if (page == PG_LFO) lfo_prepare();
    else scope_prepare();
}

static void ui_draw_page(int y0, int y1);
void ui_draw(int y0, int y1)                    /* the current band is [y0, y1) */
{
    ui_draw_page(y0, y1);
    overlay_draw();                            /* the preset list, the name editor, the toast: over any page */
}
static void ui_draw_page(int y0, int y1)
{
    int k;
    gfx_bg();
    if (page == PG_HOME) { home_draw(); return; }
    if (ep_strips()) { epm_draw(); return; }
    if (page == PG_SEQ) { seq_draw(); return; }
    if (page == PG_DSEQ) { dseq_draw(); return; }
    {
        char tb[32];
        const char *t = PAGES[page].title;
        int32_t w;
        if (ep_on()) t = EP.title;
        if (page == PG_LFO || page == PG_FX) { dyn_title(tb); t = tb; }
        int ls = page == PG_FXO ? 0 : 65;                                  /* #fot has no letter-spacing */
        w = gfx_text_w64(F_TITLE, t, ls);
        gfx_text(F_TITLE, t, 460 + ((14440 - w) >> 1), 23, COL_TEXT, ls, 0, 233);   /* centred in 7.19 .. 232.82 */
    }
    if (page == PG_FXO) { fxo_draw(); return; }
    for (k = 0; k < 4; k++)
        draw_cell(k);
    if (page == PG_LFO)
        sync_box(PV[lfo_pid(4)]);
    else if (page == PG_FX && fxs[fxo] == 5)
        sync_box(PV[P_F5SYNC]);
    if (y1 > 124 && y0 < 206) {
        if (ep_on()) epg_draw();
        else if (page == PG_ENV) env_draw();
        else if (page == PG_LFO) lfo_draw();
        else if (page == PG_ARP || page == PG_ARP2 || page == PG_ARP3) arp_draw();
        else if (page == PG_FX) fx_draw();
        else if (page == PG_TEMPO) tempo_draw();
        else if (page == PG_MENU) menu_draw();
        else draw_scope();
    }
}
