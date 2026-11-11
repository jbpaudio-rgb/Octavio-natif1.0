/* SPDX-License-Identifier: MPL-2.0 */
/* The voice layer: tracks, key routing, arpeggiator, mix. See voices.h. */
#ifndef ENGINE_TRIM_Q15
#define ENGINE_TRIM_Q15 19661          /* 0.6 */
#endif
#include "voices.h"
#include "params.h"
#include "ui.h"
#include "eng_moog.c"
#include "eng_vc3.c"

int32_t mod_bus[4][12];
static int16_t vsc_buf[1024];                           /* the last 1024 samples of the selected track while it sounds (it keeps the last picture when it stops) */
static int vsc_w;
static void vsc_push(const int32_t *b, int n)
{
    int i, pk = 0;
    for (i = 0; i < n; i++) { int a = b[i] < 0 ? -b[i] : b[i]; if (a > pk) pk = a; }
    if (pk < 400) return;                                 /* the tail of a release: keep the last real picture */
    for (i = 0; i < n; i++) { int32_t v = b[i]; vsc_buf[vsc_w] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); vsc_w = (vsc_w + 1) & 1023; }
}
void voices_scope(int16_t *dst, int n)                  /* the last n (<= 1024) samples, oldest first */
{
    int i, r = (vsc_w - n) & 1023;
    for (i = 0; i < n; i++) dst[i] = vsc_buf[(r + i) & 1023];
}
#include "eng_pol.c"
#include "eng_tb3.c"
#include "eng_prophet.c"
#include "eng_drum.c"

#define NTRK 4
typedef struct {
    int16_t p[ARP_COUNT];
    uint8_t held[8], nheld;
    uint8_t list[256];
    uint16_t nlist, i, cur;
    int16_t last;                                         /* the note the arpeggiator is sounding on a polyphonic engine (-1: none) */
    int32_t cnt, goff;
    uint32_t down[4];                                     /* bitmap of the notes held (0..127) */
} arp_t;

static struct { uint8_t eng; int16_t vol; int32_t lvl; arp_t a; } TK[NTRK];       /* vol: Q15; lvl: the Level destination's gain (Q15, 32768 = none) */
#ifndef C_BOOT_TRACK
#define C_BOOT_TRACK 0
#endif
static uint8_t sel_t = C_BOOT_TRACK;
static uint32_t keys_prev_v;
static uint8_t key_note[27], key_trk[27];
static int ctrl_bank_key_cb(int key);
static void ctrl_key_rel_cb(int key);                 /* main.c: a chord key (no note) was released */                  /* main.c: a key pressed while the bank keys are active */
static int bpm_v = 120;
static int32_t lvl_prev[NTRK] = { 32768, 32768, 32768, 32768 };
static uint8_t vac_owner = 0;                              /* the Vactrol is a singleton: one track plays it */
static const uint8_t SPB[6] = {1, 2, 3, 4, 6, 8};
static const uint8_t PATS[6][8] = {{1, 1, 1, 1, 1, 1, 1, 1}, {1, 0, 0, 1, 0, 0, 1, 0}, {0, 1, 0, 1, 0, 1, 0, 1},
                                   {1, 0, 1, 1, 1, 0, 1, 1}, {1, 0, 0, 1, 0, 1, 0, 0}, {1, 0, 0, 0, 1, 0, 0, 0}};
static const uint8_t PATN[6] = {1, 8, 2, 4, 8, 4};
static const int16_t ARP_DEF[ARP_COUNT] = {0, 1, 0, 500, 0, 0, 0, 1000, -12, 1, 0, 0};      /* the prototype: rate 1/4 (list index 0), transpose "-12" (its select starts on the first option) */

static void eng_on(int t, int note, int vel15)
{
    switch (TK[t].eng) {
    case ENG_VAC: if (t == vac_owner) vac_note_on(note, vel15); break;
    case ENG_MOOG: moog_on(t, note, vel15); break;
    case ENG_PROPH: proph_on(t, note, vel15); break;
    case ENG_DRUM: drum_on(t, note, vel15); break;
    case ENG_TB3: tb3_on(t, note, vel15); break;
    case ENG_POL: pol_on(t, note, vel15); break;
    case ENG_VC3: v3_on(t, note, vel15); break;
    }
}
static void eng_off(int t)                                /* every voice of the track into release */
{
    switch (TK[t].eng) {
    case ENG_VAC: if (t == vac_owner) vac_note_off(); break;
    case ENG_MOOG: moog_off(t); break;
    case ENG_PROPH: proph_off(t); break;
    case ENG_TB3: tb3_off(t); break;
    case ENG_POL: pol_off(t); break;
    case ENG_VC3: v3_off(t); break;
    }
}
static int8_t sq_note[NTRK] = { -1, -1, -1, -1 };          /* the note the step sequencer sounds on a polyphonic engine */
static void arp_rel(int t)                                /* the arpeggiator's note ends: on the polyphonic Prophet only its own voice */
{
    arp_t *a = &TK[t].a;
    if (TK[t].eng == ENG_PROPH) { if (a->last >= 0) proph_off_note(t, a->last); }
    else eng_off(t);
    a->last = -1;
}

