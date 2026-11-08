/* SPDX-License-Identifier: MPL-2.0 */
/* Per-track FX state (the prototype's TRX[t].fxs + fxqT[t]): for each of the 4 tracks the order of its FX_NSLOT slots and
 * every effect's knobs (the 16 effects' parameters P_F0DRIVE..P_F15MIX + the delay's sync switch), in a small versioned
 * structure that can be saved as bytes (fxbank_save / fxbank_load).
 *
 * The selected track's knobs live in PV[] (the pages edit PV[] like every other parameter); the other tracks' are in
 * FXT[t].p.  fx_select() swaps them when the selected track changes, fx_commit() copies PV[] -> FXT[view].p (the audio
 * does it every block).  Included from main.c after params.c and before voices.c / ctrl.c / ui.c. */

#define FX_NTRK 4
/* MAXTRACKS begin (temporary stability setting, see NOTES-M.md "MAX TRACKS": delete every MAXTRACKS block + the 4 call sites to remove it) */
#ifndef FEATURE_MAXTRACKS
#define FEATURE_MAXTRACKS 1
#endif
#ifndef MAXTR_DEF
#define MAXTR_DEF 4                                  /* default (-DMAXTR_DEF=n only for the load tests) */
#endif
#if FEATURE_MAXTRACKS
static uint8_t max_tracks = MAXTR_DEF;                       /* global, not saved: at most this many tracks are rendered at once (1..4) */
#endif
/* MAXTRACKS end */
#define FX_NSLOT 4                                   /* effects in a track's chain (the prototype has 6: see NOTES-H) */
#define FX_NP 53                                     /* P_F0DRIVE .. P_F15MIX (52 knobs) + P_F5SYNC */
#define FXBANK_VER 1
typedef struct { uint8_t ord[FX_NSLOT]; int16_t p[FX_NP]; } fxt_t;   /* ord: effect id (0..15) of each slot; p: thousandths, ids P_F0DRIVE + k */
typedef char fx_np_check[(P_F5SYNC - P_F0DRIVE == FX_NP - 1 && P_F15MIX - P_F0DRIVE == FX_NP - 2) ? 1 : -1];

/* CPU measures (timer ticks, 24 MHz; one audio half-block of 128 samples = 69,660 ticks), one struct so a single --dump reads them all */
struct {
    uint32_t audio_max, audio_last, audio_sum, blocks, audio_max_w, fx_max, fx_last, mod_max;       /* words 0..7: NOTES-H */
#ifndef NO_PERF                                          /* octavio-J load test (NOTES-J): -DNO_PERF removes everything below and its hooks */
    uint32_t magic;                                      /*  8: 0x4A504552 */
    uint32_t nw, sum_w;                                  /*  9..10: blocks and block ticks after the warm-up (PERF_WARM blocks) */
    uint32_t n75, n90, n100;                             /* 11..13: warm blocks above 75 / 90 / 100 % of a block */
    uint32_t uf, uf_first;                               /* 14..15: lost audio boundaries (an ISR entry gap of 2 blocks or more) and the block index of the first */
    uint32_t gap_min, gap_max, last_in;                  /* 16..18: spacing of the audio ISR entries (a block = PERF_P) */
    uint32_t fx_sum, mod_sum, mod_last, mod_n;           /* 19..22: ticks of the chains / the mod matrix, summed over the warm blocks */
    uint32_t loop_n, loop_gap_max, last_loop;            /* 23..25: main-loop passes; the longest time between two passes (ticks) */
    uint32_t frame_n, frame_sum, frame_max, full_n, full_max, frame_gap_max, last_frame, rows_sum;       /* 26..33: lcd_frame calls: count, total / worst ticks, full-screen frames, longest gap between two frames */
    uint32_t key_n, key_sum, key_max, key_blk_max;       /* 34..37: note keys: ticks from the first scan that saw the key closed to the audio block that plays it; the block's own length */
    uint32_t enc_steps, btn_n;                           /* 38..39: encoder clicks and button presses taken by the main loop */
    uint32_t ustack_free, sstack_free, bss_end, ram_free;       /* 40..43: stack bytes never touched (painted at boot), end of .bss, bytes from there to the 0x01C74000 guard */
    uint32_t hist[24];                                   /* 44..67: warm blocks by length, bin = ticks / (PERF_P / 20): 5 % each, bin 20+ = over a block */
    uint32_t lt[8];                                      /* 68..75: what the load test set up (loadtest.c): routes, bpm, engines */
    uint32_t te_sum[4], te_max[4], tf_sum[4], tf_max[4]; /* 76..91: per track, warm blocks: ticks of the engine's render / of the track's effect chain (sum, worst) */
    uint32_t gov[8];                                     /* 92..99: governor: slots bypassed, drum-cap steps, voices stolen, lowest cap, cap now, blocks of cool-down left, ISR run (perf.blocks) that took the first note key, its scan-to-block ticks */
    uint32_t alloc_n, alloc_max, alloc_sum;              /* 100..102: fx_alloc calls (a chain's order changed) after the warm-up, worst and total ticks */
#ifdef LOADTEST
    uint16_t trace[640];                                 /* 103..422: ticks / 4 of the audio blocks 200 .. 839 (NOTES-M: the time series, to see what the governor does) */
#endif
#endif
} perf;
#ifdef FM1_TICKS_PER_US
#define PERF_NOW() fm1_ticks()
#else
#define PERF_NOW() 0u                                    /* host tests (tests/state_test.c): no timer */
#endif
#ifndef NO_PERF
#define PERF_P 69632u                                    /* one audio half-block in timer ticks (fm1-emu: 128 frames x 544; 69,660 at exactly 44.1 kHz) */
#define PERF_WARM 200u                                   /* blocks left out of the statistics (allocation, start-up) */
static void perf_audio(uint32_t t_in, uint32_t d, uint32_t fxt)       /* end of an audio block: entry tick, length, ticks of the chains */
{
    uint32_t gap = t_in - perf.last_in, b = d / (PERF_P / 20u);
    if (perf.blocks > 1u) {
        if (gap > 3u * PERF_P / 2u) { if (!perf.uf) perf.uf_first = perf.blocks; perf.uf += (gap + PERF_P / 2u) / PERF_P - 1u; }
        if (perf.blocks > PERF_WARM) {
            if (!perf.gap_min || gap < perf.gap_min) perf.gap_min = gap;
            if (gap > perf.gap_max) perf.gap_max = gap;
        }
    }
    perf.last_in = t_in;
    if (perf.blocks > PERF_WARM) {
        perf.nw++; perf.sum_w += d; perf.fx_sum += fxt; perf.mod_sum += perf.mod_last;
        perf.hist[b > 23u ? 23u : b]++;
#ifdef LOADTEST
        if (perf.blocks - 200u < 640u) perf.trace[perf.blocks - 200u] = (uint16_t)(d >> 2);
#endif
        if (d > PERF_P * 3u / 4u) perf.n75++;
        if (d > PERF_P * 9u / 10u) perf.n90++;
        if (d > PERF_P) perf.n100++;
    }
}
#endif
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
/* ---- CPU governor (octavio-J, NOTES-J): the ISR measures its own length with TIMER4 on the device, so the ceiling holds whatever the silicon's real speed is.
 * gov_peak = the second longest of the last 4 audio blocks (a single start-up / reallocation spike is ignored).  Above GOV_SHED the last-priority running effect
 * slot (track 4 slot 4 first) is bypassed, one every GOV_WAIT blocks; with no effect left running the number of drum voices sounding in all is capped (the quietest
 * tail is stolen).  Slots / voices come back only after GOV_COOL blocks, and only while the load stays under GOV_ADMIT.  -DNO_GOV: off (the NOTES-H behaviour). */
