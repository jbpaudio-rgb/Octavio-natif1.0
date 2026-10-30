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

static struct { uint8_t eng; int16_t vol; arp_t a; } TK[NTRK];       /* vol: Q15 */
#ifndef C_BOOT_TRACK
#define C_BOOT_TRACK 0
#endif
static uint8_t sel_t = C_BOOT_TRACK;
static uint32_t keys_prev_v;
static uint8_t key_note[27], key_trk[27];
static int ctrl_bank_key_cb(int key);                  /* main.c: a key pressed while the bank keys are active */
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
    for (t = 0; t < NTRK; t++) {
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
}
void voices_param(int t, int eng, int id, int v)
{
    if (eng == ENG_MOOG) moog_set(t & 3, id, v);
    else if (eng == ENG_PROPH) proph_set(t & 3, id, v);
    else if (eng == ENG_DRUM) drum_set(t & 3, id, v);
}
int voices_param_get(int t, int eng, int id) { return eng == ENG_MOOG ? moog_get(t & 3, id) : eng == ENG_PROPH ? proph_get(t & 3, id) : eng == ENG_DRUM ? drum_get(t & 3, id) : 0; }

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

void voices_render(int32_t *out, int n)
{
    int32_t tmp[128];
    int t, i;
    int64_t acc[128];
    for (i = 0; i < n; i++) acc[i] = 0;
    for (t = 0; t < NTRK; t++) {
        arp_tick(t, n);
        switch (TK[t].eng) {
        case ENG_VAC: if (t != vac_owner) continue; vac_render(tmp, n); break;
        case ENG_MOOG: if (!moog_active(t)) continue; moog_render(t, tmp, n); break;
        case ENG_PROPH: if (!proph_active(t)) continue; proph_render(t, tmp, n); break;
        case ENG_DRUM: if (!drum_active(t)) continue; drum_render(t, tmp, n); break;
        default: continue;
        }
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