void voices_init(void)
{
    int t, i;
    fx_defaults();
    mod_defaults();
    for (t = 0; t < NTRK; t++) {
        TK[t].lvl = 32768; TK[t].a.last = -1;
        TK[t].eng = (uint8_t)(t == 0 ? ENG_VAC : t == 1 ? ENG_MOOG : t == 2 ? ENG_PROPH : ENG_DRUM);
        TK[t].vol = 26214;
        for (i = 0; i < ARP_COUNT; i++) TK[t].a.p[i] = ARP_DEF[i];
        moog_init(t);
        v3_init(t);
        pol_init(t);
        tb3_init(t);
        proph_init(t);
        drum_init(t);
    }
#ifdef C_BOOT_ARP
    voices_arp(C_BOOT_TRACK, ARP_MODE, C_BOOT_ARP);
    voices_arp(C_BOOT_TRACK, ARP_OCT, 2);
#endif
}

void voices_select(int t) { sel_t = (uint8_t)(t & 3); }
int voices_sel(void) { return sel_t; }
void voices_bpm(int b) { bpm_v = b < 20 ? 20 : b > 300 ? 300 : b; }
void voices_track_level(int t, int v) { TK[t & 3].vol = (int16_t)((v < 0 ? 0 : v > 1000 ? 1000 : v) * 32767 / 1000); }
int voices_engine(int t) { return TK[t & 3].eng; }
int voices_vac_owner(void) { return vac_owner; }
void voices_set_vac_owner(int t)
{
    t &= 3;
    if (t != vac_owner && TK[t].eng == ENG_VAC) { vac_note_off(); vac_owner = (uint8_t)t; }
}
void voices_set_engine(int t, int e)
{
    t &= 3;
    if (e == TK[t].eng) return;
    if (e == ENG_DRUM && t < DRUM_T0) return;              /* only track 4 can run the drum engine (one drum machine) */
    eng_off(t);
    if (e == ENG_VAC) {                                    /* the Vactrol moves to this track */
        eng_off(vac_owner);
        vac_owner = (uint8_t)t;
    }
    TK[t].eng = (uint8_t)e;
    mod_fix_track(t);                                      /* rebuildDst(): the destinations the new engine does not have go back to None */
}
void voices_lvl_mod(int t, int q15) { TK[t & 3].lvl = q15 < 0 ? 0 : q15 > 32768 ? 32768 : q15; }
void voices_param(int t, int eng, int id, int v)
{
    if (eng == ENG_MOOG) moog_set(t & 3, id, v);
    else if (eng == ENG_PROPH) proph_set(t & 3, id, v);
    else if (eng == ENG_DRUM) drum_set(t & 3, id, v);
    else if (eng == ENG_VC3) v3_set(t & 3, id, v);
    else if (eng == ENG_POL) pol_set(t & 3, id, v);
    else if (eng == ENG_TB3) tb3_set(t & 3, id, v);
}
int voices_param_get(int t, int eng, int id) { return eng == ENG_MOOG ? moog_get(t & 3, id) : eng == ENG_PROPH ? proph_get(t & 3, id) : eng == ENG_DRUM ? drum_get(t & 3, id) : eng == ENG_TB3 ? tb3_get(t & 3, id) : eng == ENG_POL ? pol_get(t & 3, id) : eng == ENG_VC3 ? v3_get(t & 3, id) : 0; }


/* ---- the step clock (the prototype's seq() / fire()): 16th notes at the tempo from PLAY; the drum lanes (DG) fire here ---- */
static int8_t seq_cur = -1;
static int32_t sq_off[4];                                  /* frames until the sequenced note of a track ends, 0: none */
static uint8_t seq_on;
static int32_t seq_cnt;
static uint32_t seq_st;
static struct { int8_t t, v; int16_t vel; int32_t d; } seq_q[16];       /* delayed hits (swing / humanize): hitD() */
static int seq_nq;
int voices_seq_cur(void) { return seq_on ? seq_cur : -1; }
void voices_play(int on)
{
    int t;
    seq_on = (uint8_t)(on != 0);
    seq_cnt = 0; seq_st = 0; seq_cur = -1; seq_nq = 0;
    for (t = 0; t < NTRK; t++) { sq_off[t] = 0; sq_note[t] = -1; }
    if (!on) for (t = 0; t < NTRK; t++) if (!TK[t].a.p[ARP_MODE] && !(TK[t].a.down[0] | TK[t].a.down[1] | TK[t].a.down[2] | TK[t].a.down[3])) eng_off(t);
}
static int rnd16u(void) { return c_rand15() + 32768; }       /* 0..65535 */
static void seq_hit(int t, int v, int vel, int d)
{
    if (d > 1 && seq_nq < 16) { seq_q[seq_nq].t = (int8_t)t; seq_q[seq_nq].v = (int8_t)v; seq_q[seq_nq].vel = (int16_t)vel; seq_q[seq_nq].d = d; seq_nq++; }
    else drum_hit(t, v, vel);
}
void voices_seq_rec(int t, int v)
{
    t &= 3;
    if (!seq_on || seq_cur < 0 || TK[t].eng != ENG_DRUM || v < 0 || v > 5) return;
    drum_gates_set(t, v, (uint16_t)(drum_gates(t, v) | (1u << seq_cur)));
    drum_hit(t, v, 32767);
}
/* the synth sequencer (the prototype's fire() / seq() for non-drum tracks): a gate step plays noteOn(60 + 12 * octave) and the note ends 6/bpm s later
   (40 % of the step); the pitch lines (destination Pitch semitones / major / minor) add their step value to the note, as the prototype adds it to q.pitch.
   Added to the prototype: the "Seq gate" destination scales the gate length (sq_glen). */
