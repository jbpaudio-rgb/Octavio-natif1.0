/* SPDX-License-Identifier: MPL-2.0 */
/* The prototype's non-graph screens: TEMPO, MENU (channel grid), HOME (4 strips), FXO (FX order), SEQ (pitch editor).
 * Included from ui.c after graphs.c.  Geometry comes from the prototype's layout (tools: ref/dumprect.js); boxes are pixel-snapped like Chrome's. */

/* ---- canvas pages: the 330 x 120 canvas at (7.19, 126.91), x scale .68373, y scale .64 ---- */
#define CXS_X16(xc) GX16(xc)                                   /* canvas x -> 1/16 px */
#define CYS_Y16(yc) GYC16((yc) * 256)                          /* canvas y -> 1/16 px */

static void cv_rect(int xa, int ya, int xb, int yb, uint32_t col, uint32_t a)      /* canvas px rectangle */
{
    gfx_frect16(CXS_X16(xa), CYS_Y16(ya), CXS_X16(xb), CYS_Y16(yb), col, a);
}
static void cv_box(int x, int y, int w, int h, uint32_t col)          /* strokeRect, lineWidth 2 */
{
    cv_rect(x - 1, y - 1, x + w + 1, y + 1, col, 256);
    cv_rect(x - 1, y + h - 1, x + w + 1, y + h + 1, col, 256);
    cv_rect(x - 1, y + 1, x + 1, y + h - 1, col, 256);
    cv_rect(x + w - 1, y + 1, x + w + 1, y + h - 1, col, 256);
}
/* centred text on the canvas: cx canvas x, base = pixel row of the baseline */
static void cv_text(int f, const char *s, int cx, int base, uint32_t col)
{
    int n = 0;
    int32_t adv = FONTS[f].adv64, w;
    const char *p;
    for (p = s; *p; p++) n++;
    w = n * adv;
    gfx_text(f, s, (CXS_X16(cx) * 4) - (w >> 1), base, col, 0, 0, 240);
}

static int tempo_beat(uint32_t ms, int32_t *frac)              /* beat 0..3 and the fraction 0..65535 of the current beat */
{
    uint32_t bpm = (uint32_t)PV[P_BPM];
    uint32_t ph = (uint32_t)(((uint64_t)ms * bpm * 1145325u) >> 20);     /* beats in Q16: ms * bpm / 60000 * 65536 */
    *frac = (int32_t)(ph & 0xffffu);
    return (int)((ph >> 16) & 3u);
}

static void tempo_draw(void)
{
    char t[8];
    int i, b;
    int32_t f;
    uint32_t bpm = (uint32_t)PV[P_BPM];
    t[0] = (char)('0' + bpm / 100u % 10u); t[1] = (char)('0' + bpm / 10u % 10u); t[2] = (char)('0' + bpm % 10u); t[3] = 0;
    cv_text(F_TBIG, bpm >= 100u ? t : t + 1, 165, 155, COL_TEXT);
    cv_text(F_TSM, "BPM", 165, 179, COL_LABEL);
    b = tempo_beat(arp_now, &f);
    for (i = 0; i < 4; i++) {
        int x = 99 + i * 44;
        if (i == b) {
            cv_box(x, 96, 24, 14, COL_RED);
            cv_rect(x + 1, 97, x + 23, 109, COL_RED, (uint32_t)(256 - ((f * 179) >> 16)));      /* 1 - .7 f */
        } else
            cv_box(x, 96, 24, 14, 0x3a3a3au);
    }
}

static void menu_draw(void)
{
    int ch, i;
    for (ch = 0; ch < 16; ch++) {
        int x = 6 + (ch % 8) * 40, y = 10 + (ch / 8) * 52, n = 0;
        char s[12], num[4];
        int k = 0;
        s[k++] = 'T';
        for (i = 0; i < 4; i++)
            if (PV[P_CHS0 + i] == ch) {
                if (n) s[k++] = ',';
                s[k++] = (char)('1' + i);
                n++;
            }
        s[k] = 0;
        cv_box(x, y, 36, 40, n ? COL_TEXT : 0x3a3a3au);
        k = 0;
        if (ch + 1 >= 10) num[k++] = '1';
        num[k++] = (char)('0' + (ch + 1) % 10);
        num[k] = 0;
        cv_text(F_MCH, num, x + 18, ch < 8 ? 140 : 174, n ? COL_RED : 0x555555u);
        if (n)
            cv_text(F_MCH, s, x + 18, ch < 8 ? 151 : 185, COL_TEXT);
    }
}

