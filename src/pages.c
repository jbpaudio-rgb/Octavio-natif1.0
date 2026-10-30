/* SPDX-License-Identifier: MPL-2.0 */
/* Page state and the pages whose cells depend on a selection: LFO (source 1..6) and FX (slot 1..6, its effect).
 * Included from ui.c (single translation unit). */
static int lfo_src;                                     /* 0..5: LFO 1, 2, 3, 4, S&H 1, S&H 2 */
static int fxo;                                         /* FX slot 0..FX_NSLOT-1 */
/* fxs[]: the effect of each slot of the selected track (fxstate.c) */

static const char *const FX_NAME[16] = { "OVERDRIVE", "PHASER", "FLANGER", "CHORUS", "MICRO PITCH", "DELAY", "REVERB", "GRANULAR",
                                         "FEEDBACK", "BITCRUSH", "RING MOD", "STUTTER", "REVERSE", "FREEZE", "TAPE", "SHIMMER" };
/* the parameters of each effect in the prototype's order (names index FX_PL), mix last */
static const char *const FX_PL[] = { "DRIVE", "TONE", "RATE", "DEPTH", "FEEDBACK", "MIX", "DETUNE", "TIME", "SIZE", "DAMPING", "CRUSH", "DENSITY",
                                     "PITCH", "FREQ", "SHAPE", "LENGTH", "CHANCE", "HOLD", "WOW", "AGE", "SHIFT", "DECAY" };
enum { L_DRIVE, L_TONE, L_RATE, L_DEPTH, L_FB, L_MIX, L_DETUNE, L_TIME, L_SIZE, L_DAMP, L_CRUSH, L_DENS, L_PITCH, L_FREQ, L_SHAPE, L_LEN, L_CHANCE, L_HOLD,
       L_WOW, L_AGE, L_SHIFT, L_DECAY };
static const struct { uint8_t n; uint8_t l[4]; } FX_P[16] = {
    { 3, { L_DRIVE, L_TONE, L_MIX } }, { 4, { L_RATE, L_DEPTH, L_FB, L_MIX } }, { 4, { L_RATE, L_DEPTH, L_FB, L_MIX } }, { 3, { L_RATE, L_DEPTH, L_MIX } },
    { 2, { L_DETUNE, L_MIX } }, { 3, { L_TIME, L_FB, L_MIX } }, { 3, { L_SIZE, L_DAMP, L_MIX } }, { 4, { L_SIZE, L_DENS, L_PITCH, L_MIX } },
    { 4, { L_FREQ, L_FB, L_TONE, L_MIX } }, { 3, { L_CRUSH, L_RATE, L_MIX } }, { 3, { L_FREQ, L_SHAPE, L_MIX } }, { 3, { L_LEN, L_CHANCE, L_MIX } },
    { 3, { L_TIME, L_FB, L_MIX } }, { 3, { L_SIZE, L_HOLD, L_MIX } }, { 4, { L_WOW, L_DRIVE, L_AGE, L_MIX } }, { 3, { L_DECAY, L_SHIFT, L_MIX } } };
static const uint8_t FX_FIRST[16] = { P_F0DRIVE, P_F1RATE, P_F2RATE, P_F3RATE, P_F4CENTS, P_F5TIME, P_F6SIZE, P_F7SIZE, P_F8FREQ, P_F9CRUSH, P_F10FREQ,
                                      P_F11LEN, P_F12TIME, P_F13SIZE, P_F14WOW, P_F15DECAY };
static const uint8_t LFO_BASE[6] = { P_M0T, P_M1T, P_M2T, P_M10T, P_M3T, P_M11T };       /* dest, shape|smooth, rate, amount, sync */

/* cell k of the LFO / FX page: the parameter id (-1: a dimmed, empty cell), its label and whether it is dimmed */
static int dyn_cell(int k, const char **label, int *dim)
{
    int id = -1;
    const char *lab = "\x03";
    if (page == PG_LFO) {
        static const char *const NM[4] = { "DEST", "SHAPE", "RATE", "AMOUNT" };
        id = LFO_BASE[lfo_src] + k;
        lab = lfo_src >= 4 && k == 1 ? "SMOOTH" : NM[k];
    } else if (page == PG_FX) {
        int fx = fxs[fxo], n = FX_P[fx].n;
        if (k == 3) { id = FX_FIRST[fx] + n - 1; lab = FX_PL[L_MIX]; }
        else if (k < n - 1) { id = FX_FIRST[fx] + k; lab = FX_PL[FX_P[fx].l[k]]; }
    }
    if (label) *label = lab;
    if (dim) *dim = id < 0;
    return id;
}
static int dyn_title(char *b)
{
    int n = 0;
    const char *t;
    if (page == PG_LFO) {
        t = lfo_src >= 4 ? "S&H " : "LFO ";
        while (*t) b[n++] = *t++;
        b[n++] = (char)('0' + (lfo_src >= 4 ? lfo_src - 3 : lfo_src + 1));
    } else {
        b[n++] = 'F'; b[n++] = 'X'; b[n++] = (char)('1' + fxo); b[n++] = ' ';
        for (t = FX_NAME[fxs[fxo]]; *t;) b[n++] = *t++;
    }
    b[n] = 0;
    return n;
}

static const char *const DLY_NAME[14] = { "1/32T", "1/32", "1/32.", "1/16T", "1/16", "1/16.", "1/8T", "1/8", "1/8.", "1/4T", "1/4", "1/4.", "1/2T", "1/2" };
/* what the cell prints for parameter id (the prototype's value rules, page by page) */
static void cell_text(int id, char *v)
{
    if (dst_kind_of(id)) { const char *o = PD[id].opts[dst_pos(voices_engine(voices_sel()), dst_kind_of(id) == 2, PV[id]) < 0 ? 0 : PV[id]]; int j; for (j = 0; (v[j] = o[j]) != 0; j++) {} }
    else param_text(id, v);
    if (page == PG_ENV && id == P_MODE && v[0] == 'L')                         /* ENV: "Low pass" -> "LP" */
        v[1] = 'P', v[2] = 0;
    if (id == P_F5TIME && PV[P_F5SYNC]) {                                      /* synced delay: the note value of the knob's 14 steps */
        int i = PV[id] * 14 / 1000, j;
        const char *s = DLY_NAME[i > 13 ? 13 : i];
        for (j = 0; (v[j] = s[j]) != 0; j++) {}
    }
}
