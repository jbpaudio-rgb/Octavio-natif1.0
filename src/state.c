/* SPDX-License-Identifier: MPL-2.0
 * state.c - the machine state as a versioned byte record: what a preset / a key scene (bank) stores.
 * Included from ctrl.c (single translation unit, after voices.c: it reads the engines' tables).
 *
 * A record body is a list of SECTIONS, each   id u8 | rev u8 | len u16 | data[len]   (little endian):
 *   0x01 CORE   PV[P_FOLD..P_MODE]                 the Vactrol's controls              (14 values)
 *   0x02 MOD    PV[P_M0T..P_M11Y]                  LFO 1..4 / S&H 1..2 and their destinations
 *   0x03 FX     PV[P_F0DRIVE..P_F5SYNC]            the effect knobs, raw    rev = FXP_REV: bump it when the FX layout changes
 *   0x04 ARP    PV[P_ARPMODE..P_ARPPAT]            the arpeggiator page
 *   0x05 SEQ    PV[P_M4T..P_STEPV]                 sequencer lines: destination / key / amount, step value
 *   0x06 STEPS  the 6 x 16 sequencer step values   (ui_seq_get / ui_seq_set)
 *   0x07 MIX    PV[P_VOL0..P_VOL3]                 the track volumes (key scenes only, like the prototype's q.vol)
 *   0x08 TRK    4 engine ids (one per track) + the track that owns the (single) Vactrol          raw bytes
 *   0x09 TARP   the 4 tracks' arpeggiators (voices_arp: 4 x ARP_COUNT)
 *   0x0A ARPS   (added later, rev 1) each track's arpeggiator position: latched chord in order, step, phase (4 x ARP_STATE_N raw bytes)
 *   0x10 + 3 * track + (engine - 1)   ENGINE  the Minimoog / Prophet / drum parameters of that track (12 sections)
 *   0x40..0x4F  per-track effects (agent H: 0x40 FXB)
 *   0x41 MODT   (rev 1) the modulation matrix of each track: LFO / S&H (dest, shape, rate, amount, sync) and sequencer lines, 4 x 54 values.
 *               A destination is an index of the master list (dst.c); 0x02 / 0x05 stay the selected track's view.
 *   0x42 DSQ    (rev 1) the drum sequencer of each drum track: 6 lanes of 16 step bits, then swing / humanize / chance / accent of the 6 voices
 * Value sections: enc u8 | nvals u8 | body. enc 0: nvals x int16 raw. enc 1: sparse, (index u8, int16) pairs for the values that
 * differ from the factory value, every other value is the factory value. The writer picks the shorter. A section whose rev or
 * nvals is not the one this firmware has is skipped (the layout it describes is gone), a section that is missing leaves
 * its values alone. The Vactrol engine is PV (CORE), the three other engines keep their parameters per track, all 12
 * (track, engine) pairs are saved, not only the engine a track runs now.
 * Not in a record: tempo, MIDI channels, MIDI routing (global settings, the prototype does not store them either). */
#include "voices.h"
#include "params.h"

#define FXP_REV 1                                   /* the layout of the FX section (PV[P_F0DRIVE..P_F5SYNC]) */
#define ST_DR_X 11                                  /* drum controls saved per voice: DR_TYPE..DR_FILT (the lane settings DR_SW.. are in the DSQ section) */
#define ST_DRUM_N (6 * ST_DR_X)                     /* the drum voices' parameters without the DR_KIT slot (a command, not a value) */
#define ST_SQ_N (6 * 16)
#define ST_TARP_N (4 * ARP_COUNT)
#define ST_NSEC 25
#define ST_MAX 3200u                                /* the largest body state_save() can produce (checked by tests) */

enum { SK_PV, SK_ENG, SK_SQ, SK_TARP, SK_TRK, SK_ARPS, SK_FXB, SK_MODT, SK_DSQ };
#define ST_MODT_N (4 * MODT_N)
#define ST_DSQ_N (4 * 30)
static int st_modt_seen;                              /* a MODT section was applied by this state_load */
uint32_t fxbank_save(uint8_t *b); int fxbank_load(const uint8_t *b, uint32_t n);   /* fxstate.c: the 4 tracks' effect chains (agent H) */
#define ST_FXB_N 444u                               /* FXBANK_BYTES */
typedef struct { uint8_t id, rev, kind, n, t, e, scene; int16_t a; } secd_t;
typedef char st_fit_n[(P_STEPV - P_M4T + 1 < 256 && P_F5SYNC - P_F0DRIVE + 1 < 256 && ST_TARP_N < 256 && ST_SQ_N < 256 && ST_DRUM_N < 256) ? 1 : -1];

