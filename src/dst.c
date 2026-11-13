/* SPDX-License-Identifier: MPL-2.0 */
/* Modulation destinations per engine, and the modulation matrix per track.
 *
 * The prototype keeps ONE destination list for the whole panel (DST) and shows each track the part its engine owns
 * (rebuildDst(): DGR() groups v / m / p / d, ORD order); a destination that the engine of the track does not have is put back
 * to "None" when the engine changes.  Its LFO / S&H / sequencer-line settings are per track (saveTrack / loadTrack).
 * Here: the master list N_DST (src/pext_gen.h, tools/gen_pext.py; entries 0..68 are the Vactrol list saved presets already
 * use, the rest is appended) with the view of each engine DSTL[engine]; the stored value of an LFO / S&H destination is the
 * master index, that of a sequencer line is master + 3 (0 none, 1..3 pitch).  PV[P_M0T..P_M11Y] and PV[P_M4T..P_M9G] are the
 * selected track's matrix, MODT[t] those of the others (swapped by mod_select(), as fx_select() does for the effects).
 * Included from main.c / the host tests after params.c, before voices.c. */
#include "voices.h"
#include "params.h"

enum { DK_NONE, DK_PV, DK_ENV, DK_LVL, DK_ARP, DK_FX, DK_ENG, DK_DRUM, DK_SQG };     /* how a destination is applied (lfo.c) */
#include "pext_gen.h"

#define MODM_N 30                                      /* PV[P_M0T..P_M11Y]: 6 sources x (dest, shape|smooth, rate, amount, sync) */
#define MODQ_N 24                                      /* PV[P_M4T..P_M9G]: 6 lines x (dest, key, amount, glide) */
#define MODT_N (MODM_N + MODQ_N + 1)                   /* + the track's sequencer gate bits (16 steps) */
static struct { int16_t m[MODM_N], q[MODQ_N]; } MODT[4];
static int8_t SQN[4][16];                        /* the note of each step of each track (semitones from the octave's C: the note-step page, SEQ once) */
static uint16_t SQG[4] = { 0, 0, 0, 0 };     /* the synth sequencer gate steps of each track (the prototype TRX[t].GT): empty at start (the prototype had track 1 on every other step) */
static uint8_t mod_view;                               /* the track whose matrix is in PV[] */

/* The effect destinations are only those of the effects in the track's chain (its 4 FX slots): dst_ft is the track the lists are
 * seen for (-1: no filter, all 52 effect knobs as the master list has them).  Effect e owns FXN[e] consecutive effect knobs. */
static int8_t dst_ft = -1;
static const uint8_t FXN[16] = { 3, 4, 4, 3, 2, 3, 3, 4, 4, 3, 3, 3, 3, 3, 4, 4 };
static int dst_fx_of(int k) { int e, a = 0; for (e = 0; e < 16; e++) { a += FXN[e]; if (k < a) return e; } return 15; }
static int dst_ok(int val)                             /* is the destination (a master index) offered for dst_ft */
{
    int e, s;
    if (dst_ft < 0 || val <= 0 || val >= N_DST || DSTM[val].kind != DK_FX) return 1;
    e = dst_fx_of(DSTM[val].a);
    for (s = 0; s < FX_NSLOT; s++) if (FXT[dst_ft].ord[s] == e) return 1;
    return 0;
}
static int dst_count(int eng, int sq)
{
    int i, n = 0;
    for (i = 0; i < DSTLN[eng]; i++) n += dst_ok(DSTL[eng][i]);
    return n + (sq ? 3 : 0);
}
static int dst_at(int eng, int sq, int pos)            /* the stored value of list position pos */
{
    int i;
    if (sq) { if (pos < 4) return pos; pos -= 3; }
    for (i = 0; i < DSTLN[eng]; i++) if (dst_ok(DSTL[eng][i]) && pos-- == 0) return DSTL[eng][i] + (sq && i ? 3 : 0);
    return 0;
}
static int dst_pos(int eng, int sq, int val)           /* list position of a stored value, -1: the engine (or the chain) does not have it */
{
    int i, p = 0;
    if (val < 0) return -1;
    if (sq) { if (val < 4) return val; val -= 3; }
    else if (val == 0) return 0;
    for (i = 0; i < DSTLN[eng]; i++) {
        if (!dst_ok(DSTL[eng][i])) continue;
        if (i > 0 && DSTL[eng][i] == val) return sq ? p + 3 : p;
        p++;
    }
    return -1;
}
static int dst_kind_of(int id)                         /* 1: an LFO / S&H destination, 2: a sequencer line's, 0: neither */
{
    if (id == P_M0T || id == P_M1T || id == P_M2T || id == P_M10T || id == P_M3T || id == P_M11T) return 1;
    if (id >= P_M4T && id <= P_M9G && ((id - P_M4T) & 3) == 0) return 2;
    return 0;
}
static int32_t dst_n_q16(int id, int eng)              /* the knob position of the destination in the engine's list */
{
    int sq = dst_kind_of(id) == 2, n, p;
    dst_ft = (int8_t)mod_view;
    n = dst_count(eng, sq); p = dst_pos(eng, sq, PV[id]);
    dst_ft = -1;
    if (p < 0) p = 0;
    return n > 1 ? (int32_t)(((int64_t)p << 16) / (n - 1)) : 0;
}
static void dst_set_n(int id, int eng, int32_t n_q16)
{
    int sq = dst_kind_of(id) == 2, c, p;
    dst_ft = (int8_t)mod_view;
    c = dst_count(eng, sq); p = (int)(((int64_t)n_q16 * (c - 1) + 32768) >> 16);
    p = p < 0 ? 0 : p >= c ? c - 1 : p;
    PV[id] = (int16_t)dst_at(eng, sq, p);
    dst_ft = -1;
}