static int ui_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* ---- HOME: the four track strips ---- */
static int ui_track;                                           /* the selected track 0..3 (red frame) */
static int32_t home_pk[4];                                     /* output peaks 0..32767, decayed like the prototype's (.9 per 512 samples) */
void ui_set_track(int t) { if (t >= 0 && t < 4 && t != ui_track) { ui_track = t; dirty_all = 1; } }
int  ui_get_track(void) { return ui_track; }
void ui_peak(int t, int32_t pk_q15) { if (t >= 0 && t < 4 && pk_q15 > home_pk[t]) home_pk[t] = pk_q15; }
static void home_tick(uint32_t dt)
{
    int t;
    uint32_t i;
    for (t = 0; t < 4; t++)
        for (i = 0; i < dt && home_pk[t]; i++)
            home_pk[t] = (home_pk[t] * 64943) >> 16;           /* .9 per 11.6 ms */
}

static const uint8_t HS_X[4] = { 7, 64, 122, 179 }, HS_R[4] = { 61, 118, 176, 233 };            /* strip, pixel-snapped */
static const uint8_t HW_X[4] = { 24, 81, 138, 196 }, HW_R[4] = { 44, 102, 159, 216 };           /* well */
static const uint8_t HC_X[4] = { 26, 84, 141, 198 }, HC_R[4] = { 42, 99, 156, 214 };            /* its 14 cells */
static const uint8_t HK_X[4] = { 16, 73, 131, 188 };                                              /* fader cap (36 wide) */
static const uint8_t HC_Y0[14] = { 180, 169, 158, 148, 137, 127, 116, 105, 95, 84, 73, 63, 52, 41 };    /* cells from the bottom */
static const uint8_t HC_Y1[14] = { 189, 179, 168, 157, 147, 136, 125, 115, 104, 93, 83, 72, 62, 51 };
static const uint16_t HEN_CX16[4] = { 545, 1461, 2378, 3295 };                                    /* centre of the engine name, 1/16 px */