static const struct { uint8_t id, rev, scene; int16_t a, z; } PVR[6] = {
    { 0x01, 1, 0, P_FOLD, P_MODE }, { 0x02, 1, 0, P_M0T, P_M11Y }, { 0x03, FXP_REV, 0, P_F0DRIVE, P_F5SYNC },
    { 0x04, 1, 0, P_ARPMODE, P_ARPPAT }, { 0x05, 1, 0, P_M4T, P_STEPV }, { 0x07, 1, 1, P_VOL0, P_VOL3 } };

static const int16_t *st_pv;                        /* encode from this PV image instead of the live one (legacy records) */
static int16_t st_def_mg[MG_COUNT], st_def_pr[PR_COUNT], st_def_dr[ST_DRUM_N], st_def_sq[ST_SQ_N];
static uint8_t st_ready;

static int st_drum_id(int i) { return DR_ID(i / ST_DR_X, 1 + i % ST_DR_X); }
static int st_dsq_id(int r) { return DR_ID((r - 6) / 4, DR_SW + (r - 6) % 4); }

/* the factory values of the engines and of the sequencer, read once at boot (ctrl_init: everything is freshly initialised) */
static void state_init(void)
{
    int i;
    if (st_ready) return;
    for (i = 0; i < MG_COUNT; i++) st_def_mg[i] = (int16_t)voices_param_get(0, ENG_MOOG, i);
    for (i = 0; i < PR_COUNT; i++) st_def_pr[i] = (int16_t)voices_param_get(0, ENG_PROPH, i);
    for (i = 0; i < ST_DRUM_N; i++) st_def_dr[i] = (int16_t)voices_param_get(0, ENG_DRUM, st_drum_id(i));
    for (i = 0; i < ST_SQ_N; i++) st_def_sq[i] = (int16_t)ui_seq_get(i / 16, i % 16);
    st_ready = 1;
}

static int st_sec(int i, secd_t *s)                 /* the i-th section this firmware knows, 0 = none */
{
    if (i < 0 || i >= ST_NSEC) return 0;
    s->scene = 0; s->t = s->e = 0; s->a = 0;
    if (i == 24) { s->id = 0x42; s->rev = 1; s->kind = SK_DSQ; s->n = ST_DSQ_N; }
    else if (i == 23) { s->id = 0x41; s->rev = 1; s->kind = SK_MODT; s->n = ST_MODT_N; }
    else if (i == 22) { s->id = 0x40; s->rev = 1; s->kind = SK_FXB; s->n = 0; }
    else if (i == 21) { s->id = 0x0A; s->rev = 1; s->kind = SK_ARPS; s->n = 0; }
    else if (i == 0) { s->id = 0x08; s->rev = 1; s->kind = SK_TRK; s->n = 5; }
    else if (i <= 6) {
        s->id = PVR[i - 1].id; s->rev = PVR[i - 1].rev; s->scene = PVR[i - 1].scene; s->kind = SK_PV;
        s->a = PVR[i - 1].a; s->n = (uint8_t)(PVR[i - 1].z - PVR[i - 1].a + 1);
    } else if (i == 7) { s->id = 0x06; s->rev = 1; s->kind = SK_SQ; s->n = ST_SQ_N; }
    else if (i == 8) { s->id = 0x09; s->rev = 1; s->kind = SK_TARP; s->n = ST_TARP_N; }
    else {
        int k = i - 9;
        s->t = (uint8_t)(k / 3); s->e = (uint8_t)(ENG_MOOG + k % 3);
        s->id = (uint8_t)(0x10 + k); s->rev = 1; s->kind = SK_ENG;
        s->n = (uint8_t)(s->e == ENG_MOOG ? MG_COUNT : s->e == ENG_PROPH ? PR_COUNT : ST_DRUM_N);
    }
    return 1;
}
static int st_find(int id, secd_t *s)
{
    int i;
    for (i = 0; st_sec(i, s); i++) if (s->id == id) return 1;
    return 0;
}

