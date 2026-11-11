/* SPDX-License-Identifier: MPL-2.0 */
/* The settings pages of the Minimoog, Prophet and drum engines (the prototype's EPG tables), shown on the knobs when the
 * selected track runs that engine: MIXER, OSCILLATOR 1..4, FILTER, LOUDNESS / CONTOUR (AMP ENV / FILTER ENV) and, for the
 * drums, the voice pages (EDIT, ENV) and the six-strip mixer.  Each page has its scope graph (dengine() / ddrum() of the
 * prototype).  The knobs edit the engine's own parameters through voices_param(); values are thousandths like params.h.
 * Included from ui.c (single translation unit, after pages.c). */
#include "voices.h"

/* ---------------------------------------------------------------- engine parameter descriptors ---- */
enum { EF_PCT, EF_MSA, EF_MSD, EF_C7, EF_C12, EF_SEL, EF_SHA, EF_SHB };     /* how the LCD prints it */
typedef struct { uint8_t fmt, step, nopt; const char *const *opts; } epd_t;
static const char *const O_WAVE[] = { "Tri", "Saw", "Square", "Pulse" };
static const char *const O_LFO[] = { "Sine", "Tri", "Square", "S&H" };
static const char *const O_RNG[] = { "32'", "16'", "8'", "4'", "2'" };
static const char *const O_NZ[] = { "White", "Pink", "Brown", "Blue", "Crackle" };
static const char *const O_REL[] = { "Short", "= Decay" };
static const char *const O_SSW[] = { "Sine", "Tri", "Square" };
static const char *const O_SOCT[] = { "-1 oct", "-2 oct" };
static const char *const O_PSN[] = { "Saw", "Saw+Tri", "Tri", "Tri+Pul", "Pulse", "Saw+Pul", "Sync" };
#define PC(s) { EF_PCT, s, 0, 0 }
#define SL(n, o) { EF_SEL, 1, n, o }
#define OSC_MG { SL(4, O_WAVE), SL(5, O_RNG), { EF_C7, 5, 0, 0 }, PC(10), PC(10) }
static const epd_t MGD[MG_COUNT] = {
    SL(4, O_WAVE), SL(5, O_RNG), { EF_C7, 5, 0, 0 }, PC(10), PC(10),
    SL(4, O_WAVE), SL(5, O_RNG), { EF_C7, 5, 0, 0 }, PC(10), PC(10),
    SL(4, O_WAVE), SL(5, O_RNG), { EF_C7, 5, 0, 0 }, PC(10), PC(10),
    SL(5, O_NZ), PC(5), PC(5), PC(10),
    PC(5), PC(10), PC(10), PC(10), SL(2, O_REL),
    { EF_MSA, 5, 0, 0 }, { EF_MSD, 5, 0, 0 }, PC(10), { EF_MSA, 5, 0, 0 }, { EF_MSD, 5, 0, 0 }, PC(10), PC(10) };
static const epd_t PRD[PR_COUNT] = {
    { EF_SHA, 5, 0, 0 }, SL(5, O_RNG), { EF_C12, 5, 0, 0 }, PC(10),
    { EF_SHB, 5, 0, 0 }, SL(5, O_RNG), { EF_C12, 5, 0, 0 }, PC(10),
    PC(10), PC(10),
    SL(3, O_SSW), SL(2, O_SOCT), { EF_C7, 5, 0, 0 }, PC(10), PC(10),
    SL(5, O_NZ), PC(5), PC(5), PC(10),
    PC(5), PC(10), PC(10), PC(10),
    { EF_MSA, 5, 0, 0 }, { EF_MSD, 5, 0, 0 }, PC(10), { EF_MSD, 5, 0, 0 },
    { EF_MSA, 5, 0, 0 }, { EF_MSD, 5, 0, 0 }, PC(10), { EF_MSD, 5, 0, 0 }, PC(10) };
static const char *const O_TBW[] = { "Saw", "Square" };
static const epd_t TBD[TB_COUNT] = { SL(2, O_TBW), { EF_C7, 5, 0, 0 }, PC(10), PC(10), PC(10), PC(10), PC(10), PC(10), PC(10), PC(10) };
static const epd_t SSD[SS_COUNT] = { { EF_C7, 5, 0, 0 }, SL(5, O_RNG), PC(10), PC(10), PC(10), PC(10), PC(10), PC(10), { EF_MSA, 5, 0, 0 }, { EF_MSD, 5, 0, 0 }, PC(10), { EF_MSD, 5, 0, 0 },
                                     { EF_MSA, 5, 0, 0 }, { EF_MSD, 5, 0, 0 }, PC(10), PC(10), PC(10), PC(10) };