int ctrl_octave_val(void);
static int32_t sq_glen[4] = { 13107, 13107, 13107, 13107 };    /* gate length, Q15 of a step (.4: the prototype's 6 / bpm of the 15 / bpm step) */
void voices_sqg_mod(int t, int delta_q15) { int32_t g = 13107 + delta_q15; sq_glen[t & 3] = g < 1000 ? 1000 : g > 32768 ? 32768 : g; }
static int seq_pitch(int t, int cur)
{
    const int16_t *q = t == mod_view ? &PV[P_M4T] : MODT[t].q;
    int j, semi = 0;
    static const uint8_t SC[2][7] = { { 0, 2, 4, 5, 7, 9, 11 }, { 0, 2, 3, 5, 7, 8, 10 } };
    for (j = 0; j < 6; j++) {
        int d = q[4 * j], key = q[4 * j + 1], v = ui_seq_get_t(t, j, cur), a = q[4 * j + 2], x;
        if (d < 1 || d > 3) continue;
        if (d == 1) { x = v * a; x = (x * 24 + (x >= 0 ? 500000 : -500000)) / 1000000; semi += x + key; }
        else {
            int dg, o, i;
            x = v * a; dg = (x * 14 + (x >= 0 ? 500000 : -500000)) / 1000000;      /* Math.round(x * 14) */
            o = dg >= 0 ? dg / 7 : -((-dg + 6) / 7); i = dg - o * 7;               /* floor(d / n), ((d % n) + n) % n */
            semi += o * 12 + SC[d - 2][i] + key;
        }
    }
    return semi;
}
static void seq_synth(int t, int cur, int32_t sl)
{
    int note;
    if (!((SQG[t] >> cur) & 1u)) { sq_off[t] = 0; return; }
    note = 60 + 12 * ctrl_octave_val() + seq_pitch(t, cur);
    note = note < 0 ? 0 : note > 127 ? 127 : note;
    if (TK[t].eng == ENG_PROPH && sq_note[t] >= 0) proph_off_note(t, sq_note[t]);
    eng_on(t, note, 32767);
    sq_note[t] = (int8_t)note;
    sq_off[t] = (int32_t)(((int64_t)sl * sq_glen[t]) >> 15) + 1;
}
static void seq_fire(void)
{
    int t, v, cur = (int)(seq_st++ & 15u);
    int32_t sl = (FS * 15) / bpm_v;
    seq_cur = (int8_t)cur;
    for (t = 0; t < NTRK; t++) {
        if (TK[t].eng != ENG_DRUM) { seq_synth(t, cur, sl); continue; }
        for (v = 0; v < 6; v++) {
            int ch, hu, ac, sw, vel, d, r;
            if (!((drum_gates(t, v) >> cur) & 1u)) continue;
            ch = drum_get(t, DR_ID(v, DR_CHANCE)); hu = drum_get(t, DR_ID(v, DR_HUM));
            ac = drum_get(t, DR_ID(v, DR_ACC)); sw = drum_get(t, DR_ID(v, DR_SW));
            if (((rnd16u() * 1000) >> 16) >= ch) continue;                         /* Math.random() >= chance: the step is skipped */
            vel = 32767;
            if (cur % 4) vel = vel * (1000 - ac * 6 / 10) / 1000;                    /* accent: the off-beat steps are softer */
            r = (rnd16u() * (hu * 400 / 1000)) >> 16;                              /* 1 - hu * .4 * random */
            vel = vel * (1000 - r) / 1000;
            d = (int)((int64_t)((rnd16u() * hu) >> 16) * sl * 3 / 20000);          /* hu * random * .15 steps */
            if (cur & 1) d += (int)((int64_t)sw * sl / 2000);                        /* swing: the odd steps late by sw * .5 step */
            seq_hit(t, v, vel, d);
        }
    }
}
static void seq_tick(int n)
{
    int k;
    if (!seq_on) return;
    for (k = 0; k < seq_nq;) {
        seq_q[k].d -= n;
        if (seq_q[k].d <= 0) { drum_hit(seq_q[k].t, seq_q[k].v, seq_q[k].vel); seq_q[k] = seq_q[--seq_nq]; }
        else k++;
    }
    for (k = 0; k < NTRK; k++) if (sq_off[k] > 0 && (sq_off[k] -= n) <= 0) {
        sq_off[k] = 0;
        if (TK[k].eng == ENG_PROPH) { if (sq_note[k] >= 0) proph_off_note(k, sq_note[k]); sq_note[k] = -1; }       /* polyphonic: only the step's own voice */
        else if (!(TK[k].a.down[0] | TK[k].a.down[1] | TK[k].a.down[2] | TK[k].a.down[3])) eng_off(k);       /* noOff() unless a key is held */
    }
    seq_cnt -= n;
    if (seq_cnt <= 0) { seq_cnt += (FS * 15) / bpm_v; seq_fire(); }
}