/* ---- the matrix per track ---- */
static void mod_commit(void) { memcpy(MODT[mod_view].m, &PV[P_M0T], sizeof MODT[0].m); memcpy(MODT[mod_view].q, &PV[P_M4T], sizeof MODT[0].q); }
static void mod_load_view(void) { memcpy(&PV[P_M0T], MODT[mod_view].m, sizeof MODT[0].m); memcpy(&PV[P_M4T], MODT[mod_view].q, sizeof MODT[0].q); }
static void mod_fix(int t, int eng, int16_t *m, int16_t *q)   /* rebuildDst(): a destination the engine (or the track's effect chain) does not have goes back to None */
{
    int i;
    dst_ft = (int8_t)t;
    for (i = 0; i < 6; i++) {
        if (dst_pos(eng, 0, m[5 * i]) < 0) m[5 * i] = 0;
        if (dst_pos(eng, 1, q[4 * i]) < 0) q[4 * i] = 0;
    }
    dst_ft = -1;
}
static void mod_fix_track(int t)                       /* the engine of track t changed */
{
    t &= 3;
    if (t == mod_view) mod_fix(t, voices_engine(t), &PV[P_M0T], &PV[P_M4T]);
    else mod_fix(t, voices_engine(t), MODT[t].m, MODT[t].q);
}
static void mod_defaults(void)
{
    int t, i;
    for (t = 0; t < 4; t++) {
        for (i = 0; i < MODM_N; i++) MODT[t].m[i] = PD[P_M0T + i].def;
        for (i = 0; i < MODQ_N; i++) MODT[t].q[i] = PD[P_M4T + i].def;
    }
    for (t = 0; t < 4; t++) { int s; SQG[t] = 0u; for (s = 0; s < 16; s++) SQN[t][s] = 0; }          /* TRX[t].GT: no step triggers at start (was: track 1 every other step) */
    mod_view = 0;
}
void mod_select(int t)                                 /* the selected track changed: its matrix into PV[] */
{
    t &= 3;
    if (t == mod_view) return;
    mod_commit();
    mod_view = (uint8_t)t;
    mod_load_view();
}
static void mod_post_load(int have_modt)               /* after a preset / scene: the tracks' matrices follow their engines */
{
    int t;
    if (have_modt) mod_load_view(); else mod_commit();
    for (t = 0; t < 4; t++) mod_fix(t, voices_engine(t), MODT[t].m, MODT[t].q);
    mod_load_view();
}
void mod_fix_all(void) { int t; for (t = 0; t < 4; t++) mod_fix_track(t); }       /* an effect chain changed */
/* the record section (state.c SK_MODT): index i = track * MODT_N + (0..29 matrix, 30..53 lines) */
static int modt_get(int i) { int t = i / MODT_N, r = i % MODT_N; return r == MODM_N + MODQ_N ? (int16_t)SQG[t] : r < MODM_N ? MODT[t].m[r] : MODT[t].q[r - MODM_N]; }
static int modt_def(int i) { int r = i % MODT_N; return r == MODM_N + MODQ_N ? 0 : r < MODM_N ? PD[P_M0T + r].def : PD[P_M4T + r - MODM_N].def; }
static void modt_put(int i, int v)
{
    int t = i / MODT_N, r = i % MODT_N;
    if (r == MODM_N + MODQ_N) { SQG[t] = (uint16_t)v; return; }
    const pdesc_t *d = &PD[r < MODM_N ? P_M0T + r : P_M4T + r - MODM_N];
    v = v < d->min ? d->min : v > d->max ? d->max : v;
    if (r < MODM_N) MODT[t].m[r] = (int16_t)v; else MODT[t].q[r - MODM_N] = (int16_t)v;
}