static const epd_t V3D[V3_COUNT] = { { EF_C7, 5, 0, 0 }, PC(10), PC(10), PC(10), PC(10), SL(4, O_LFO), PC(10), PC(10), PC(10), PC(10), PC(10), PC(10), PC(10),
                                     PC(10), PC(10), PC(10), PC(10), PC(10), PC(10), PC(10), PC(10), PC(10), PC(10), PC(10),
                                     { EF_MSA, 5, 0, 0 }, { EF_MSD, 5, 0, 0 }, PC(10), { EF_MSD, 5, 0, 0 }, PC(10) };
static const epd_t DRD[DR_PER] = { PC(10), PC(10), PC(10), PC(10), PC(10), PC(10), PC(10), PC(10), PC(10), PC(10), PC(10), PC(10), PC(10), PC(10), PC(10), PC(10) };

static const char *const DVN[3][8] = { { "BD", "SD", "CH", "OH", "CP", "CB", "HT", "LT" }, { "BD", "SD", "CH", "OH", "CP", "RD", "HT", "LT" },
                                       { "KICK", "SNR", "CH", "OH", "TOM", "CRSH", "HI T", "LO T" } };
static const char *const KITN[3] = { "808", "909", "ACOU" };
static const char *const DVL[3][8] = {
    { "KICK", "SNARE", "CLOSED HAT", "OPEN HAT", "CLAP", "COWBELL", "HI TOM", "LOW TOM" },
    { "KICK", "SNARE", "CLOSED HAT", "OPEN HAT", "CLAP", "RIDE", "HI TOM", "LOW TOM" },
    { "KICK", "SNARE", "CLOSED HAT", "OPEN HAT", "TOM", "CRASH", "HI TOM", "LOW TOM" } };
static const char *const DV3[8] = { "CLICK", "SNAPPY", "TONE", "TONE", "TONE", "TONE", "CLICK", "CLICK" };

/* ---------------------------------------------------------------- the pages ---- */
enum { EPI_GLO, EPI_OSC1, EPI_OSC2, EPI_OSC3, EPI_OSC4, EPI_EDIT, EPI_ENV, EPI_ENV2, EPI_ARP, EPI_N };      /* EPI_ARP: the drums' lane settings (ARP) */
typedef struct { const char *title; const char *nm[4]; int8_t id[4]; } epg_t;
static const epg_t MG_PG[EPI_N] = {
    { "MIXER", { "OSC 1", "OSC 2", "OSC 3", "NOISE" }, { MG_L1, MG_L2, MG_L3, MG_NLEV } },
    { "OSCILLATOR 1", { "WAVE", "RANGE", "TUNE", "WIDTH" }, { MG_W1, MG_R1, MG_T1, MG_P1 } },
    { "OSCILLATOR 2", { "WAVE", "RANGE", "TUNE", "WIDTH" }, { MG_W2, MG_R2, MG_T2, MG_P2 } },
    { "OSCILLATOR 3", { "WAVE", "RANGE", "TUNE", "WIDTH" }, { MG_W3, MG_R3, MG_T3, MG_P3 } },
    { "NOISE", { "TYPE", "TONE", "CRUSH", "LEVEL" }, { MG_NTYPE, MG_NTONE, MG_NCRUSH, MG_NLEV } },
    { "FILTER", { "CUTOFF", "EMPHASIS", "CONTOUR", "GLIDE" }, { MG_CUT, MG_RES, MG_ENV, MG_GLIDE } },
    { "LOUDNESS", { "ATTACK", "DECAY", "SUSTAIN", "RELEASE" }, { MG_ATK, MG_DEC, MG_SUS, MG_REL } },
    { "CONTOUR", { "ATTACK", "DECAY", "SUSTAIN", "RELEASE" }, { MG_FATK, MG_FDEC, MG_FSUS, MG_REL } } };
