/* SPDX-License-Identifier: MPL-2.0 */
/* The prototype's overlays: the list (#pre), the name editor (#nm) and the toast (#tst), laid out from its CSS
 * (1 cqw = 2.4 px). Courier-style glyphs come from Chrome renders (tools/fonts_spec.json: ov_*). */
#include "octavio.h"
#include "ctrl.h"

#define C_PAPER 0xf2efe6u
#define C_INK   0x14110du
#define C_GREY  0xa8a59du
#define C_RED   0xff3b1fu
#define C_DKRED 0xb02a14u
#define C_LINE  0x3a3a3au

static void text_c(int f, const char *s, int cx, int base, uint32_t col, int32_t ls)    /* centred on cx (px) */
{
    int32_t w = gfx_text_w64(f, s, ls) - ls;
    gfx_text(f, s, (int32_t)cx * 64 - w / 2, base, col, ls, 0, SCR_W);
}
static void frame(int x, int y, int w, int h, int b, uint32_t col)
{
    gfx_rect(x, y, w, b, col, 256); gfx_rect(x, y + h - b, w, b, col, 256);
    gfx_rect(x, y, b, h, col, 256); gfx_rect(x + w - b, y, b, h, col, 256);
}

static void draw_list(void)
{
    char rows[5][24], tags[5][8], title[20], note[24];
    int cur[1], n, i;
    gfx_rect(0, 0, SCR_W, SCR_H, COL_BG, 256);
    n = ctrl_list_rows(rows, tags, cur, title, note);
    text_c(F_OV_PT, title, 120, 24, C_PAPER, 24);
    for (i = 0; i < n; i++) {
        int y = 33 + (i * 2964 + 50) / 100, on = i == cur[0];
        uint32_t fg = on ? C_INK : C_GREY, bg = on ? C_PAPER : COL_BG, ln = on ? C_PAPER : C_LINE;
        gfx_rect(10, y, 220, 27, bg, 256);
        frame(10, y, 220, 27, 2, ln);
        gfx_text(F_OV_ROW, rows[i], 16 * 64, y + 18, fg, 0, 0, SCR_W);
        if (tags[i][0]) {
            int32_t w = gfx_text_w64(F_OV_ROW, tags[i], 0);
            gfx_text(F_OV_ROW, tags[i], 224 * 64 - w, y + 18, on ? C_DKRED : C_RED, 0, 0, SCR_W);
        }
    }
    text_c(F_OV_PN, note, 120, 227, C_RED, 0);
}

static void draw_name(void)
{
    char t[16], el[32], one[2] = { 0, 0 };
    int pos, i;
    gfx_rect(0, 0, SCR_W, SCR_H, COL_BG, 256);
    if (!ctrl_name(t, &pos, el)) return;
    text_c(F_OV_PT, "NAME PRESET", 120, 24, C_PAPER, 24);
    for (i = 0; i < 12; i++) {
        int x = 17 + (i * 1728 + 50) / 100 * 1 + 0, cur = i == pos;
        x = 17 + (i * 17280 + 500) / 1000;
        if (cur) frame(x - 1, 36, 18, 24, 1, C_RED);
        gfx_rect(x, 37, 15, 22, cur ? C_PAPER : 0x1c1c1cu, 256);
        one[0] = t[i];
        if (t[i] != ' ') text_c(F_OV_CH, one, x + 8, 37 + 15, cur ? C_INK : C_PAPER, 0);
    }
    text_c(F_OV_HINT, "SELECT = LETTER \x02 SEL = NEXT", 120, 76, C_GREY, 0);
    text_c(F_OV_PN, el, 120, 98, C_RED, 0);
    gfx_rect(10, 205, 107, 26, 0xd9362bu, 256);
    text_c(F_OV_PN, "OK", 63, 222, 0xffffffu, 0);
    gfx_rect(124, 205, 107, 26, 0x232323u, 256);
    frame(124, 205, 107, 26, 1, C_LINE);
    text_c(F_OV_PN, "CANCEL", 177, 222, C_PAPER, 0);
}

