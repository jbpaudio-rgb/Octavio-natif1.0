/* SPDX-License-Identifier: MPL-2.0 */
/* The voice layer: tracks, key routing, arpeggiator, mix. See voices.h. */
#include "voices.h"
#include "params.h"
#include "ui.h"
#include "eng_moog.c"
#include "eng_prophet.c"
#include "eng_drum.c"

#define NTRK 4
typedef struct {
    int16_t p[ARP_COUNT];
    uint8_t held[8], nheld;
    uint8_t list[256];
    uint16_t nlist, i, cur;
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
static uint8_t vac_owner = 0;                              /* the Vactrol is a singleton: one track plays it */
static const uint8_t SPB[6] = {1, 2, 3, 4, 6, 8};
static const uint8_t PATS[6][8] = {{1, 1, 1, 1, 1, 1, 1, 1}, {1, 0, 0, 1, 0, 0, 1, 0}, {0, 1, 0, 1, 0, 1, 0, 1},
                                   {1, 0, 1, 1, 1, 0, 1, 1}, {1, 0, 0, 1, 0, 1, 0, 0}, {1, 0, 0, 0, 1, 0, 0, 0}};
static const uint8_t PATN[6] = {1, 8, 2, 4, 8, 4};
static const int16_t ARP_DEF[ARP_COUNT] = {0, 1, 3, 500, 0, 0, 0, 1000, 0, 1, 0, 0};

static void eng_on(int t, int note, int vel15)
{
    switch (TK[t].eng) {
    case ENG_VAC: if (t == vac_owner) vac_note_on(note); break;
    case ENG_MOOG: moog_on(t, note, vel15); break;
    case ENG_PROPH: proph_on(t, note, vel15); break;
    case ENG_DRUM: drum_on(t, note, vel15); break;
    }
}
static void eng_off(int t)
{
    switch (TK[t].eng) {
    case ENG_VAC: if (t == vac_owner) vac_note_off(); break;
    case ENG_MOOG: moog_off(t); break;
    case ENG_PROPH: proph_off(t); break;
    }
}

void voices_init(void)
{
    int t, i;
    fx_defaults();
    mod_defaults();
    for (t = 0; t < NTRK; t++) {
        TK[t].lvl = 32768;
        TK[t].eng = (uint8_t)(t == 0 ? ENG_VAC : t == 1 ? ENG_MOOG : t == 2 ? ENG_PROPH : ENG_DRUM);
        TK[t].vol = 26214;
        for (i = 0; i < ARP_COUNT; i++) TK[t].a.p[i] = ARP_DEF[i];
        moog_init(t);
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
}
int voices_param_get(int t, int eng, int id) { return eng == ENG_MOOG ? moog_get(t & 3, id) : eng == ENG_PROPH ? proph_get(t & 3, id) : eng == ENG_DRUM ? drum_get(t & 3, id) : 0; }


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
    for (t = 0; t < NTRK; t++) sq_off[t] = 0;
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
        int d = q[4 * j], key = q[4 * j + 1], v = ui_seq_get(j, cur), a = q[4 * j + 2], x;
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
    eng_on(t, note, 32767);
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
        if (!(TK[k].a.down[0] | TK[k].a.down[1] | TK[k].a.down[2] | TK[k].a.down[3])) eng_off(k);       /* noOff() unless a key is held */
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
        if (a->goff <= 0) eng_off(t);
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
        if (!pass) { eng_off(t); a->goff = 0; return; }
        note = a->p[ARP_MODE] == 5 ? a->list[(c_rand32() >> 8) % a->nlist] : a->list[k % a->nlist];
        vel = 32767;
        if (a->p[ARP_ACC] > 0 && k % 4 != 0) vel = vel * (1000 - a->p[ARP_ACC] * 55 / 100) / 1000;
        if (a->p[ARP_HUM]) vel -= (int32_t)(((c_rand32() >> 16) * (uint32_t)a->p[ARP_HUM] / 1000u) >> 1);
        eng_on(t, note + a->p[ARP_TR], vel);
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
            if (!(a->down[0] | a->down[1] | a->down[2] | a->down[3])) eng_off(t);
            return;
        }
        if (!a->p[ARP_LATCH]) {
            for (k = 0; k < a->nheld && a->held[k] != note; k++) ;
            if (k < a->nheld) {
                for (; k + 1 < a->nheld; k++) a->held[k] = a->held[k + 1];
                a->nheld--;
            }
            varp_build(a);
            if (!a->nheld) { eng_off(t); a->goff = 0; }
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

void fx_track(int t, int32_t *x, int n, int act);          /* fxdsp.c: the track's chain, in place */
int fx_awake(int t);
void voices_render(int32_t *out, int n)
{
    int32_t tmp[128];
    int t, i;
    int64_t acc[128];
    for (i = 0; i < n; i++) acc[i] = 0;
    bpm_v = PV[P_BPM] < 20 ? 20 : PV[P_BPM] > 300 ? 300 : PV[P_BPM];      /* the TEMPO page */
    seq_tick(n);
    for (t = 0; t < NTRK; t++) {
        int act = 1;
        arp_tick(t, n);
        switch (TK[t].eng) {
        case ENG_VAC: if (t != vac_owner) act = 0; else vac_render(tmp, n); break;
        case ENG_MOOG: if (!moog_active(t)) act = 0; else moog_render(t, tmp, n); break;
        case ENG_PROPH: if (!proph_active(t)) act = 0; else proph_render(t, tmp, n); break;
        case ENG_DRUM: if (!drum_active(t)) act = 0; else drum_render(t, tmp, n); break;
        default: act = 0; break;
        }
        if (!act) {                             /* silent engine: only the chain's tail (delay, reverb ...) while it lasts */
            if (!fx_awake(t)) continue;
            for (i = 0; i < n; i++) tmp[i] = 0;
        }
        if (act && TK[t].lvl != 32768) for (i = 0; i < n; i++) tmp[i] = (int32_t)(((int64_t)tmp[i] * TK[t].lvl) >> 15);      /* the Level destination */
        fx_track(t, tmp, n, act);               /* the track's effects (fxdsp.c), then the track volume as the prototype: F(tb[i]) * t.vol */
        {                                       /* HOME's meter: this block's peak of the track */
            int32_t pk = 0;
            for (i = 0; i < n; i++) { int32_t a = tmp[i] < 0 ? -tmp[i] : tmp[i]; if (a > pk) pk = a; }
            ui_peak(t, pk > 32767 ? 32767 : pk);
        }
        for (i = 0; i < n; i++) acc[i] += (int64_t)tmp[i] * TK[t].vol;
    }
    for (i = 0; i < n; i++) {
        int32_t s = (int32_t)(((acc[i] >> 15) * 34406) >> 15);            /* * 1.05 (1.5 * .7) */
        out[i] = c_tanh(s);
    }
}