static void home_draw(void)
{
    int i, j;
    for (i = 0; i < 4; i++) {
        int sel = i == ui_track, x0 = HS_X[i], x1 = HS_R[i], vol = PV[P_VOL0 + i], n, cy;
        const char *nm = OPT_ENGINE[PV[P_TYP0 + i] & 3];
        int32_t w;
        int pk = (int)((home_pk[i] * (vol > 0 ? vol : 0)) / 1000);          /* 0..32767 */
        uint32_t bc = sel ? COL_RED : 0x2a2a2au;
        gfx_rect(x0, 7, x1 - x0, 192, bc, 256);                              /* the strip: 1 px border, #0d0d0d */
        gfx_rect(x0 + 1, 8, x1 - x0 - 2, 190, 0x0d0d0du, 256);
        gfx_rect(x0 + 1, 8, x1 - x0 - 2, 24, COL_TEXT, 256);                 /* the name plate */
        gfx_rect(x0 + 1, 8, x1 - x0 - 2, 1, 0x333333u, 256);
        w = (int32_t)ui_strlen(nm) * FONTS[F_HEN].adv64;
        gfx_text(F_HEN, nm, (int32_t)HEN_CX16[i] * 4 - (w >> 1), 24, 0x000000u, 0, 0, 240);
        gfx_rect(x0 + 1, 38, x1 - x0 - 2, 1, 0x333333u, 256);                /* the fader's top rule */
        gfx_rect(HW_X[i], 39, HW_R[i] - HW_X[i], 153, 0x333333u, 256);       /* the well */
        gfx_rect(HW_X[i] + 1, 40, HW_R[i] - HW_X[i] - 2, 151, 0x050505u, 256);
        n = (int)(((pk * 22 / 10 > 32767 ? 32767 : pk * 22 / 10) * 14 + 16384) >> 15);
        for (j = 0; j < 14; j++)
            gfx_rect(HC_X[i], HC_Y0[j], HC_R[i] - HC_X[i], HC_Y1[j] - HC_Y0[j], j < n ? (j >= 11 ? COL_RED : COL_TEXT) : 0x1c1c1cu, 256);
        cy = 67 + ((800 - vol) * 141 + (vol > 800 ? -500 : 500)) / 1000;     /* the cap: top row */
        gfx_rect(HK_X[i], cy, 36, 11, 0x000000u, 256);
        gfx_frect16(HK_X[i] * 16, (cy + 11) * 16, (HK_X[i] + 36) * 16, (cy + 11) * 16 + 24, 0x000000u, 256);     /* its shadow, 1.44 px */
        gfx_frect16(HK_X[i] * 16, (cy + 11) * 16 + 24, (HK_X[i] + 36) * 16, (cy + 11) * 16 + 32, 0x000000u, 118);
        for (j = 0; j < 9; j++) {                                                                                  /* #f2efe6 -> #bdb9ac */
            uint32_t t = (uint32_t)(j * 2 + 1) * 128u / 9u;                                                       /* (j + .5) / 9, 0..256 */
            uint32_t r = 242u - (53u * t >> 8), g = 239u - (54u * t >> 8), b = 230u - (58u * t >> 8);
            gfx_rect(HK_X[i] + 1, cy + 1 + j, 34, 1, (r << 16) | (g << 8) | b, 256);
        }
        gfx_rect(HK_X[i] + 1, cy + 5, 34, 2, COL_RED, 256);
    }
}

/* ---- FXO: "FX ORDER", six tiles (FX1..FX6) in two columns ---- */
#define MB_BG 0x241f1cu
#define MB_LINE 0x5a554fu
static int rnd16(int32_t v16) { return (v16 + 8) >> 4; }              /* 1/16 px -> pixel (half up) */