#ifndef GOV_P
#define GOV_P 69660u                                 /* one audio block in TIMER4 ticks */
#endif
#ifndef GOV_SHED
#define GOV_SHED (GOV_P / 100u * 50u)                /* M: was 74 %.  The fm1-emu tick is not the silicon's (nor another emulator's): the ceiling leaves room for a machine up to ~1.7x slower */
#endif
#ifndef GOV_ADMIT
#define GOV_ADMIT (GOV_P / 100u * 40u)
#endif
#ifndef GOV_AIM
#define GOV_AIM (GOV_P / 100u * 44u)                 /* a shed removes enough to land about here */
#endif
#ifndef GOV_EMERG
#define GOV_EMERG (GOV_P / 100u * 75u)               /* one block this long: shed now, without waiting for the window or the wait (the ISR is about to overrun) */
#endif
#define GOV_SETTLE 16u                              /* blocks measured before the governor acts at all (the start-up burst is not a load) */
#define GOV_WAIT 12u                                 /* blocks between two sheds: the 4-block window must show the effect of the last one */
#define GOV_COOL 480u                                /* blocks (1.4 s) after a shed before anything comes back */
#define GOV_DRUMS 6                                  /* drum voices in all: one drum track x 6 */
#define GOV_DMIN 2                                   /* the governor leaves at least this many drum voices sounding */
#define GOV_PMIN 3                                   /* ... and this many Prophet voices (taken only under a severe load) */
#define GOV_PROPH 20                                 /* Prophet voices in all: 4 tracks x 5 */
#define GOV_UNIT(c) (((c) * 7u) >> 3)                /* ticks per FX_COST unit: measured 0.66..0.86 (NOTES-J) */
static uint32_t gov_w[8], gov_peak, gov_last, gov_wait, gov_cool, gov_vcool, gov_n;       /* gov_cool: effects, gov_vcool: voices */          /* gov_n: blocks measured so far (once the ISR reports, nothing is admitted before 4) */
static uint8_t gov_on;                               /* the audio ISR reports (not in the host tests) */
static uint32_t pv_cost = 3500;                      /* ticks of one Prophet voice in a block (measured while it sounds) */
static uint32_t dv_cost = 3500;                      /* ticks of one drum voice in a block (measured while drums sound: voices.c; this is the emulator's figure) */
static uint8_t gov_wi, gov_back[2];                  /* [0] effects, [1] voices */
                    /* gov_back: back-off, 0..3: a shed soon after the previous one doubles the cool-down (no flapping at the border) */
