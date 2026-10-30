/* SPDX-License-Identifier: MPL-2.0 */
/* Per-track FX state (the prototype's TRX[t].fxs + fxqT[t]): for each of the 4 tracks the order of its FX_NSLOT slots and
 * every effect's knobs (the 16 effects' parameters P_F0DRIVE..P_F15MIX + the delay's sync switch), in a small versioned
 * structure that can be saved as bytes (fxbank_save / fxbank_load).
 *
 * The selected track's knobs live in PV[] (the pages edit PV[] like every other parameter); the other tracks' are in
 * FXT[t].p.  fx_select() swaps them when the selected track changes, fx_commit() copies PV[] -> FXT[view].p (the audio
 * does it every block).  Included from main.c after params.c and before voices.c / ctrl.c / ui.c. */

#define FX_NTRK 4
#define FX_NSLOT 4                                   /* effects in a track's chain (the prototype has 6: see NOTES-H) */
#define FX_NP 53                                     /* P_F0DRIVE .. P_F15MIX (52 knobs) + P_F5SYNC */
#define FXBANK_VER 1
typedef struct { uint8_t ord[FX_NSLOT]; int16_t p[FX_NP]; } fxt_t;   /* ord: effect id (0..15) of each slot; p: thousandths, ids P_F0DRIVE + k */
typedef char fx_np_check[(P_F5SYNC - P_F0DRIVE == FX_NP - 1 && P_F15MIX - P_F0DRIVE == FX_NP - 2) ? 1 : -1];

/* CPU measures (timer ticks, 24 MHz; one audio half-block of 128 samples = 69,660 ticks), one struct so a single --dump reads them all */
struct { uint32_t audio_max, audio_last, audio_sum, blocks, audio_max_w, fx_max, fx_last, mod_max; } perf;
#define fx_ticks_max perf.fx_max
#define fx_ticks_last perf.fx_last
#define mod_ticks_max perf.mod_max

static fxt_t FXT[FX_NTRK];
static uint8_t fx_view;                              /* the track whose knobs are in PV[] */
static uint8_t fx_off[FX_NTRK];                      /* bit s: slot s of the track is active (Mix > 0) but bypassed: it did not fit the CPU budget */
#define fxs (FXT[fx_view].ord)                       /* the pages' view: the effect of each slot of the selected track */

/* ---- CPU budget: worst-block ticks (24 MHz, 128 samples = 69,660) of each effect, measured on the target (NOTES-H); only slots with Mix > 0 count ---- */
static const uint16_t FX_COST[16] = { 722, 1216, 1183, 1643, 1570, 957, 2593, 7591, 1303, 424, 672, 696, 1953, 2144, 1952, 2790 };
#ifndef FX_BUDGET
#define FX_BUDGET 22000u                             /* in these units, for all the chains together: audio engines + mod worst case ~26k ticks, so the block stays under ~66 % (NOTES-H) */
#endif

void ui_params_changed(void);

static uint8_t fx_run[FX_NTRK];                      /* bit s: the active slot (Mix > 0) was admitted by the CPU budget and runs (fxdsp.c fx_begin) */
uint32_t fx_refused;                                 /* counts the slots the budget just refused (the main loop shows a toast) */
static void fx_check(void) { memset(fx_run, 0, sizeof fx_run); }      /* after a load: every active slot is admitted again, in the order track 1 slot 1 ... track 4 slot 4 */

static void fx_defaults(void)
{
    int t, s, k;
    for (t = 0; t < FX_NTRK; t++) {
        for (s = 0; s < FX_NSLOT; s++) FXT[t].ord[s] = (uint8_t)s;
        for (k = 0; k < FX_NP; k++) FXT[t].p[k] = PD[P_F0DRIVE + k].def;
    }
    fx_view = 0;
    fx_check();
}
static void fx_commit(void) { memcpy(FXT[fx_view].p, &PV[P_F0DRIVE], sizeof FXT[0].p); }
static void fx_load_view(void) { memcpy(&PV[P_F0DRIVE], FXT[fx_view].p, sizeof FXT[0].p); }
void fx_select(int t)                                /* the selected track changed: its knobs into PV[] */
{
    t &= 3;
    if (t == fx_view) return;
    fx_commit();
    fx_view = (uint8_t)t;
    fx_load_view();
}