static void fxo_draw(void)
{
    int i;
    for (i = 0; i < FX_NSLOT; i++) {
        int col = i & 1, row = i >> 1, sel = i == fxo;
        int32_t L16 = col ? 1997 : 153;                              /* the tile's layout x (9.58 / 124.8 px), 1/16 */
        int x0 = col ? 125 : 10, x1 = x0 + 105, T = 32 + 60 * row;
        const char *fx = FX_NAME[fxs[i]];
        char lab[4] = { 'F', 'X', (char)('1' + i), 0 };
        int nb = ui_strlen(fx);
        int32_t wb = nb * (FONTS[F_MBB].adv64 + 31) + 307;           /* b: text + 4.8 px padding, 1/64 px */
        int32_t wi = 3 * (FONTS[F_MBI].adv64 + 31) + 307;
        int32_t cx64 = L16 * 4 + 3380;                               /* tile centre (52.815 px) in 1/64 px */
        int ib0, ib1, bb0, bb1, dy = 0, y;
        uint32_t tcol = fx_slot_off(fx_view, i) ? 0x77736cu : sel ? 0xffffffu : 0xbebbb4u;      /* a slot the CPU budget bypasses is dimmed */
        if (!sel) {
            gfx_rect(x0 + 3, T + 3, 105, 53, 0x000000u, 256);
            gfx_rect(x0, T, 105, 53, COL_TEXT, 256);
            gfx_rect(x0 + 2, T + 2, 101, 49, MB_BG, 256);
            gfx_rect(x0 + 2, T + 12, 101, 2, MB_LINE, 256);
            gfx_rect(x0 + 2, T + 25, 101, 1, MB_LINE, 256);
            gfx_rect(x0 + 2, T + 37, 101, 2, MB_LINE, 256);
            gfx_rect(x0 + 2, T + 50, 101, 1, MB_LINE, 256);
            gfx_rect(x0 + 25, T + 2, 1, 49, MB_LINE, 256);
            gfx_rect(x0 + 49, T + 2, 1, 49, MB_LINE, 61);
            gfx_rect(x0 + 73, T + 2, 1, 49, MB_LINE, 256);
            gfx_rect(x0 + 97, T + 2, 1, 49, MB_LINE, 256);
        } else {
            int32_t ya = T * 16 - 46, yb = ya + 53 * 16;
            gfx_frect16((x0 + 3) * 16, ya + 92, (x1 + 3) * 16, yb + 92, 0x000000u, 256);
            gfx_frect16(x0 * 16, ya, x1 * 16, yb, COL_RED, 256);
            gfx_frect16(x0 * 16, ya, x1 * 16, ya + 32, COL_TEXT, 256);
            gfx_frect16(x0 * 16, yb - 32, x1 * 16, yb, COL_TEXT, 256);
            gfx_frect16(x0 * 16, ya + 32, (x0 + 2) * 16, yb - 32, COL_TEXT, 256);
            gfx_frect16((x1 - 2) * 16, ya + 32, x1 * 16, yb - 32, COL_TEXT, 256);
            {
                int32_t pl = (x0 + 2) * 256 + 1229, pr = (x1 - 2) * 256 - 1229, pt = ya * 16 + 512 + 1229, pb = yb * 16 - 512 - 1229;
                gfx_disc(pl, pt, 461, 461, COL_TEXT);
                gfx_disc(pr, pt, 461, 461, COL_TEXT);
                gfx_disc(pl, pb, 461, 461, COL_TEXT);
                gfx_disc(pr, pb, 461, 461, COL_TEXT);
            }
            dy = -3;
        }
        ib0 = rnd16((cx64 - (wi >> 1)) >> 2); ib1 = rnd16((cx64 + (wi >> 1)) >> 2);
        bb0 = rnd16((cx64 - (wb >> 1)) >> 2); bb1 = rnd16((cx64 + (wb >> 1)) >> 2);
        if (bb0 < x0 + 2) bb0 = x0 + 2;
        if (bb1 > x1 - 2) bb1 = x1 - 2;
        y = T + dy;
        if (!sel) {
            gfx_rect(ib0, y + 6, ib1 - ib0, 17, MB_BG, 256);
            gfx_rect(bb0, y + 24, bb1 - bb0, 23, MB_BG, 256);
        }
        gfx_text(F_MBI, lab, cx64 - (wi >> 1) + 141, y + 17, tcol, 31, x0 + 2, x1 - 2);
        {   /* the slot's Mix: a percentage at the right of the label and a bar along the tile's foot (K1..K4 edit it) */
            int mv = PV[fx_mix_pid(i)], pc = (mv + 5) / 10, bw = 101 * (mv < 0 ? 0 : mv > 1000 ? 1000 : mv) / 1000;
            char pt[5]; int q = 0; int32_t w64;
            if (pc >= 100) pt[q++] = '1', pt[q++] = '0', pt[q++] = '0'; else { if (pc >= 10) pt[q++] = (char)('0' + pc / 10); pt[q++] = (char)('0' + pc % 10); }
            pt[q] = 0;
            w64 = ui_strlen(pt) * (FONTS[F_MBI].adv64 + 31);
            gfx_text(F_MBI, pt, (x1 - 9) * 64 - w64, y + 17, tcol, 31, x0 + 2, x1 - 2);
            if (bw > 0) gfx_rect(x0 + 2, y + 45, bw, 3, fx_slot_off(fx_view, i) ? 0x77736cu : sel ? COL_TEXT : COL_RED, 256);
        }
        gfx_text(F_MBB, fx, cx64 - (wb >> 1) + 141, y + 39, fx_slot_off(fx_view, i) ? 0x77736cu : sel ? 0xffffffu : COL_TEXT, 31, x0 + 2, x1 - 2);
    }
}