/* ---- arpeggiator ---- */
static void varp_build(arp_t *a)
{
    uint8_t b[8], l[64], r[256];
    int n = a->nheld, i, j, o, m = 0, mode = a->p[ARP_MODE], rep = a->p[ARP_REP];
    for (i = 0; i < n; i++) b[i] = a->held[i];
    if (mode != 6)
        for (i = 1; i < n; i++)                           /* insertion sort */
            for (j = i; j > 0 && b[j - 1] > b[j]; j--) { uint8_t x = b[j]; b[j] = b[j - 1]; b[j - 1] = x; }
    for (o = 0; o < a->p[ARP_OCT]; o++)
        for (i = 0; i < n; i++) l[m++] = (uint8_t)(b[i] + 12 * o);
    /* mode shapes: 2 down, 3 up-down, 4 down-up, 7 converge, 8 diverge */
    {
        int w = 0;
        if (mode == 2) for (i = m - 1; i >= 0; i--) r[w++] = l[i];
        else if (mode == 3 || mode == 4) {
            for (i = 0; i < m; i++) r[w++] = mode == 3 ? l[i] : l[m - 1 - i];
            for (i = m - 2; i >= 1; i--) r[w++] = mode == 3 ? l[i] : l[m - 1 - i];
        } else if (mode == 7 || mode == 8) {
            int lo = 0, hi = m - 1;
            while (lo <= hi) {
                r[w++] = l[lo++];
                if (lo <= hi) r[w++] = l[hi--];
            }
            if (mode == 8)
                for (i = 0; i < w / 2; i++) { uint8_t x = r[i]; r[i] = r[w - 1 - i]; r[w - 1 - i] = x; }
        } else for (i = 0; i < m; i++) r[w++] = l[i];
        a->nlist = 0;
        for (i = 0; i < w; i++)
            for (j = 0; j < rep && a->nlist < 256; j++) a->list[a->nlist++] = r[i];
    }
}

static void arp_tick(int t, int n)
{
    arp_t *a = &TK[t].a;
    int32_t L, iv, k;
    if (!a->p[ARP_MODE] || !a->nlist) return;
    if (a->goff > 0) {
        a->goff -= n;
        if (a->goff <= 0) arp_rel(t);
    }
    a->cnt -= n;
    if (a->cnt > 0) return;
    L = (FS * 60) / (bpm_v * SPB[a->p[ARP_RATE]]);
    iv = (L * (1000 + ((a->i & 1) ? -a->p[ARP_SW] : a->p[ARP_SW]) / 2)) / 1000;
    iv = iv + (iv * ((c_rand15() * a->p[ARP_HUM]) >> 15)) / 6667;      /* (rnd - .5) * hum * .3 */
    a->cnt += iv < 16 ? 16 : iv;
    k = a->i++;
    a->cur = (uint16_t)k;
    {
        int pat = a->p[ARP_PAT], vel, note, gt, pass;
        pass = PATS[pat][k % PATN[pat]] && ((c_rand32() >> 8) % 1000u) < (uint32_t)a->p[ARP_CHANCE];
        if (!pass) { arp_rel(t); a->goff = 0; return; }
        note = a->p[ARP_MODE] == 5 ? a->list[(c_rand32() >> 8) % a->nlist] : a->list[k % a->nlist];
        vel = 32767;
        if (a->p[ARP_ACC] > 0 && k % 4 != 0) vel = vel * (1000 - a->p[ARP_ACC] * 55 / 100) / 1000;
        if (a->p[ARP_HUM]) vel -= (int32_t)(((c_rand32() >> 16) * (uint32_t)a->p[ARP_HUM] / 1000u) >> 1);
        if (TK[t].eng == ENG_PROPH && a->last >= 0) proph_off_note(t, a->last);       /* the step before ends as this one starts (its release tail overlaps) */
        eng_on(t, note + a->p[ARP_TR], vel);
        a->last = (int16_t)(note + a->p[ARP_TR]);
        gt = a->p[ARP_GATE];
        if (gt >= 980) a->goff = 0;
        else a->goff = (iv * (gt < 40 ? 40 : gt)) / 1000;
    }
}