/* assign effect e to slot `slot` of the selected track, as the prototype (the effect already in the chain swaps places).
 * 1: done, -1: invalid.  The CPU budget is not checked here: an effect costs CPU only while its Mix > 0, and it is admitted
 * (or bypassed, dimmed on the FX ORDER page) by fx_begin. */
int fx_assign(int slot, int e)
{
    fxt_t *T = &FXT[fx_view];
    int j = -1, s;
    if (slot < 0 || slot >= FX_NSLOT || e < 0 || e > 15) return -1;
    if (T->ord[slot] == e) return 1;
    for (s = 0; s < FX_NSLOT; s++) if (T->ord[s] == e) j = s;
    if (j >= 0) T->ord[j] = T->ord[slot];
    T->ord[slot] = (uint8_t)e;
    return 1;
}
int fx_slot_off(int t, int s) { return (fx_off[t & 3] >> s) & 1; }       /* the slot is active but bypassed by the budget */

/* ---- serialisation (octavio-G's presets): [ver, nslot, ntrk, np] then per track ord[nslot], p[np] as int16 little endian ----
 *   4 + 4 * (4 + 53 * 2) = 444 bytes.  fxbank_load accepts another nslot / np (a later firmware): what fits is taken, the rest
 *   keeps its default; values are clamped to their range, doubles in a chain are fixed. */
#define FXBANK_BYTES (4 + FX_NTRK * (FX_NSLOT + 2 * FX_NP))
uint32_t fxbank_save(uint8_t *b)                     /* b: FXBANK_BYTES; returns the size */
{
    int t, k, s, o = 4;
    fx_commit();
    b[0] = FXBANK_VER; b[1] = FX_NSLOT; b[2] = FX_NTRK; b[3] = FX_NP;
    for (t = 0; t < FX_NTRK; t++) {
        for (s = 0; s < FX_NSLOT; s++) b[o++] = FXT[t].ord[s];
        for (k = 0; k < FX_NP; k++) { uint16_t v = (uint16_t)FXT[t].p[k]; b[o++] = (uint8_t)v; b[o++] = (uint8_t)(v >> 8); }
    }
    return (uint32_t)o;
}
int fxbank_load(const uint8_t *b, uint32_t n)        /* 1: loaded, 0: not a bank of this version (nothing changed) */
{
    uint32_t ns, nt, np, o = 4, t, k, s;
    fxt_t tmp[FX_NTRK];
    if (n < 4 || b[0] != FXBANK_VER) return 0;
    ns = b[1]; nt = b[2]; np = b[3];
    if (!ns || !nt || !np || n < 4 + nt * (ns + 2 * np)) return 0;
    for (t = 0; t < FX_NTRK; t++) {
        for (s = 0; s < FX_NSLOT; s++) tmp[t].ord[s] = (uint8_t)s;
        for (k = 0; k < FX_NP; k++) tmp[t].p[k] = PD[P_F0DRIVE + k].def;
    }
    for (t = 0; t < nt; t++) {
        for (s = 0; s < ns; s++, o++) if (t < FX_NTRK && s < FX_NSLOT) tmp[t].ord[s] = b[o] & 15;
        for (k = 0; k < np; k++, o += 2) {
            if (t < FX_NTRK && k < FX_NP) {
                int v = (int16_t)(uint16_t)(b[o] | (b[o + 1] << 8)), mn = PD[P_F0DRIVE + k].min, mx = PD[P_F0DRIVE + k].max;
                tmp[t].p[k] = (int16_t)(v < mn ? mn : v > mx ? mx : v);
            }
        }
    }
    for (t = 0; t < FX_NTRK; t++) {                  /* one effect once per chain: a repeat becomes the first effect missing */
        uint32_t seen = 0;
        for (s = 0; s < FX_NSLOT; s++) {
            if (seen >> tmp[t].ord[s] & 1u) {
                uint32_t e = 0;
                while (e < 15 && (seen >> e & 1u)) e++;
                tmp[t].ord[s] = (uint8_t)e;
            }
            seen |= 1u << tmp[t].ord[s];
        }
    }
    memcpy(FXT, tmp, sizeof FXT);
    fx_load_view();
    fx_check();
    return 1;
}