static uint32_t gov_since[2];                        /* blocks since the last shed */
static uint32_t fx_alloc_blk;                        /* ticks the audio block spent in fx_alloc (a one-off: not load) */
static int dv_cap = GOV_DRUMS, pv_cap = GOV_PROPH;          /* voices allowed to sound at once: drums (all drum tracks together) and Prophet voices (all Prophet tracks together) */
struct { uint32_t fx, voices, steals, cap_min, unload; } gov;      /* activity counters (perf dump, loadtest) */
/* The debug indicator (HOME held + REC toggles it): what the audio block costs, measured by the device's own timer, over the last second and since boot, so the numbers
 * can be read on any emulator or on the silicon.  Published by the audio ISR once per ~1 s (344 blocks); drawn by overlay.c. */
struct {
    uint8_t on;                                          /* the panel is shown */
    uint32_t ver;                                        /* counts the publications (the screen redraws on a change) */
    uint32_t avg, max, maxall;                           /* permille of a block: mean / worst of the last second, worst since boot */
    uint32_t uf;                                         /* audio boundaries lost (an ISR entry more than 1.5 blocks after the previous one) */
    uint32_t jit, jitall;                                /* the ISR entry spacing deviating from one block: worst of the last second / since boot, permille of a block */
    uint32_t acc, mx, n, jmx, last_in;                   /* the running window */
    uint32_t fps, nfr, nfr0;                             /* LCD frames per second */
    uint8_t np, nd, nm, run, off, cd, cp, eco;                /* Prophet voices, drum voices, Minimoog tracks sounding; FX slots running / bypassed; the governor's drum and Prophet voice caps */
    uint8_t mt;                                          /* MAXTRACKS: the limit (the HUD shows it) */
    uint32_t keys, btn;                                  /* the debounced note keys (bit n = key n) and buttons the scan reports now (the HUD's input line: first real-FM-1 test) */
    uint8_t kmax;                                        /* most note keys seen down at once since the start */
    uint32_t irqoff;                                     /* longest span (ticks) the main loop found interrupts disabled in the last second: not measured here, 0 */
} hud;
#define HUD_P 69632u
static void gov_block(uint32_t d)                    /* end of an audio block: its length in ticks */
{
#ifndef NO_GOV
    uint32_t sum = 0;
    int i;
    gov_w[gov_wi++ & 7u] = d;
    for (i = 0; i < 8; i++) sum += gov_w[i];
    gov_peak = sum >> 3;                              /* M: the mean of the last 8 blocks (was: the second longest of 4, which followed every drum hit and note-on and made the border flap) */
    gov_last = d;
    if (gov_since[0] < 1000000u) { gov_since[0]++; gov_since[1]++; }
    if (gov_n < 1000u) gov_n++;
    gov_on = 1;
    if (gov_wait) gov_wait--;
    if (gov_cool) gov_cool--;
    if (gov_vcool) gov_vcool--;
#else
    (void)d;
#endif
}
static void gov_mark(int k, uint32_t *cd, uint32_t cool)       /* a shed of kind k happened: wait before the next, cool down before anything comes back (longer when it keeps happening) */
{
    if (gov_since[k] < 4u * GOV_COOL) { if (gov_back[k] < 3) gov_back[k]++; }
    else if (gov_since[k] >= 12u * GOV_COOL) gov_back[k] = 0;
    gov_since[k] = 0;
    gov_wait = GOV_WAIT;
    *cd = cool << gov_back[k];
}
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
void mod_fix_all(void);                              /* dst.c: modulation destinations of an effect that left the chain go back to None */
int fx_assign(int slot, int e)
{
    fxt_t *T = &FXT[fx_view];
    int j = -1, s;
    if (slot < 0 || slot >= FX_NSLOT || e < 0 || e > 15) return -1;
    if (T->ord[slot] == e) return 1;
    for (s = 0; s < FX_NSLOT; s++) if (T->ord[s] == e) j = s;
    if (j >= 0) T->ord[j] = T->ord[slot];
    T->ord[slot] = (uint8_t)e;
    mod_fix_all();
    return 1;
}
/* the prototype's FX ORDER reordering (fxs.splice(from, 1) then splice(to, 0, e)): the effect of slot `from` goes to slot `to`, the ones between shift by one.
 * Effects keep their knobs (they are per effect); the chain's delay lines are reallocated by fx_begin (the order is part of their signature). */
void fx_move(int from, int to)
{
    uint8_t o[FX_NSLOT], e;                          /* built aside, then copied: the audio never sees a chain with a repeated effect */
    int s;
    if (from < 0 || to < 0 || from >= FX_NSLOT || to >= FX_NSLOT || from == to) return;
    memcpy(o, FXT[fx_view].ord, sizeof o);
    e = o[from];
    if (from < to) for (s = from; s < to; s++) o[s] = o[s + 1];
    else for (s = from; s > to; s--) o[s] = o[s - 1];
    o[to] = e;
    memcpy(FXT[fx_view].ord, o, sizeof o);
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