static int st_get(const secd_t *s, int i)
{
    switch (s->kind) {
    case SK_PV: return (st_pv ? st_pv : PV)[s->a + i];
    case SK_ENG: return voices_param_get(s->t, s->e, s->e == ENG_DRUM ? st_drum_id(i) : i);
    case SK_SQ: return ui_seq_get(i / 16, i % 16);
    case SK_MODT: return modt_get(i);
    case SK_DSQ: { int t = i / 30, r = i % 30; return r < 6 ? (int16_t)drum_gates(t, r) : voices_param_get(t, ENG_DRUM, st_dsq_id(r)); }
    default: return voices_arp_get(i / ARP_COUNT, i % ARP_COUNT);
    }
}
static int st_def(const secd_t *s, int i)
{
    switch (s->kind) {
    case SK_PV: return PD[s->a + i].def;
    case SK_ENG: return s->e == ENG_MOOG ? st_def_mg[i] : s->e == ENG_PROPH ? st_def_pr[i] : st_def_dr[i];
    case SK_SQ: return st_def_sq[i];
    case SK_MODT: return modt_def(i);
    case SK_DSQ: { int r = i % 30; return r < 6 ? 0 : ((r - 6) % 4 == DR_CHANCE - DR_SW) ? 1000 : 0; }
    default: return ARP_DEF[i % ARP_COUNT];
    }
}
static void st_put(const secd_t *s, int i, int v)
{
    switch (s->kind) {
    case SK_PV: {
        const pdesc_t *d = &PD[s->a + i];
        v = v < d->min ? d->min : v > d->max ? d->max : v;
        PV[s->a + i] = (int16_t)v;
        if (s->id == 0x07) voices_track_level(i, v);          /* the scene's track volumes reach the mix */
        break;
    }
    case SK_ENG:
        if (s->e == ENG_DRUM) {                      /* kit 0..2, voice type 0..7, the rest 0..1000 */
            int x = 1 + i % ST_DR_X, mx = x == DR_KV ? 2 : x == DR_TYPE ? 7 : 1000;
            v = v < 0 ? 0 : v > mx ? mx : v;
            voices_param(s->t, s->e, st_drum_id(i), v);
        } else voices_param(s->t, s->e, i, v < 0 ? 0 : v);      /* the engines clamp to their maxima */
        break;
    case SK_SQ: ui_seq_set(i / 16, i % 16, v < -1000 ? -1000 : v > 1000 ? 1000 : v); break;
    case SK_MODT: modt_put(i, v); st_modt_seen = 1; break;
    case SK_DSQ: { int t = i / 30, r = i % 30; if (r < 6) drum_gates_set(t, r, (uint16_t)v); else voices_param(t, ENG_DRUM, st_dsq_id(r), v < 0 ? 0 : v > 1000 ? 1000 : v); break; }
    default: voices_arp(i / ARP_COUNT, i % ARP_COUNT, v); break;
    }
}

/* ---- TRK: engine per track + Vactrol owner ---- */
static void st_trk_apply(const uint8_t *b)
{
    int t;
    vac_note_off();                                 /* the Vactrol may change track: let go of its note */
    for (t = 0; t < 4; t++) {
        int e = b[t] < ENG_N ? b[t] : t;
        voices_set_engine(t, e);
        PV[P_TYP0 + t] = (int16_t)e;                /* HOME shows it */
    }
    voices_set_vac_owner(b[4]);
}

/* ---- encode ---- */
static uint32_t st_enc(uint8_t *o, uint32_t cap, const secd_t *s)     /* bytes written, 0 = no room */
{
    uint32_t len, w;
    int i, n = s->n, d = 0;
    if (s->kind == SK_FXB) {
        uint8_t b[ST_FXB_N]; uint32_t n = fxbank_save(b);
        if (n != ST_FXB_N || cap < 4u + n) return 0;
        o[0] = s->id; o[1] = s->rev; o[2] = (uint8_t)n; o[3] = (uint8_t)(n >> 8);
        memcpy(o + 4, b, n);
        return 4u + n;
    }
    if (s->kind == SK_ARPS) {
        if (cap < 4u + 4u * ARP_STATE_N) return 0;
        o[0] = s->id; o[1] = s->rev; o[2] = (uint8_t)(4 * ARP_STATE_N); o[3] = 0;
        for (i = 0; i < 4; i++) voices_arp_state_get(i, o + 4 + i * ARP_STATE_N);
        return 4u + 4u * ARP_STATE_N;
    }
    if (s->kind == SK_TRK) {
        if (cap < 9) return 0;
        o[0] = s->id; o[1] = s->rev; o[2] = 5; o[3] = 0;
        for (i = 0; i < 4; i++) o[4 + i] = (uint8_t)voices_engine(i);
        o[8] = (uint8_t)voices_vac_owner();
        return 9;
    }
    for (i = 0; i < n; i++) if (st_get(s, i) != st_def(s, i)) d++;
    {
        int sparse = 3 * d < 2 * n;
        len = 2u + (sparse ? 3u * (uint32_t)d : 2u * (uint32_t)n);
        if (cap < 4u + len) return 0;
        o[0] = s->id; o[1] = s->rev; o[2] = (uint8_t)len; o[3] = (uint8_t)(len >> 8);
        o[4] = (uint8_t)sparse; o[5] = (uint8_t)n;
        w = 6;
        for (i = 0; i < n; i++) {
            int v = st_get(s, i);
            if (sparse) {
                if (v == st_def(s, i)) continue;
                o[w++] = (uint8_t)i;
            }
            o[w++] = (uint8_t)v; o[w++] = (uint8_t)((unsigned)v >> 8);
        }
    }
    return w;
}