/* ---- SEQ: the sequencer's pitch editor (line sl, 16 steps) ---- */
static int16_t sq_vt[4][6][16] = { { { 0, 260, 600, 850, 1000, 850, 600, 260, 0, 260, 600, 850, 1000, 850, 600, 260 } } };    /* thousandths, per track (TRX[t].SQ: track 1 starts with the triangle, the others with zeros) */
#define sq_v sq_vt[mod_view]                     /* the selected track's (dst.c: the track whose matrix is in PV[]) */
static int sq_step, sq_cur = -1;                       /* selected line / step, the playing step (-1: stopped) */
static int sq_snap(int v) { return v >= 0 ? (v + 25) / 50 * 50 : -((-v + 25) / 50 * 50); }       /* an <input type=range step=.05> */
static void seq_sync(void) { PV[P_STEPV] = (int16_t)sq_snap(sq_v[sq_line][sq_step]); }
static void seq_commit(void)                   /* the STEP VALUE knob moved: REC while playing writes the playing step ("Turn KNOB3 while playing"), else the selected one */
{
    int i = (ctrl_rec() && ctrl_playing() && sq_cur >= 0) ? sq_cur : sq_step;
    sq_v[sq_line][i] = PV[P_STEPV];
}
void ui_seq_select(int line, int step)
{
    if (line >= 0 && line < 6) sq_line = line;
    if (step >= 0 && step < 16) sq_step = step;
    seq_sync(); dirty_all = 1;
}
int  ui_seq_line(void) { return sq_line; }
int  ui_seq_step(void) { return sq_step; }
void ui_seq_set(int line, int step, int v) { if (line >= 0 && line < 6 && step >= 0 && step < 16) { sq_v[line][step] = (int16_t)v; seq_sync(); dirty_all = 1; } }
int  ui_seq_get(int line, int step) { return sq_v[line][step]; }
int  ui_seq_get_t(int t, int line, int step) { return sq_vt[t & 3][line][step]; }
void ui_seq_set_t(int t, int line, int step, int v) { if (line >= 0 && line < 6 && step >= 0 && step < 16) { sq_vt[t & 3][line][step] = (int16_t)v; seq_sync(); dirty_all = 1; } }
void ui_seq_gate_toggle(int step) { if (step >= 0 && step < 16) { SQG[mod_view] ^= (uint16_t)(1u << step); dirty_all = 1; } }
int  ui_seq_gate(int step) { return (SQG[mod_view] >> step) & 1; }
void ui_seq_cur(int step) { if (step != sq_cur) { sq_cur = step; dirty_all = 1; } }