void voices_arp(int t, int id, int v)
{
    arp_t *a = &TK[t & 3].a;
    int lim[ARP_COUNT] = {8, 4, 5, 1000, 1000, 1, 1000, 1000, 24, 4, 1000, 5}, mn[ARP_COUNT] = {0, 1, 0, 0, 0, 0, 0, 0, -24, 1, 0, 0};
    if (id < 0 || id >= ARP_COUNT) return;
    v = v < mn[id] ? mn[id] : v > lim[id] ? lim[id] : v;
    if (id == ARP_MODE && !v && a->p[ARP_MODE]) {          /* the arpeggiator goes off: release what it plays */
        eng_off(t & 3);
        a->nheld = 0; a->nlist = 0; a->goff = 0;
    }
    a->p[id] = (int16_t)v;
    if (id == ARP_MODE || id == ARP_OCT || id == ARP_REP) varp_build(a);
}
void voices_arp_state_get(int t, uint8_t *o)
{
    arp_t *a = &TK[t & 3].a;
    int k, n = (a->p[ARP_MODE] && a->p[ARP_LATCH]) ? a->nheld : 0;       /* keys really held are not state: only a latched chord is */
    o[0] = (uint8_t)n;
    for (k = 0; k < 8; k++) o[1 + k] = k < n ? a->held[k] : 0;
    if (!a->p[ARP_MODE]) { for (k = 9; k < ARP_STATE_N; k++) o[k] = 0; return; }       /* off: no position */
    o[9] = (uint8_t)a->i; o[10] = (uint8_t)(a->i >> 8); o[11] = (uint8_t)a->cur; o[12] = (uint8_t)(a->cur >> 8);
    for (k = 0; k < 4; k++) o[13 + k] = (uint8_t)((uint32_t)a->cnt >> (8 * k));
}
void voices_arp_state_set(int t, const uint8_t *b)
{
    arp_t *a = &TK[t & 3].a;
    int k, n = b[0] > 8 ? 8 : b[0];
    int32_t c = (int32_t)((uint32_t)b[13] | ((uint32_t)b[14] << 8) | ((uint32_t)b[15] << 16) | ((uint32_t)b[16] << 24));
    if (!a->p[ARP_MODE]) return;
    eng_off(t & 3);
    a->goff = 0;
    a->nheld = (uint8_t)(a->p[ARP_LATCH] ? n : 0);
    for (k = 0; k < a->nheld; k++) a->held[k] = b[1 + k] & 127;
    varp_build(a);
    a->i = (uint16_t)(b[9] | (b[10] << 8)); a->cur = (uint16_t)(b[11] | (b[12] << 8));
    a->cnt = c < 0 ? 0 : c > FS * 8 ? FS * 8 : c;
}
int voices_arp_get(int t, int id) { return TK[t & 3].a.p[id]; }
void voices_arp_pos(int t, int *cur, int *len) { *cur = TK[t & 3].a.cur; *len = TK[t & 3].a.nlist; }

/* kOnT / kOffT of the prototype */
void voices_note(int t, int note, int on)
{
    arp_t *a = &TK[t & 3].a;
    uint32_t bit = 1u << (note & 31);
    int w = (note >> 5) & 3, k;
    t &= 3;
    if (on) {
        if (!a->p[ARP_MODE]) {
            a->down[w] |= bit;
            eng_on(t, note, 32767);
            return;
        }
        if (a->p[ARP_LATCH] && !(a->down[0] | a->down[1] | a->down[2] | a->down[3])) a->nheld = 0;
        a->down[w] |= bit;
        for (k = 0; k < a->nheld && a->held[k] != note; k++) ;
        if (k == a->nheld && a->nheld < 8) a->held[a->nheld++] = (uint8_t)note;
        k = a->nlist == 0;
        varp_build(a);
        if (k) { a->i = 0; a->cnt = 0; }
    } else {
        a->down[w] &= ~bit;
        if (!a->p[ARP_MODE]) {
            if (TK[t].eng == ENG_PROPH) proph_off_note(t, note);                 /* polyphonic: this key's voice only */
            else if (!(a->down[0] | a->down[1] | a->down[2] | a->down[3])) eng_off(t);
            return;
        }
        if (!a->p[ARP_LATCH]) {
            for (k = 0; k < a->nheld && a->held[k] != note; k++) ;
            if (k < a->nheld) {
                for (; k + 1 < a->nheld; k++) a->held[k] = a->held[k + 1];
                a->nheld--;
            }
            varp_build(a);
            if (!a->nheld) { arp_rel(t); a->goff = 0; }
        }
    }
}

void voices_keys(uint32_t now)
{
    uint32_t ch = now ^ keys_prev_v;
    int i;
    for (i = 0; ch && i < 27; i++)
        if ((ch >> i) & 1u) {
            ch &= ~(1u << i);
            if ((now >> i) & 1u) {
                if (ctrl_bank_key_cb(i)) { key_trk[i] = 255; continue; }     /* HOME held / SAVE BANK: the white keys pick a bank */
                key_note[i] = (uint8_t)ui_note_of_key(i);
                key_trk[i] = sel_t;
                voices_note(sel_t, key_note[i], 1);
            } else if (key_trk[i] != 255)
                voices_note(key_trk[i], key_note[i], 0);
            else
                ctrl_key_rel_cb(i);
        }
    keys_prev_v = now;
}

int voices_arp_note(int t)
{
    arp_t *a = &TK[t & 3].a;
    if (!a->p[ARP_MODE] || !a->nlist || a->goff <= 0 || !a->i) return -1;
    return a->list[(a->i - 1) % a->nlist] + a->p[ARP_TR];
}
uint32_t voices_held_keys(int t, int base)
{
    arp_t *a = &TK[t & 3].a;
    uint32_t m = 0;
    int k;
    for (k = 0; k < a->nheld; k++)
        if (a->held[k] >= base && a->held[k] < base + 27) m |= 1u << (a->held[k] - base);
    return m;
}

uint32_t voices_gate_mask(void)
{
    uint32_t m = 0;
    int t;
    for (t = 0; t < NTRK; t++)
        if (TK[t].a.down[0] | TK[t].a.down[1] | TK[t].a.down[2] | TK[t].a.down[3] | TK[t].a.nheld) m |= 1u << t;
    return m;
}

#ifndef GOV_ECO
#define GOV_ECO 1                                     /* 0: the governor never goes to the Prophet's half rate (A/B tests) */
#endif
#ifndef NO_GOV
/* Voice governor (NOTES-M): with no effect left to bypass, the voices sounding in all are capped, drum voices (dv_cap, all drum tracks) and Prophet voices (pv_cap,
 * all Prophet tracks) alike: the class that costs most right now (voices sounding x measured ticks per voice) loses a voice first, so neither is taken before
 * the other without a reason.  A taken voice is never cut: it fades out over the next block (kill).  They come back one at a time while the load is under GOV_ADMIT. */