/* The body of a preset / scene (what is live now). scene: also the track volumes. 0 = it does not fit in cap. */
static uint32_t state_save(uint8_t *o, uint32_t cap, int scene)
{
    secd_t s;
    uint32_t w = 0, k;
    int i;
    state_init();
    mod_commit();                                   /* the selected track's matrix (PV) into MODT before it is written */
    for (i = 0; st_sec(i, &s); i++) {
        if (s.scene && !scene) continue;
        if (s.kind == SK_ARPS && !(voices_arp_get(0, ARP_MODE) | voices_arp_get(1, ARP_MODE) | voices_arp_get(2, ARP_MODE) | voices_arp_get(3, ARP_MODE))) continue;   /* no arpeggiator on: no position to keep */
        k = st_enc(o + w, cap - w, &s);
        if (!k) return 0;
        w += k;
    }
    return w;
}

/* A body made of the PV image of a legacy (Vactrol only) record: the PV sections, no engine, no track. */
static uint32_t state_save_pv(uint8_t *o, uint32_t cap, const int16_t *pv, int scene)
{
    secd_t s;
    uint32_t w = 0, k;
    int i;
    state_init();
    st_pv = pv;
    for (i = 1; i <= 6 && st_sec(i, &s); i++) {
        if (s.scene && !scene) continue;
        k = st_enc(o + w, cap - w, &s);
        if (!k) { w = 0; break; }
        w += k;
    }
    st_pv = 0;
    return w;
}

/* ---- decode ---- */
static int st_dec(const secd_t *s, const uint8_t *d, uint32_t len)    /* 1 = applied */
{
    int i, n = s->n;
    if (s->kind == SK_FXB) return fxbank_load(d, len);
    if (s->kind == SK_ARPS) {
        if (len != 4u * ARP_STATE_N) return 0;
        for (i = 0; i < 4; i++) voices_arp_state_set(i, d + i * ARP_STATE_N);
        return 1;
    }
    if (s->kind == SK_TRK) {
        if (len < 5) return 0;
        st_trk_apply(d);
        return 1;
    }
    if (len < 2 || d[1] != n || d[0] > 1) return 0;
    if (!d[0]) {
        if (len != 2u + 2u * (uint32_t)n) return 0;
        for (i = 0; i < n; i++) st_put(s, i, (int16_t)(d[2 + 2 * i] | (d[3 + 2 * i] << 8)));
    } else {
        uint32_t p;
        if ((len - 2) % 3) return 0;
        for (p = 2; p < len; p += 3) if (d[p] >= n) return 0;          /* nothing is applied from a damaged section */
        for (i = 0; i < n; i++) st_put(s, i, st_def(s, i));
        for (p = 2; p < len; p += 3) st_put(s, d[p], (int16_t)(d[p + 1] | (d[p + 2] << 8)));
    }
    return 1;
}

/* Apply a body; the number of sections applied (the engines of the tracks first: the arpeggiators act on them). */
static int state_load(const uint8_t *p, uint32_t n)
{
    uint32_t o;
    int pass, cnt = 0;
    state_init();
    st_modt_seen = 0;
    for (pass = 0; pass < 2; pass++)
        for (o = 0; o + 4 <= n;) {
            uint32_t len = (uint32_t)(p[o + 2] | (p[o + 3] << 8));
            secd_t s;
            if (o + 4 + len > n) break;
            if (st_find(p[o], &s) && (s.kind == SK_TRK) == (pass == 0) && p[o + 1] == s.rev)
                cnt += st_dec(&s, p + o + 4, len);
            o += 4 + len;
        }
    mod_post_load(st_modt_seen);                    /* the matrices follow the engines of the tracks (a destination the engine does not have: None) */
    return cnt;
}

/* The factory state of everything a record holds (a key scene never saved: "DEFAULT"). */
static void state_defaults(void)
{
    secd_t s;
    uint8_t trk[5] = { ENG_VAC, ENG_MOOG, ENG_PROPH, ENG_DRUM, 0 };
    int i, j;
    state_init();
    st_trk_apply(trk);
    st_modt_seen = 0;
    for (i = 1; st_sec(i, &s); i++)
        for (j = 0; j < s.n; j++) st_put(&s, j, st_def(&s, j));
    mod_post_load(st_modt_seen);
}