/* one knob: svg origin (ox8, oy8) in Q8 px, u = px per svg unit (Q16); the body is a sprite */
static void knob_g(int32_t ox8, int32_t oy8, const uint8_t *spr, int n, int32_t u16, int32_t n_q16, uint32_t am, int skin)
{
    int32_t cxq = ox8 + ((30 * u16) >> 8), cyq = oy8 + ((30 * u16) >> 8);
    int i;
    for (i = 0; i < 11; i++) {
        int32_t s, c;
        int lit = (int32_t)i * 6554 <= n_q16 + 66;
        sin_cos_deg((-135 + 27 * i) * 256, &s, &c);
        gfx_capsule(cxq + (int32_t)(((int64_t)s * ((23 * u16) >> 8)) >> 15), cyq - (int32_t)(((int64_t)c * ((23 * u16) >> 8)) >> 15),
                    cxq + (int32_t)(((int64_t)s * ((28 * u16) >> 8)) >> 15), cyq - (int32_t)(((int64_t)c * ((28 * u16) >> 8)) >> 15),
                    u16 >> 8, lit ? COL_TEXT : COL_DIMTICK, am);
    }
    gfx_sprite(spr, ox8 >> 8, oy8 >> 8, n, am);
    {
        int32_t s, c, deg = -135 * 256 + (int32_t)(((int64_t)n_q16 * 270 * 256) >> 16);
        int32_t a = (25 * u16) >> 9, b = (2 * u16) >> 8;                       /* (30,17.5)-(30,28): 12.5 and 2 units from the centre */
        sin_cos_deg(deg, &s, &c);
#define UNITS(u100) ((int32_t)(((int64_t)(u100) * u16) >> 8) / 100)             /* svg units (x100) -> Q8 px */
        if (skin == ENG_MOOG) {                                                 /* a cream dot on the rim: circle (30, 11) r 3, stroke .5 */
            int32_t dx = cxq + (int32_t)(((int64_t)s * UNITS(1900)) >> 15), dy = cyq - (int32_t)(((int64_t)c * UNITS(1900)) >> 15);
            gfx_disc(dx, dy, UNITS(325), UNITS(325), 0x000000u);
            gfx_disc(dx, dy, UNITS(275), UNITS(275), 0xf4f1e6u);
            return;
        }
        if (skin == ENG_PROPH) {                                                /* a thin light line from 3.5 to 14.5 units out */
            gfx_capsule(cxq + (int32_t)(((int64_t)s * UNITS(1450)) >> 15), cyq - (int32_t)(((int64_t)c * UNITS(1450)) >> 15),
                        cxq + (int32_t)(((int64_t)s * UNITS(350)) >> 15), cyq - (int32_t)(((int64_t)c * UNITS(350)) >> 15), UNITS(90), 0xf4f4f0u, am);
            return;
        }
        gfx_capsule(cxq + (int32_t)(((int64_t)s * a) >> 15), cyq - (int32_t)(((int64_t)c * a) >> 15),
                    cxq + (int32_t)(((int64_t)s * b) >> 15), cyq - (int32_t)(((int64_t)c * b) >> 15), (11 * u16 / 10) >> 8, 0x15181bu, am);
    }
}

static void upcase(char *d, const char *s) { for (; *s; s++) *d++ = (*s >= 'a' && *s <= 'z') ? (char)(*s - 32) : *s; *d = 0; }

/* a rectangle with 2 px rounded corners (coverage of the corner pixels .12 / .85) */
static void rrect2(int x, int y, int w, int h, uint32_t col)
{
    if (w < 4 || h < 4) { gfx_rect(x, y, w, h, col, 256); return; }
    gfx_rect(x + 2, y, w - 4, h, col, 256);
    gfx_rect(x, y + 2, w, h - 4, col, 256);
    gfx_rect(x + 1, y + 1, w - 2, h - 2, col, 256);
    gfx_rect(x + 1, y, 1, 1, col, 218); gfx_rect(x + w - 2, y, 1, 1, col, 218);
    gfx_rect(x, y + 1, 1, 1, col, 218); gfx_rect(x + w - 1, y + 1, 1, 1, col, 218);
    gfx_rect(x + 1, y + h - 1, 1, 1, col, 218); gfx_rect(x + w - 2, y + h - 1, 1, 1, col, 218);
    gfx_rect(x, y + h - 2, 1, 1, col, 218); gfx_rect(x + w - 1, y + h - 2, 1, 1, col, 218);
    gfx_rect(x, y, 1, 1, col, 31); gfx_rect(x + w - 1, y, 1, 1, col, 31);
    gfx_rect(x, y + h - 1, 1, 1, col, 31); gfx_rect(x + w - 1, y + h - 1, 1, 1, col, 31);
}

