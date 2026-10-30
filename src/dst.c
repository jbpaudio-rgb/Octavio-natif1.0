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
static uint16_t SQG[4] = { 0x5555u, 0x5555u, 0x5555u, 0x5555u };     /* the synth sequencer's gate steps of each track (the prototype's TRX[t].GT) */
static uint8_t mod_view;                               /* the track whose matrix is in PV[] */

static int dst_count(int eng, int sq) { return DSTLN[eng & 3] + (sq ? 3 : 0); }
static int dst_at(int eng, int sq, int pos)            /* the stored value of list position pos */
{
    if (!sq) return DSTL[eng & 3][pos];
    return pos < 4 ? pos : DSTL[eng & 3][pos - 3] + 3;
}
static int dst_pos(int eng, int sq, int val)           /* list position of a stored value, -1: the engine does not have it */
{
    int i;
    if (val < 0) return -1;
    if (sq) { if (val < 4) return val; val -= 3; }
    else if (val == 0) return 0;
    for (i = 1; i < DSTLN[eng & 3]; i++) if (DSTL[eng & 3][i] == val) return sq ? i + 3 : i;
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
    int sq = dst_kind_of(id) == 2, n = dst_count(eng, sq), p = dst_pos(eng, sq, PV[id]);
    if (p < 0) p = 0;
    return n > 1 ? (int32_t)(((int64_t)p << 16) / (n - 1)) : 0;
}
static void dst_set_n(int id, int eng, int32_t n_q16)
{
    int sq = dst_kind_of(id) == 2, c = dst_count(eng, sq), p = (int)(((int64_t)n_q16 * (c - 1) + 32768) >> 16);
    p = p < 0 ? 0 : p >= c ? c - 1 : p;
    PV[id] = (int16_t)dst_at(eng, sq, p);
}

/* ---- the matrix per track ---- */
static void mod_commit(void) { memcpy(MODT[mod_view].m, &PV[P_M0T], sizeof MODT[0].m); memcpy(MODT[mod_view].q, &PV[P_M4T], sizeof MODT[0].q); }
static void mod_load_view(void) { memcpy(&PV[P_M0T], MODT[mod_view].m, sizeof MODT[0].m); memcpy(&PV[P_M4T], MODT[mod_view].q, sizeof MODT[0].q); }
static void mod_fix(int eng, int16_t *m, int16_t *q)   /* rebuildDst(): a destination the engine does not have goes back to None */
{
    int i;
    for (i = 0; i < 6; i++) {
        if (dst_pos(eng, 0, m[5 * i]) < 0) m[5 * i] = 0;
        if (dst_pos(eng, 1, q[4 * i]) < 0) q[4 * i] = 0;
    }
}
static void mod_fix_track(int t)                       /* the engine of track t changed */
{
    t &= 3;
    if (t == mod_view) mod_fix(voices_engine(t), &PV[P_M0T], &PV[P_M4T]);
    else mod_fix(voices_engine(t), MODT[t].m, MODT[t].q);
}
static void mod_defaults(void)
{
    int t, i;
    for (t = 0; t < 4; t++) {
        for (i = 0; i < MODM_N; i++) MODT[t].m[i] = PD[P_M0T + i].def;
        for (i = 0; i < MODQ_N; i++) MODT[t].q[i] = PD[P_M4T + i].def;
    }
    for (t = 0; t < 4; t++) SQG[t] = 0x5555u;
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
    for (t = 0; t < 4; t++) mod_fix(voices_engine(t), MODT[t].m, MODT[t].q);
    mod_load_view();
}
/* the record section (state.c SK_MODT): index i = track * MODT_N + (0..29 matrix, 30..53 lines) */
static int modt_get(int i) { int t = i / MODT_N, r = i % MODT_N; return r == MODM_N + MODQ_N ? (int16_t)SQG[t] : r < MODM_N ? MODT[t].m[r] : MODT[t].q[r - MODM_N]; }
static int modt_def(int i) { int r = i % MODT_N; return r == MODM_N + MODQ_N ? 0x5555 : r < MODM_N ? PD[P_M0T + r].def : PD[P_M4T + r - MODM_N].def; }
static void modt_put(int i, int v)
{
    int t = i / MODT_N, r = i % MODT_N;
    if (r == MODM_N + MODQ_N) { SQG[t] = (uint16_t)v; return; }
    const pdesc_t *d = &PD[r < MODM_N ? P_M0T + r : P_M4T + r - MODM_N];
    v = v < d->min ? d->min : v > d->max ? d->max : v;
    if (r < MODM_N) MODT[t].m[r] = (int16_t)v; else MODT[t].q[r - MODM_N] = (int16_t)v;
}