static const epg_t PR_PG[EPI_N] = {
    { "MIXER", { "OSC A", "OSC B", "SUB", "NOISE" }, { PR_LA, PR_LB, PR_SL, PR_NLEV } },
    { "OSCILLATOR A", { "SHAPE", "RANGE", "TUNE", "WIDTH" }, { PR_WAVE, PR_RNG, PR_TUNE, PR_PW } },
    { "OSCILLATOR B", { "SHAPE", "RANGE", "TUNE", "WIDTH" }, { PR_BWAVE, PR_BRNG, PR_BTUNE, PR_BPW } },
    { "SUB OSCILLATOR", { "WAVE", "OCTAVE", "TUNE", "WIDTH" }, { PR_SW, PR_SO, PR_ST, PR_SP } },
    { "NOISE", { "TYPE", "TONE", "CRUSH", "LEVEL" }, { PR_NTYPE, PR_NTONE, PR_NCRUSH, PR_NLEV } },
    { "FILTER", { "CUTOFF", "RESO", "ENV AMT", "POLY-MOD" }, { PR_CUT, PR_RES, PR_ENV, PR_PM } },
    { "AMP ENV", { "ATTACK", "DECAY", "SUSTAIN", "RELEASE" }, { PR_ATK, PR_DEC, PR_SUS, PR_REL } },
    { "FILTER ENV", { "ATTACK", "DECAY", "SUSTAIN", "RELEASE" }, { PR_FATK, PR_FDEC, PR_FSUS, PR_FREL } } };

/* TB-303, Supersaw, Gremlin: their parameters fill only the pages they need (title 0 = the engine has no such page; ctrl.c eng_pmask() lists them); -1 = a knob without a parameter */
static const epg_t TB_PG[EPI_N] = {
    [EPI_GLO] = { "OSCILLATOR", { "WAVE", "TUNE", "DRIVE", "LEVEL" }, { TB_WAVE, TB_TUNE, TB_DRV, TB_LVL } },
    [EPI_EDIT] = { "FILTER", { "CUTOFF", "RESO", "ENV MOD", "DECAY" }, { TB_CUT, TB_RES, TB_ENV, TB_DEC } },
    [EPI_ENV] = { "ACCENT / SLIDE", { "ACCENT", "SLIDE", "", "" }, { TB_ACC, TB_SLD, -1, -1 } } };
static const epg_t SS_PG[EPI_N] = {
    [EPI_GLO] = { "SUPERSAW", { "TUNE", "RANGE", "DETUNE", "MIX" }, { SS_TUNE, SS_RNG, SS_DET, SS_MIX } },
    [EPI_OSC1] = { "VOICE", { "VOWEL", "FORMANT", "OCT UP", "DRIFT" }, { SS_VOW, SS_FRM, SS_OCT, SS_DRF } },
    [EPI_EDIT] = { "FILTER", { "CUTOFF", "RESO", "ENV AMT", "LEVEL" }, { SS_CUT, SS_RES, SS_ENV, SS_LVL } },
    [EPI_ENV] = { "AMP ENV", { "ATTACK", "DECAY", "SUSTAIN", "RELEASE" }, { SS_ATK, SS_DEC, SS_SUS, SS_REL } },
    [EPI_ENV2] = { "FILTER ENV", { "ATTACK", "DECAY", "", "" }, { SS_FATK, SS_FDEC, -1, -1 } } };
static const epg_t V3_PG[EPI_N] = {
    [EPI_GLO] = { "VCS3", { "TUNE", "O1 WAVE", "O2 WAVE", "O2 TUNE" }, { V3_TUNE, V3_O1W, V3_O2W, V3_O2T } },
    [EPI_OSC1] = { "MODULATION", { "O3 RATE", "O3 SHAPE", "", "" }, { V3_O3R, V3_O3S, -1, -1 } },
    [EPI_OSC2] = { "PATCH", { "SOURCE", "DEST", "PIN", "" }, { -1, -1, -1, -1 } },
    [EPI_OSC3] = { "MIXER", { "O1", "O2", "NOISE", "RING" }, { V3_M1, V3_M2, V3_MN, V3_MR } },
    [EPI_EDIT] = { "FILTER", { "CUTOFF", "RESO", "KEY", "LEVEL" }, { V3_CUT, V3_RES, V3_KEY, V3_LVL } },
    [EPI_ENV] = { "ENVELOPE", { "ATTACK", "DECAY", "SUSTAIN", "RELEASE" }, { V3_ATK, V3_DEC, V3_SUS, V3_REL } } };