static void seq_draw(void)
{
    static const char *const SQLAB[3] = { "KEY", "STEP VALUE", "AMOUNT" };
    char t[40], v[16];
    int k, line = sq_line, ids[3];
    int32_t w;
    upcase(t, PD[P_M4T + 4 * line].opts[PV[P_M4T + 4 * line]]);
    w = gfx_text_w64(F_STT, t, 0);
    gfx_text(F_STT, t, 460 + ((14440 - w) >> 1), 27, COL_TEXT, 0, 0, 233);
    for (k = 0; k < 16; k++) {
        int32_t x8 = 1841 + k * 3634;
        int xa = (x8 + 128) >> 8, xb = (x8 + 3265 + 128) >> 8, val = sq_v[line][k];
        int bx0 = (x8 + 256 + 128) >> 8, bx1 = (x8 + 3009 + 128) >> 8, mid = 69725 /* 69.725 */, top, bot;
        (void)mid;
        gfx_rect(xa, 69, xb - xa, 1, 0x444444u, 256);                            /* the step's centre line */
        if (val >= 0) { top = (69725 - ((val * 33595) / 1000) + 500) / 1000; bot = 70; }
        else { top = 70; bot = (69725 + ((-val * 33595) / 1000) + 500) / 1000; }
        if (bot > top)
            rrect2(bx0, top, bx1 - bx0, bot - top, val >= 0 ? COL_TEXT : COL_RED);
        gfx_rect((x8 + 817 + 128) >> 8, 109, ((x8 + 2448 + 128) >> 8) - ((x8 + 817 + 128) >> 8), 2, (SQG[mod_view] >> k) & 1 ? COL_TEXT : 0x333333u, 256);
        if (k == sq_step) {                                                        /* inset 1 px frame */
            gfx_rect(xa, 36, xb - xa, 1, COL_TEXT, 256); gfx_rect(xa, 102, xb - xa, 1, COL_TEXT, 256);
            gfx_rect(xa, 37, 1, 65, COL_TEXT, 256); gfx_rect(xb - 1, 37, 1, 65, COL_TEXT, 256);
        }
        if (k == sq_cur) {                                                         /* outline 1 px outside */
            gfx_rect(xa - 1, 35, xb - xa + 2, 1, COL_TEXT, 256); gfx_rect(xa - 1, 103, xb - xa + 2, 1, COL_TEXT, 256);
            gfx_rect(xa - 1, 36, 1, 67, COL_TEXT, 256); gfx_rect(xb, 36, 1, 67, COL_TEXT, 256);
        }
    }
    ids[0] = P_M4K + 4 * line; ids[1] = P_STEPV; ids[2] = P_M4A + 4 * line;
    for (k = 0; k < 3; k++) {
        int32_t kl8 = 1841 + k * 19482;                                            /* kk left: 7.19 + k * (73.61 + 2.4) */
        int32_t cx64 = (kl8 + 9421) >> 2;                                          /* kk centre (36.805 px) in 1/64 px */
        int kl = (kl8 + 128) >> 8, kr = ((kl8 + 18844) + 128) >> 8, nl;
        int id = ids[k];
        char lab[16];
        {   /* the knob follows the engine of the track: Minimoog / Prophet sprites (the drums' SEQ is the lane grid: its pitch sequencer keeps the Vactrol one) */
            int skin = voices_engine(voices_sel());
            knob_g(kl8 + 2662, 29212, skin == ENG_MOOG ? KNOB_BIGM : skin == ENG_PROPH ? KNOB_BIGP : KNOB_BIG, 54, 57672, param_n_q16(id), 256, skin);
        }
        upcase(lab, SQLAB[k]);
        nl = ui_strlen(lab);
        {
            int32_t x64 = cx64 - ((nl * (FONTS[F_SQL].adv64 + 50)) >> 1);
            if (x64 < kl8 * 4 / 16 * 1) x64 = kl8 >> 2;                           /* wider than the cell: starts at its left edge */
            gfx_text(F_SQL, lab, x64, 180, COL_LABEL, 50, kl, kr);
        }
        if (id == P_M4K + 4 * line) upcase(v, PD[id].opts[PV[id]]);
        else param_text(id, v);
        nl = ui_strlen(v);
        {
            int32_t x64 = cx64 - ((nl * (FONTS[F_SQV].adv64 + 57)) >> 1);
            if (x64 < (kl8 >> 2)) x64 = kl8 >> 2;
            gfx_text(F_SQV, v, x64, 198, COL_TEXT, 57, kl, kr);
        }
    }
}

/* ---- DSEQ: the drum sequencer (ddseq()): six lanes (the six drum voices) x 16 steps, on the 330 x 280 canvas the prototype shows at
 * 225.625 x 187.19 px at (7.19, 35.73) (x scale .68371, y scale .66852); canvas lengths in 1/16 canvas px -> screen 1/16 px ---- */