static int gov_nd(void)
{
    int t, v, n = 0;
    for (t = DRUM_T0; t < NTRK; t++) if (TK[t].eng == ENG_DRUM) for (v = 0; v < 6; v++) n += DRM[t - DRUM_T0].V[v].on && !DRM[t - DRUM_T0].V[v].kill;
    return n;
}
static int gov_nsus(void) { int t, n = 0; for (t = 0; t < NTRK; t++) if (TK[t].eng == ENG_PROPH) n += proph_nsuspended(t); return n; }
static int gov_np(void)
{
    int t, n = 0;
    for (t = 0; t < NTRK; t++) if (TK[t].eng == ENG_PROPH) n += proph_nvoices(t);
    return n;
}
static void gov_kill_drum(void)                       /* the quietest sounding drum voice (V->e: the peak follower); among equals the one of the track with most voices, then the oldest */
{
    dvoice_t *q = 0;
    int t, v, nt[NDRUM], qn = 0;
    for (t = 0; t < NDRUM; t++) { nt[t] = 0; for (v = 0; v < 6; v++) nt[t] += DRM[t].V[v].on && !DRM[t].V[v].kill; }
    for (t = 0; t < NDRUM; t++) if (TK[t + DRUM_T0].eng == ENG_DRUM) for (v = 0; v < 6; v++) {
        dvoice_t *V = &DRM[t].V[v];
        if (!V->on || V->kill) continue;
        if (!q || V->e < q->e || (V->e == q->e && (nt[t] > qn || (nt[t] == qn && V->t > q->t)))) { q = V; qn = nt[t]; }
    }
    if (q) { q->kill = DRUM_FADE; gov.steals++; }
}
static int gov_shed_prophet(void)                     /* one Prophet voice goes: the track that has most voices loses its quietest */
{
    int t, bt = -1, best = 0;
    for (t = 0; t < NTRK; t++) if (TK[t].eng == ENG_PROPH && proph_nvoices(t) > best) { best = proph_nvoices(t); bt = t; }
    return bt >= 0 && proph_shed_one(bt);
}
static int gov_resume_prophet(void)                   /* a held note the governor suspended comes back (the track with most suspended notes first) */
{
    int t, bt = -1, best = 0;
    for (t = 0; t < NTRK; t++) if (TK[t].eng == ENG_PROPH && proph_nsuspended(t) > best) { best = proph_nsuspended(t); bt = t; }
    return bt >= 0 && proph_resume_one(bt);
}
static void gov_voices(void)
{
    int nd = gov_nd(), np = gov_np();
    static int np_prev;
    if (GOV_ECO && np > np_prev && !pv_eco && gov_n >= GOV_SETTLE && gov_peak + (uint32_t)(np - np_prev) * pv_cost > GOV_SHED) {         /* a chord starts: the voices it adds would push the load over the line, and the 8-block mean would only notice later: half rate at once */
        pv_eco = 1; gov_mark(1, &gov_vcool, GOV_COOL / 8u); gov.voices++;
    }
    np_prev = np;
    int severe = gov_last > GOV_EMERG || gov_peak > GOV_P / 100u * 70u;               /* a real overrun is near: only then does the Prophet lose voices */
    if (gov_n >= GOV_SETTLE && ((gov_peak > GOV_SHED && !gov_wait) || gov_last > GOV_EMERG) && !(fx_run[0] | fx_run[1] | fx_run[2] | fx_run[3]) && ((GOV_ECO && !pv_eco && np > 0) || nd > GOV_DMIN || (severe && np > GOV_PMIN))) {       /* no effect left to bypass: fewer voices */
        uint32_t over = (gov_last > gov_peak ? gov_last : gov_peak) - GOV_AIM, freed = 0;
        int guard;
        for (guard = 0; guard < 24 && freed < over; guard++) {                    /* order: effects (above), then drum voices down to GOV_DMIN, and the Prophet (down to GOV_PMIN) only when severe: a chord of five keys is the instrument's point */
            if (GOV_ECO && !pv_eco && np > 0) { pv_eco = 1; freed += (uint32_t)np * pv_cost * 45u / 100u; }       /* effects first (above), then the Prophet goes to half rate (keeps all its voices) */
            else if (nd > GOV_DMIN) { nd--; dv_cap = nd < dv_cap ? nd : dv_cap; freed += dv_cost; }       /* then the drum voices, down to GOV_DMIN */
            else if (severe && np > GOV_PMIN) { np--; pv_cap = np < pv_cap ? np : pv_cap; freed += pv_cost; }
            else break;
        }
        gov_mark(1, &gov_vcool, GOV_COOL / 8u);
        gov.voices++;
        if (!gov.cap_min || (uint32_t)(dv_cap + pv_cap) < gov.cap_min) gov.cap_min = (uint32_t)(dv_cap + pv_cap);
    } else if (!gov_vcool && !(perf.blocks & 15u)) {                              /* the class that lost more comes back first */
        int fd = dv_cap * 100 / GOV_DRUMS, fp = pv_cap * 100 / GOV_PROPH;
        if (pv_eco && pv_cap >= GOV_PROPH && !gov_nsus() && gov_peak + (uint32_t)gov_np() * pv_cost * 6u / 10u <= GOV_ADMIT) pv_eco = 0;        /* quality comes back after polyphony */
        else if (fp <= fd && pv_cap < GOV_PROPH && gov_peak + pv_cost * 3u / 2u <= GOV_ADMIT) pv_cap++;
        else if (fd < fp && dv_cap < GOV_DRUMS && gov_peak + dv_cost * 3u / 2u <= GOV_ADMIT) dv_cap++;
    }
    nd = gov_nd(); np = gov_np();
    while (nd > dv_cap) { gov_kill_drum(); nd--; }
    while (np > pv_cap && gov_shed_prophet()) { gov.steals++; np--; }
    while (np < pv_cap && gov_peak + pv_cost <= GOV_ADMIT && !gov_vcool && gov_resume_prophet()) np++;          /* a suspended note comes back, one per block, while the load allows */
}
#endif
void fx_track(int t, int32_t *x, int n, int act);          /* fxdsp.c: the track's chain, in place */
int fx_awake(int t);
#ifdef LT_BURN                                  /* load test: a busy wait of LT_BURN ticks in every audio block = a machine that is slower by that much (NOTES-M) */
static void lt_burn(void) { uint32_t t0 = PERF_NOW(); while (PERF_NOW() - t0 < (uint32_t)(LT_BURN)) ; }
#endif
#ifndef NO_GOV
void hud_block(uint32_t t_in, uint32_t d)             /* end of an audio block (main.c): the debug indicator's counters */
{
    uint32_t gap = t_in - hud.last_in, dev;
    if (hud.last_in) {
        if (gap > HUD_P * 3u / 2u) hud.uf += (gap + HUD_P / 2u) / HUD_P - 1u;
        dev = gap > HUD_P ? gap - HUD_P : HUD_P - gap;
        if (dev > hud.jmx) hud.jmx = dev;
    }
    hud.last_in = t_in;
    hud.acc += d; hud.n++;
    if (d > hud.mx) hud.mx = d;
    if (hud.n >= 344u) {                                              /* one second */
        int t, q;
        hud.avg = (hud.acc / hud.n) * 1000u / HUD_P;
        hud.max = hud.mx * 1000u / HUD_P;
        if (hud.max > hud.maxall) hud.maxall = hud.max;
        hud.jit = hud.jmx * 1000u / HUD_P;
        if (hud.jit > hud.jitall) hud.jitall = hud.jit;
        hud.np = (uint8_t)gov_np(); hud.nd = (uint8_t)gov_nd();
        hud.nm = 0; hud.run = hud.off = 0;
        for (t = 0; t < NTRK; t++) {
            if (TK[t].eng == ENG_MOOG && moog_active(t)) hud.nm++;
            for (q = 0; q < 8; q++) { hud.run += (uint8_t)(fx_run[t] >> q & 1); hud.off += (uint8_t)(fx_off[t] >> q & 1); }
        }
        hud.cd = (uint8_t)dv_cap; hud.cp = (uint8_t)pv_cap; hud.eco = pv_eco;
        hud.fps = hud.nfr - hud.nfr0; hud.nfr0 = hud.nfr;
        hud.acc = hud.mx = hud.n = hud.jmx = 0;
        hud.ver++;
    }
}
#endif
/* MAXTRACKS begin: the selected track always plays, then the lowest-numbered others, up to max_tracks.  A track outside the set is not rendered at all
 * (engine and FX cost nothing); its level ramps to 0 over the first block (no click) and back up over one block when it re-enters. */