static struct { char title[28]; const char *nm[4]; int16_t id[4]; int eng, idx; } EP;     /* the page on the screen */
static int ep_eng(void) { return voices_engine(voices_sel()); }
static int ep_pidx(void)                        /* the page as an engine page index, -1 when it is not one */
{
    switch (page) {
    case PG_GLO: return EPI_GLO;
    case PG_OSC1: return EPI_OSC1;
    case PG_OSC2: return EPI_OSC2;
    case PG_OSC3: return EPI_OSC3;
    case PG_OSC4: return EPI_OSC4;
    case PG_EDIT: return EPI_EDIT;
    case PG_ENV: return EPI_ENV;
    case PG_ENV2: return EPI_ENV2;
    case PG_ARP: return ep_eng() == ENG_DRUM ? EPI_ARP : -1;
    }
    return -1;
}
static int ep_strips(void) { return ep_eng() == ENG_DRUM && (page == PG_GLO || page == PG_GLO2); }     /* the drum mixer: six strips */
static int ep_on(void)                          /* the 2 x 2 knob page of a non-Vactrol engine? (fills EP) */
{
    int e = ep_eng(), x = ep_pidx(), i;
    const epg_t *p;
    if (e == ENG_VAC || x < 0 || ep_strips()) return 0;
    EP.eng = e; EP.idx = x;
    if (e == ENG_DRUM) {
        int v, k, y, n = 0;
        const char *s;
        if (x != EPI_EDIT && x != EPI_ENV && x != EPI_ARP) return 0;
        v = ctrl_drum_voice();
        k = voices_param_get(voices_sel(), e, DR_ID(v, DR_KV)); y = voices_param_get(voices_sel(), e, DR_ID(v, DR_TYPE));
        k = k < 0 ? 0 : k > 2 ? 2 : k; y = y < 0 ? 0 : y > 7 ? 7 : y;
        for (s = KITN[k]; *s;) EP.title[n++] = *s++;
        EP.title[n++] = ' ';
        for (s = DVL[k][y]; *s;) EP.title[n++] = *s++;
        if (x == EPI_ENV) for (s = " ENV"; *s;) EP.title[n++] = *s++;
        EP.title[n] = 0;
        if (x == EPI_ARP) {                                                     /* the lane's settings: "BD SEQ" */
            static const int8_t I[4] = { DR_SW, DR_HUM, DR_CHANCE, DR_ACC };
            n = 0;
            for (s = DVN[k][y]; *s;) EP.title[n++] = *s++;
            for (s = " SEQ"; *s;) EP.title[n++] = *s++;
            EP.title[n] = 0;
            EP.nm[0] = "SWING"; EP.nm[1] = "HUMAN."; EP.nm[2] = "CHANCE"; EP.nm[3] = "ACCENT";
            for (i = 0; i < 4; i++) EP.id[i] = (int16_t)DR_ID(v, I[i]);
        } else if (x == EPI_EDIT) {
            static const int8_t I[4] = { DR_TUNE, DR_TONE, DR_DRIVE, DR_FILT };
            EP.nm[0] = "TUNE"; EP.nm[1] = DV3[y]; EP.nm[2] = "DRIVE"; EP.nm[3] = "FILTER";
            for (i = 0; i < 4; i++) EP.id[i] = (int16_t)DR_ID(v, I[i]);
        } else {
            static const int8_t I[4] = { DR_ATK, DR_DECAY, DR_PEAK, DR_HOLD };
            EP.nm[0] = "ATTACK"; EP.nm[1] = "DECAY"; EP.nm[2] = "PITCH"; EP.nm[3] = "HOLD";
            for (i = 0; i < 4; i++) EP.id[i] = (int16_t)DR_ID(v, I[i]);
        }
        return 1;
    }
    p = e == ENG_MOOG ? &MG_PG[x] : e == ENG_PROPH ? &PR_PG[x] : e == ENG_TB3 ? &TB_PG[x] : e == ENG_SSAW ? &SS_PG[x] : &V3_PG[x];
    if (!p->title) p = e == ENG_TB3 ? &TB_PG[0] : e == ENG_SSAW ? &SS_PG[0] : &V3_PG[0];      /* a page the engine does not have */
    {
        int n = 0;
        const char *s;
        for (s = p->title; *s;) EP.title[n++] = *s++;
        EP.title[n] = 0;
    }
    for (i = 0; i < 4; i++) { EP.nm[i] = p->nm[i]; EP.id[i] = p->id[i]; }
    return 1;
}
static const epd_t *ep_desc(int id)
{
    switch (EP.eng) {
    case ENG_MOOG: return &MGD[id];
    case ENG_PROPH: return &PRD[id];
    case ENG_TB3: return &TBD[id];
    case ENG_SSAW: return &SSD[id];
    case ENG_VC3: return &V3D[id];
    }
    return &DRD[id % DR_PER];
}
static int ep_get(int id) { return voices_param_get(voices_sel(), EP.eng, id); }
static int32_t ep_n_q16(int id)                 /* the knob's position */
{
    const epd_t *d = ep_desc(id);
    int v = ep_get(id);
    if (d->fmt == EF_SEL) return d->nopt > 1 ? (v * 65536) / (d->nopt - 1) : 0;
    return v < 0 ? 0 : v > 1000 ? 65536 : (v * 65536 + 500) / 1000;
}
static void ep_set_n(int id, int32_t n)         /* setn(): value = n * range snapped to the step */
{
    const epd_t *d = ep_desc(id);
    int32_t v;
    if (n < 0) n = 0;
    if (n > 65536) n = 65536;
    if (d->fmt == EF_SEL)
        v = (n * (d->nopt - 1) + 32768) >> 16;
    else {
        int32_t raw = n * 1000, st = d->step;
        v = ((raw + (st << 15)) / (st << 16)) * st;
        if (v > 1000) v = 1000;
    }
    voices_param(voices_sel(), EP.eng, id, v);
}
static int ep_put_u(char *o, int v) { char t[8]; int n = 0, i, m = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); for (i = n - 1; i >= 0; i--) o[m++] = t[i]; return m; }
static void ep_cents(char *o, int v, int range)         /* Math.round(sign(d) * d * d * range) + "c", d = (v - .5) * 2 */
{
    int d = (v - 500) * 2, sg = d < 0 ? -1 : 1, n;
    int32_t num = sg * (d * d * range / 1000);          /* thousandths of a cent */
    int32_t t = num + 500;
    n = t >= 0 ? t / 1000 : -((-t + 999) / 1000);                           /* floor((x + .5)): Math.round, halves go up */
    if (n < 0) *o++ = '-';
    o += ep_put_u(o, n < 0 ? -n : n);
    *o++ = 'c'; *o = 0;
}
static void ep_text(int id, char *out)          /* what the cell prints */
{
    const epd_t *d = ep_desc(id);
    int v = ep_get(id);
    const char *s;
    switch (d->fmt) {
    case EF_SEL:
        for (s = d->opts[v < 0 ? 0 : v >= d->nopt ? d->nopt - 1 : v]; *s;) *out++ = *s++;
        *out = 0;
        break;
    case EF_SHA: case EF_SHB: {
        int i = (v * (d->fmt == EF_SHA ? 6 : 5) + 500) / 1000;
        for (s = O_PSN[i]; *s;) *out++ = *s++;
        *out = 0;
        break;
    }
    case EF_MSA: param_text_ms(1, v, out); break;
    case EF_MSD: param_text_ms(0, v, out); break;
    case EF_C7: ep_cents(out, v, 700); break;
    case EF_C12: ep_cents(out, v, 1200); break;
    default:
        out += ep_put_u(out, (v * 100 + 500) / 1000);
        *out++ = ' '; *out++ = '%'; *out = 0;
    }
}
/* the VC3 patch matrix (the OSC2 page): K1 = source row, K2 = destination column, SEL = put / take the pin, K3 = the pin's amount */
static const int8_t VM_PIN[4][4] = { { V3_L1, V3_L2, V3_LF, V3_LA }, { V3_E1, V3_E2, V3_EF, -1 }, { -1, V3_X2, -1, V3_XA }, { V3_F1, V3_F2, -1, -1 } };
static const char *const VM_SRC[4] = { "LFO", "ENV", "OSC1", "FILT" };
static const char *const VM_DST[4] = { "PCH1", "PCH2", "CUT", "AMP" };
static int vm_r, vm_c;
static int vm_on(void) { return EP.eng == ENG_VC3 && EP.idx == EPI_OSC2; }
static int vm_id(void) { return VM_PIN[vm_r][vm_c]; }
static int vm_val(int r, int c) { int id = VM_PIN[r][c]; return id < 0 ? -1 : voices_param_get(voices_sel(), ENG_VC3, id); }
static void vm_draw(void);
static void vm_encoder(int k, int steps)
{
    int id;
    if (k == 0) { vm_r += steps; vm_r = vm_r < 0 ? 0 : vm_r > 3 ? 3 : vm_r; }
    else if (k == 1) { vm_c += steps; vm_c = vm_c < 0 ? 0 : vm_c > 3 ? 3 : vm_c; }
    else if (k == 2 && (id = vm_id()) >= 0) {
        int v = voices_param_get(voices_sel(), ENG_VC3, id) + steps * 20;
        v = (v + 5) / 10 * 10;
        voices_param(voices_sel(), ENG_VC3, id, v < 0 ? 0 : v > 1000 ? 1000 : v);
    }
}
static void vm_cell(int k, int32_t *q16, const char **txt, char *buf, int *dim)      /* what a knob cell shows on this page */
{
    int id = vm_id();
    *dim = 0; *txt = "";
    if (k == 0) { *q16 = vm_r * 21845; *txt = VM_SRC[vm_r]; }
    else if (k == 1) { *q16 = vm_c * 21845; *txt = VM_DST[vm_c]; }
    else if (k == 2 && id >= 0) {
        int v = voices_param_get(voices_sel(), ENG_VC3, id), n = 0;
        *q16 = (v * 65536 + 500) / 1000;
        if (v == 0) { buf[0] = 'O'; buf[1] = 'F'; buf[2] = 'F'; buf[3] = 0; }
        else { n = ep_put_u(buf, (v * 100 + 500) / 1000); buf[n++] = ' '; buf[n++] = '%'; buf[n] = 0; }
        *txt = buf;
    } else if (k == 2) { *q16 = 0; buf[0] = '-'; buf[1] = '-'; buf[2] = 0; *txt = buf; }
    else *dim = 1;
}
int ui_sel_tap(void)                           /* SEL pressed on the patch matrix: put the pin (50 %) or take it away */
{
    int id;
    if (!ep_on() || !vm_on() || (id = vm_id()) < 0) return 0;
    voices_param(voices_sel(), ENG_VC3, id, voices_param_get(voices_sel(), ENG_VC3, id) ? 0 : 500);
    return 1;
}
static int32_t ep_wheel[4], ep_wg[4];
static int ep_wheel_id[4] = { -1, -1, -1, -1 };
static void ep_encoder(int k, int steps)        /* one detent = 1/50 of the range, from the knob's unrounded position */
{
    int i, n = steps < 0 ? -steps : steps;
    if (!ep_on()) return;
    if (vm_on()) { vm_encoder(k, steps); return; }
    for (i = 0; i < n; i++) {
        int id = EP.id[k];
        int32_t g, base, nv;
        if (id < 0) return;
        g = ep_n_q16(id);
        base = (ep_wheel_id[k] == id + 1000 * EP.eng && g == ep_wg[k]) ? ep_wheel[k] : g;
        nv = base + (steps < 0 ? -1 : 1) * (65536 / 50);
        {   /* few values (an oscillator's octave, the noise type, a wave ...): one detent = one value (see ui_encoder) */
            const epd_t *dd = ep_desc(id);
            int cnt = dd->fmt == EF_SEL ? dd->nopt : (dd->step > 0 && 1000 / dd->step + 1 <= 32) ? 1000 / dd->step + 1 : 0;
            if (cnt >= 2) {
                int idx = (int)(((int64_t)g * (cnt - 1) + 32768) >> 16) + (steps < 0 ? -1 : 1);
                idx = idx < 0 ? 0 : idx > cnt - 1 ? cnt - 1 : idx;
                nv = (int32_t)(((int64_t)idx * 65536 + (cnt - 1) / 2) / (cnt - 1));
            }
        }
        if (nv < 0) nv = 0;
        if (nv > 65536) nv = 65536;
        ep_set_n(id, nv);
        ep_wheel[k] = nv;
        ep_wg[k] = ep_n_q16(id);
        ep_wheel_id[k] = id + 1000 * EP.eng;
    }
}

static void epm_encoder(int k, int steps);     /* the drum mixer strips (engine_graphs.c) */
static void epm_draw(void);