static void draw_toast(void)
{
    char a[24], b[24];
    int h;
    if (!ctrl_toast(a, b)) return;
    h = b[0] ? 60 : 44;
    gfx_rect(16, 86, 208, h + 6, 0x000000u, 256);                        /* the 1.2 cqw shadow */
    gfx_rect(19, 89, 202, h, C_PAPER, 256);
    gfx_rect(21, 91, 198, h - 4, 0x0a0a0au, 256);
    text_c(F_OV_TST, a, 120, 89 + 2 + 10 + 16, C_PAPER, 0);
    if (b[0]) text_c(F_OV_SM, b, 120, 89 + 2 + 10 + 21 + 4 + 10, C_RED, 17);
}

/* the LFO page's DEST knob: the destination's name in a big framed box that shows while the knob turns */
static void draw_destpop(const char *t, const char *title)
{
    gfx_rect(12, 80, 216, 70, 0x000000u, 256);
    gfx_rect(15, 83, 210, 64, C_PAPER, 256);
    gfx_rect(17, 85, 206, 60, 0x0a0a0au, 256);
    text_c(F_OV_SM, title, 120, 85 + 16, C_RED, 17);
    text_c(F_VALUE, t, 120, 85 + 44, C_PAPER, 0);
}

static void put_n(char *o, uint32_t v, int pad)           /* unsigned, space padded to pad */
{
    char t[12]; int n = 0, i = 0;
    do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while (v && n < 11);
    while (n + i < pad) o[i++] = ' ';
    while (n) o[i++] = t[--n];
    o[i] = 0;
}
static void hud_pct(char *o, uint32_t pm)                 /* permille -> "41.5" */
{
    char a[12]; put_n(a, pm / 10u, 0);
    { int k = 0; while (a[k]) { o[k] = a[k]; k++; } o[k++] = '.'; o[k++] = (char)('0' + pm % 10u); o[k] = 0; }
}
static void draw_hud(void)
{
    char s[40], a[12], b[12], c[12]; int i, k;
    if (!hud.on) return;
    gfx_rect(0, 0, SCR_W, 82, 0x000000u, 256);
    gfx_rect(0, 81, SCR_W, 1, C_RED, 256);
    hud_pct(a, hud.avg); hud_pct(b, hud.max); hud_pct(c, hud.maxall);
    s[0] = 0; k = 0;
    { const char *p = "CPU AVG "; for (i = 0; p[i]; i++) s[k++] = p[i]; for (i = 0; a[i]; i++) s[k++] = a[i];
      p = " MAX "; for (i = 0; p[i]; i++) s[k++] = p[i]; for (i = 0; b[i]; i++) s[k++] = b[i];
      p = " ALL "; for (i = 0; p[i]; i++) s[k++] = p[i]; for (i = 0; c[i]; i++) s[k++] = c[i]; s[k++] = '%'; s[k] = 0; }
    gfx_text(F_OV_HINT, s, 4 * 64, 11, C_PAPER, 0, 0, SCR_W);
    hud_pct(a, hud.jit); hud_pct(b, hud.jitall);
    k = 0; { const char *p = "LOST "; for (i = 0; p[i]; i++) s[k++] = p[i]; put_n(c, hud.uf, 0); for (i = 0; c[i]; i++) s[k++] = c[i];
      p = "  ISR JITTER "; for (i = 0; p[i]; i++) s[k++] = p[i]; for (i = 0; a[i]; i++) s[k++] = a[i];
      p = " ALL "; for (i = 0; p[i]; i++) s[k++] = p[i]; for (i = 0; b[i]; i++) s[k++] = b[i]; s[k++] = '%'; s[k] = 0; }
    gfx_text(F_OV_HINT, s, 4 * 64, 22, hud.uf ? C_RED : C_GREY, 0, 0, SCR_W);
    k = 0; { const char *p = "VOICES PRO "; for (i = 0; p[i]; i++) s[k++] = p[i]; put_n(c, hud.np, 0); for (i = 0; c[i]; i++) s[k++] = c[i];
      p = " DRUM "; for (i = 0; p[i]; i++) s[k++] = p[i]; put_n(c, hud.nd, 0); for (i = 0; c[i]; i++) s[k++] = c[i];
      p = " MOOG "; for (i = 0; p[i]; i++) s[k++] = p[i]; put_n(c, hud.nm, 0); for (i = 0; c[i]; i++) s[k++] = c[i]; s[k] = 0; }
    gfx_text(F_OV_HINT, s, 4 * 64, 33, C_GREY, 0, 0, SCR_W);
    k = 0; { const char *p = "FX RUN "; for (i = 0; p[i]; i++) s[k++] = p[i]; put_n(c, hud.run, 0); for (i = 0; c[i]; i++) s[k++] = c[i];
      p = " BYPASSED "; for (i = 0; p[i]; i++) s[k++] = p[i]; put_n(c, hud.off, 0); for (i = 0; c[i]; i++) s[k++] = c[i];
      p = " (GOV "; for (i = 0; p[i]; i++) s[k++] = p[i]; put_n(c, gov.fx, 0); for (i = 0; c[i]; i++) s[k++] = c[i]; s[k++] = ')'; s[k] = 0; }
    gfx_text(F_OV_HINT, s, 4 * 64, 44, C_GREY, 0, 0, SCR_W);
    k = 0; { const char *p = "CAPS DRUM "; for (i = 0; p[i]; i++) s[k++] = p[i]; put_n(c, hud.cd, 0); for (i = 0; c[i]; i++) s[k++] = c[i];
      p = " PRO "; for (i = 0; p[i]; i++) s[k++] = p[i]; put_n(c, hud.cp, 0); for (i = 0; c[i]; i++) s[k++] = c[i];
      p = hud.eco ? " ECO  FPS " : "  FPS "; for (i = 0; p[i]; i++) s[k++] = p[i]; put_n(c, hud.fps, 0); for (i = 0; c[i]; i++) s[k++] = c[i]; s[k] = 0; }
    gfx_text(F_OV_HINT, s, 4 * 64, 55, C_GREY, 0, 0, SCR_W);
    {   /* what the scan reports: the note keys down (hex, bit n = key n, bit 0 = F3), the most at once so far, the buttons down (hex) */
        static const char HX[] = "0123456789ABCDEF";
        const char *p = "KEYS "; k = 0; for (i = 0; p[i]; i++) s[k++] = p[i];
        for (i = 6; i >= 0; i--) s[k++] = HX[(hud.keys >> (4 * i)) & 15u];
        p = " MAX "; for (i = 0; p[i]; i++) s[k++] = p[i]; put_n(c, hud.kmax, 0); for (i = 0; c[i]; i++) s[k++] = c[i];
        p = " BTN "; for (i = 0; p[i]; i++) s[k++] = p[i];
        for (i = 3; i >= 0; i--) s[k++] = HX[(hud.btn >> (4 * i)) & 15u];
        s[k] = 0; gfx_text(F_OV_HINT, s, 4 * 64, 77, C_GREY, 0, 0, SCR_W);
    }
    #if FEATURE_MAXTRACKS /* MAXTRACKS */
    { char m[40] = "MAX TRACKS n  PLAY=CHG REC=HIDE"; m[11] = (char)('0' + hud.mt); gfx_text(F_OV_HINT, m, 4 * 64, 66, hud.mt < 4 ? C_RED : 0x6a6862u, 0, 0, SCR_W); }
#else
    gfx_text(F_OV_HINT, "HOME + REC = HIDE", 4 * 64, 66, 0x6a6862u, 0, 0, SCR_W);
#endif
}

void overlay_draw(void)
{
    int o = ctrl_overlay();
    if (o == OV_LIST) draw_list();
    else if (o == OV_NAME) draw_name();
    draw_toast();
    draw_hud();
}