#if FEATURE_MAXTRACKS
static uint8_t mt_prev = 15;
static unsigned mt_mask(void)
{
    unsigned m = 1u << (ctrl_track() & 3); int c = 1, t;
    for (t = 0; t < NTRK && c < max_tracks; t++) if (!((m >> t) & 1)) { m |= 1u << t; c++; }
    return m;
}
#endif
/* MAXTRACKS end */
void voices_render(int32_t *out, int n)
{
#ifdef LT_BURN
    lt_burn();
#endif
    int32_t tmp[128];
    int t, i;
    int64_t acc[128];
    for (i = 0; i < n; i++) acc[i] = 0;
    bpm_v = PV[P_BPM] < 20 ? 20 : PV[P_BPM] > 300 ? 300 : PV[P_BPM];      /* the TEMPO page */
    seq_tick(n);
#ifndef NO_GOV
    gov_voices();                               /* after the step clock: this block's hits count */
#endif
#if FEATURE_MAXTRACKS /* MAXTRACKS */
    unsigned mt_cur = mt_mask(), mt_was = mt_prev; mt_prev = (uint8_t)mt_cur; hud.mt = max_tracks;
#endif
    for (t = 0; t < NTRK; t++) {
        int act = 1;
#ifndef NO_PERF
        uint32_t te0;
#endif
        arp_tick(t, n);
#if FEATURE_MAXTRACKS /* MAXTRACKS */
        if (!((mt_cur | mt_was) >> t & 1)) continue;            /* outside the set for more than a block: free */
#endif
#ifndef NO_PERF
        te0 = PERF_NOW();
#endif
        switch (TK[t].eng) {
        case ENG_VAC: if (t != vac_owner) act = 0; else vac_render(tmp, n); break;
        case ENG_MOOG: if (!moog_active(t)) act = 0; else moog_render(t, tmp, n); break;
        case ENG_PROPH: if (!proph_active(t)) act = 0; else {
#ifndef NO_GOV
                int nv = proph_nvoices(t);
                uint32_t tp = PERF_NOW();
                proph_render(t, tmp, n);
                if (nv > 0 && PERF_NOW() != 0u) { uint32_t c = (PERF_NOW() - tp) / (uint32_t)nv; pv_cost = (pv_cost * 7u + c) >> 3; }      /* what a Prophet voice costs on this device (smoothed) */
#else
                proph_render(t, tmp, n);
#endif
            } break;
        case ENG_DRUM: if (!drum_active(t)) act = 0; else {
#ifndef NO_GOV
                int nv = 0, vv;
                uint32_t td = PERF_NOW();
                for (vv = 0; vv < 6; vv++) nv += DRM[t - DRUM_T0].V[vv].on;
                drum_render(t, tmp, n);
                if (nv > 1 && PERF_NOW() != 0u) { uint32_t c = (PERF_NOW() - td) / (uint32_t)nv; dv_cost = (dv_cost * 7u + c) >> 3; }      /* what a drum voice costs on this device (smoothed) */
#else
                drum_render(t, tmp, n);
#endif
            } break;
        case ENG_TB3: if (!tb3_active(t)) act = 0; else tb3_render(t, tmp, n); break;
        case ENG_POL: if (!pol_active(t)) act = 0; else pol_render(t, tmp, n); break;
        case ENG_VC3: if (!v3_active(t)) act = 0; else v3_render(t, tmp, n); break;
        default: act = 0; break;
        }
        if (act && t == sel_t) vsc_push(tmp, n);          /* the screen's waveform: the selected track's block, as the engine made it */
        if (act && TK[t].eng != ENG_PROPH)      /* ENGINE_TRIM: every engine sits 4.4 dB lower (the Prophet's own trim is in its voice sum, ahead of its soft limit) so that a 5-note chord does not saturate */
            for (i = 0; i < n; i++) tmp[i] = (int32_t)(((int64_t)tmp[i] * ENGINE_TRIM_Q15) >> 15);
#ifndef NO_PERF
        if (perf.blocks > PERF_WARM) { uint32_t d = PERF_NOW() - te0; perf.te_sum[t] += d; if (d > perf.te_max[t]) perf.te_max[t] = d; }
#endif
        if (!act) {                             /* silent engine: only the chain's tail (delay, reverb ...) while it lasts */
            if (!fx_awake(t)) continue;
            for (i = 0; i < n; i++) tmp[i] = 0;
        }
        {                                       /* the Level destination: the gain ramps over the block from the previous block's value (a step every 128 samples is a 344 Hz zipper line) */
            int32_t g0 = lvl_prev[t], g1 = TK[t].lvl, rq = 65536 / n;
            if (act && (g0 != 32768 || g1 != 32768))
                for (i = 0; i < n; i++) tmp[i] = (int32_t)(((int64_t)tmp[i] * (g0 + (int32_t)(((int64_t)(g1 - g0) * ((i + 1) * rq)) >> 16))) >> 15);
            lvl_prev[t] = g1;
        }
#if FEATURE_MAXTRACKS /* MAXTRACKS */
        if ((mt_cur ^ mt_was) >> t & 1) {                       /* entering / leaving: a one-block ramp */
            int up = (mt_cur >> t) & 1, rq = 65536 / n;
            for (i = 0; i < n; i++) { int32_t g = (int32_t)(((i + 1) * rq) >> 1); if (!up) g = 32768 - g; tmp[i] = (int32_t)(((int64_t)tmp[i] * g) >> 15); }
        }
#endif
        fx_track(t, tmp, n, act);               /* the track's effects (fxdsp.c), then the track volume as the prototype: F(tb[i]) * t.vol */
        {                                       /* HOME's meter: this block's peak of the track */
            int32_t pk = 0;
            for (i = 0; i < n; i++) { int32_t a = tmp[i] < 0 ? -tmp[i] : tmp[i]; if (a > pk) pk = a; }
            ui_peak(t, pk > 32767 ? 32767 : pk);
        }
        {                                       /* the track volume = HOME's fader (PV[P_VOL0 + t]), read every block and ramped over it (a step would zipper) */
            int32_t v0 = TK[t].vol, v1 = PV[P_VOL0 + t], rq = 65536 / n;
            v1 = (v1 < 0 ? 0 : v1 > 1000 ? 1000 : v1) * 32767 / 1000;
            TK[t].vol = (int16_t)v1;
            if (v0 == v1) for (i = 0; i < n; i++) acc[i] += (int64_t)tmp[i] * v1;
            else for (i = 0; i < n; i++) acc[i] += (int64_t)tmp[i] * (v0 + (int32_t)(((int64_t)(v1 - v0) * ((i + 1) * rq)) >> 16));
        }
    }
    for (i = 0; i < n; i++) {
        int32_t s = (int32_t)(((acc[i] >> 15) * 34406) >> 15);            /* * 1.05 (1.5 * .7) */
        out[i] = c_tanh(s);
    }
}