#define DQ_X16(c16) (115 + (int32_t)(((int64_t)(c16) * 44808) >> 16))
#define DQ_Y16(c16) (572 + (int32_t)(((int64_t)(c16) * 43811) >> 16))
static void dq_rect(int32_t xa, int32_t ya, int32_t xb, int32_t yb, uint32_t col)        /* canvas 1/16 px rectangle */
{
    gfx_frect16(DQ_X16(xa), DQ_Y16(ya), DQ_X16(xb), DQ_Y16(yb), col, 256);
}
static void dq_stroke(int32_t x, int32_t y, int32_t w, int32_t h, int32_t lw, uint32_t col)     /* strokeRect, line width lw, centred on the path */
{
    int32_t a = lw / 2, b = lw - lw / 2;
    dq_rect(x - a, y - a, x + w + b, y + b, col);
    dq_rect(x - a, y + h - a, x + w + b, y + h + b, col);
    dq_rect(x - a, y + b, x + b, y + h - a, col);
    dq_rect(x + w - a, y + b, x + w + b, y + h - a, col);
}
static void dseq_draw(void)
{
    static const char *const KITL[3] = { "808", "909", "ACOUSTIC" };
    int t = voices_sel(), sv = ctrl_drum_voice(), v, x, k, held = ctrl_dseq_held(), cur = sq_cur;
    char tt[16];
    int32_t w;
    k = voices_param_get(t, ENG_DRUM, DR_ID(sv, DR_KV)); k = k < 0 ? 0 : k > 2 ? 2 : k;
    { int j = 0; while ((tt[j] = KITL[k][j]) != 0) j++; }
    if (held >= 0) { int n = ui_strlen(tt); tt[n++] = ' '; tt[n++] = ' '; tt[n++] = ' '; tt[n] = 0; }       /* "  " and the room of the held-lane dot */
    w = gfx_text_w64(F_TITLE, tt, 65);
    gfx_text(F_TITLE, tt, 460 + ((14440 - w) >> 1), 23, COL_TEXT, 65, 0, 233);
    if (held >= 0) {                                                           /* "  ●" after the kit name */
        int32_t xr = 460 + ((14440 - w) >> 1) + w - ((FONTS[F_TITLE].adv64 + 65) >> 1);      /* the dot's centre: half an advance before the right end, 1/64 px */
        gfx_disc(xr * 4, 17 * 256, 1100, 1100, COL_TEXT);
    }
    for (v = 0; v < 6; v++) {
        int32_t y = 48 + (v * 2192) / 3, rh = 731;
        int on = v == sv, kv = voices_param_get(t, ENG_DRUM, DR_ID(v, DR_KV)), ty = voices_param_get(t, ENG_DRUM, DR_ID(v, DR_TYPE));
        uint16_t g = drum_gates(t, v);
        int base = (DQ_Y16(y + 365) + 8) >> 4;
        kv = kv < 0 ? 0 : kv > 2 ? 2 : kv; ty = ty < 0 ? 0 : ty > 7 ? 7 : ty;
        gfx_text(F_DQL, DVN[kv][ty], 635, base, on ? 0xf2efe6u : 0x7d7a73u, 0, 0, 240);
        if (on) dq_rect(0, y + 32, 40, y + rh - 32, COL_RED);
        for (x = 0; x < 16; x++) {
            int hit = (g >> x) & 1;
            uint32_t col = hit ? (on ? 0xf2efe6u : 0xa8a59du) : (x % 4 == 0 ? 0x2a2a2au : 0x1a1a1au);
            dq_rect(944 + x * 268, y + 24, 944 + x * 268 + 236, y + 24 + 683, col);
        }
    }
    if (cur >= 0) dq_stroke(928 + (cur & 15) * 268, 32, 268, 4416, 24, COL_RED);               /* the playing step: 1.5 px red frame */
    dq_stroke(928 - 16, 48 + (sv * 2192) / 3, 16 * 268 + 32, 731, 16, COL_RED);                 /* the selected lane */
}
