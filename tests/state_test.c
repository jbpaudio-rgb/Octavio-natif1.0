/* SPDX-License-Identifier: MPL-2.0
 * Host test of the preset / scene records (src/state.c, ctrl.c bank + scenes, persist.c) against an in-memory NOR flash.
 *   gcc -O1 -w -Isrc -Ibsp/chip -o /tmp/state_test tests/state_test.c && /tmp/state_test
 * Round trip of every engine: random state -> SAVE (name editor) / scene save -> scramble -> load -> compare every value;
 * "reboot" (pools wiped, rebuilt from the flash image only); old-format records; power cut at every flash operation. */
#define PERSIST_FLASH 1
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "../src/octavio.h"
#include "../src/params.h"
#include "../src/ui.h"
#include "../src/mathfx.c"
#include "../src/tables_gen.c"
#include "../src/assets_gen.c"
#include "../src/gfx.c"
#include "../src/params.c"
#include "../src/fxstate.c"
void fx_track(int t, int32_t *x, int n, int act) { (void)t; (void)x; (void)n; (void)act; }
int fx_awake(int t) { (void)t; return 0; }
#include "../src/vactrol.c"
#include "../src/dst.c"
#include "../src/voices.c"
#include "../src/ctrl.c"
#include "../src/overlay.c"
#include "../src/ui.c"
/* the LED layer on a flat fake key matrix: key id i -> column i, row 1 */
#define FM1_NCOL 41u
static int8_t FM1_KEYMAP[6][FM1_NCOL];
static struct { uint32_t notes, buttons; } fm1_in;
static uint8_t fm1_led[FM1_NCOL], fm1_led_dim[FM1_NCOL], fm1_led_breath[FM1_NCOL];
static int8_t km_init(void) { int p; for (p = 0; p < (int)FM1_NCOL; p++) { FM1_KEYMAP[0][p] = -1; FM1_KEYMAP[1][p] = (int8_t)p; FM1_KEYMAP[2][p] = FM1_KEYMAP[3][p] = FM1_KEYMAP[4][p] = -1; } return 0; }
#include "../src/leds.c"
void ui_params_changed(void) {}
static int ctrl_bank_key_cb(int key) { (void)key; return 0; }
static void ctrl_key_rel_cb(int key) { (void)key; }
void fx_bus(int32_t *x, int n) { (void)x; (void)n; }

/* ---- in-memory NOR flash, 1 MiB; programming can only clear bits; the 0xF00..0xFFF of the sectors in use stay untouched ---- */
static uint8_t FL[1 << 20];
static uint8_t flash_ok = 1;
static int fl_ops, fl_fail_at = -1, fl_bad;
static uint32_t irq_save(void) { return 0; }
static void irq_restore(uint32_t f) { (void)f; }
#define FL_FAR(fn) (fn)
static int in_range(uint32_t off, uint32_t n)
{
    return (off >= 0x93000u && off + n <= 0x97000u) || (off >= 0xE7000u && off + n <= 0xFC000u);
}
static int fl_read_ram(uint32_t off, uint8_t *dst, uint32_t n) { if (off + n > sizeof FL) return -1; memcpy(dst, FL + off, n); return 0; }
static int fl_erase4k_ram(uint32_t off, uint32_t *t)
{
    *t = 0;
    if (fl_fail_at >= 0 && fl_ops++ >= fl_fail_at) return -9;
    if ((off & 0xFFF) || !in_range(off, 0x1000)) { fl_bad++; return -1; }
    memset(FL + off, 0xFF, 0x1000);
    return 0;
}
static int fl_prog_ram(uint32_t off, const uint8_t *src, uint32_t n, uint32_t *t)
{
    uint32_t i;
    *t = 0;
    if (fl_fail_at >= 0 && fl_ops++ >= fl_fail_at) return -9;
    if (!in_range(off, n) || (off & 0xFFF) + n > 0xF00u || n > 256) { fl_bad++; return -1; }   /* never across a piece, never in 0xF00.. */
    for (i = 0; i < n; i++) { if (FL[off + i] != 0xFF && FL[off + i] != src[i]) fl_bad++; FL[off + i] &= src[i]; }
    return 0;
}
#include "../src/persist.c"

/* ---- random ---- */
static uint32_t rs = 12345;
static uint32_t rnd(void) { rs = rs * 1664525u + 1013904223u; return rs >> 8; }
static int rr(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }

static int fails, quiet;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* the whole state through the public getters */
typedef struct {
    int16_t pv[P_COUNT];
    int16_t mg[4][MG_COUNT], pr[4][PR_COUNT], dr[4][DR_COUNT];
    int16_t arp[4][ARP_COUNT], sq[4][96];
    int16_t mt[4][MODT_N], dg[4][6];                 /* the matrix of each track (dst.c), the drum sequencer lanes */
    uint8_t eng[4], owner, arps[4][ARP_STATE_N];
} snap_t;
static void take(snap_t *s)
{
    int t, i;
    memcpy(s->pv, PV, sizeof PV);
    mod_commit();
    for (t = 0; t < 4; t++) {
        memcpy(s->mt[t], MODT[t].m, sizeof MODT[0].m); memcpy(s->mt[t] + MODM_N, MODT[t].q, sizeof MODT[0].q); s->mt[t][MODM_N + MODQ_N] = (int16_t)SQG[t];
        for (i = 0; i < 6; i++) s->dg[t][i] = (int16_t)drum_gates(t, i);
        for (i = 0; i < MG_COUNT; i++) s->mg[t][i] = (int16_t)voices_param_get(t, ENG_MOOG, i);
        for (i = 0; i < PR_COUNT; i++) s->pr[t][i] = (int16_t)voices_param_get(t, ENG_PROPH, i);
        for (i = 0; i < DR_COUNT; i++) s->dr[t][i] = (int16_t)voices_param_get(t, ENG_DRUM, i);
        for (i = 0; i < ARP_COUNT; i++) s->arp[t][i] = (int16_t)voices_arp_get(t, i);
        s->eng[t] = (uint8_t)voices_engine(t);
        voices_arp_state_get(t, s->arps[t]);
    }
    for (t = 0; t < 4; t++) for (i = 0; i < 96; i++) s->sq[t][i] = (int16_t)ui_seq_get_t(t, i / 16, i % 16);
    s->owner = (uint8_t)voices_vac_owner();
}
/* compare what a record holds; scene: also the volumes. Returns the number of differing values. */
static int diff(const snap_t *a, const snap_t *b, int scene, const char *what)
{
    int vb = !quiet;
    int t, i, bad = 0;
    int in_rec[P_COUNT];
    memset(in_rec, 0, sizeof in_rec);
    for (i = 0; i < 6; i++) { int j; if (PVR[i].scene && !scene) continue; for (j = PVR[i].a; j <= PVR[i].z; j++) in_rec[j] = 1; }
    for (i = 0; i < P_COUNT; i++) if (in_rec[i] && a->pv[i] != b->pv[i]) { bad++; if (bad < 6) if (vb) printf("  %s: PV[%d] %d != %d\n", what, i, a->pv[i], b->pv[i]); }
    for (t = 0; t < 4; t++) {
        for (i = 0; i < MG_COUNT; i++) if (a->mg[t][i] != b->mg[t][i]) { bad++; if (bad < 6) if (vb) printf("  %s: moog t%d %d: %d != %d\n", what, t, i, a->mg[t][i], b->mg[t][i]); }
        for (i = 0; i < PR_COUNT; i++) if (a->pr[t][i] != b->pr[t][i]) { bad++; if (bad < 6) if (vb) printf("  %s: prophet t%d %d: %d != %d\n", what, t, i, a->pr[t][i], b->pr[t][i]); }
        for (i = 0; i < DR_COUNT; i++) if (i % DR_PER != DR_KIT && a->dr[t][i] != b->dr[t][i]) { bad++; if (bad < 6) if (vb) printf("  %s: drum t%d %d: %d != %d\n", what, t, i, a->dr[t][i], b->dr[t][i]); }
        for (i = 0; i < ARP_COUNT; i++) if (a->arp[t][i] != b->arp[t][i]) { bad++; if (bad < 6) if (vb) printf("  %s: arp t%d %d: %d != %d\n", what, t, i, a->arp[t][i], b->arp[t][i]); }
        for (i = 0; i < MODT_N; i++) if (a->mt[t][i] != b->mt[t][i]) { bad++; if (bad < 6) if (vb) printf("  %s: matrix t%d %d: %d != %d\n", what, t, i, a->mt[t][i], b->mt[t][i]); }
        for (i = 0; i < 6; i++) if (a->dg[t][i] != b->dg[t][i]) { bad++; if (bad < 6) if (vb) printf("  %s: drum lane t%d v%d: %04x != %04x\n", what, t, i, a->dg[t][i] & 0xffff, b->dg[t][i] & 0xffff); }
        if (memcmp(a->arps[t], b->arps[t], ARP_STATE_N)) { bad++; if (vb) printf("  %s: arp position of track %d\n", what, t); }
        if (a->eng[t] != b->eng[t]) { bad++; if (vb) printf("  %s: engine of track %d: %d != %d\n", what, t, a->eng[t], b->eng[t]); }
        if (a->pv[P_TYP0 + t] != b->pv[P_TYP0 + t]) { bad++; if (vb) printf("  %s: PV[P_TYP%d]\n", what, t); }
    }
    for (t = 0; t < 4; t++) for (i = 0; i < 96; i++) if (a->sq[t][i] != b->sq[t][i]) { bad++; if (bad < 6) if (vb) printf("  %s: step t%d %d: %d != %d\n", what, t, i, a->sq[t][i], b->sq[t][i]); }
    for (t = 0, i = 0; t < 4; t++) i |= a->eng[t] == ENG_VAC;          /* the owner means something only while a track runs the Vactrol */
    if (i && a->owner != b->owner) { bad++; if (vb) printf("  %s: Vactrol owner %d != %d\n", what, a->owner, b->owner); }
    return bad;
}

/* a random state: `density` per cent of the values are changed from their current value */
static void randomize(int density, int seed_engines)
{
    int t, i;
    if (seed_engines) {
        int e[4] = { rr(0, 3), rr(0, 3), rr(0, 3), rr(0, 3) };
        int pick = rr(0, 3), a, b;
        for (a = 0; a < 4; a++) for (b = a + 1; b < 4; b++) if (e[a] == ENG_VAC && e[b] == ENG_VAC) e[b] = ENG_PROPH;   /* one Vactrol at most */
        e[pick] = e[pick];
        for (t = 0; t < 3; t++) if (e[t] == ENG_DRUM) e[t] = ENG_MOOG;      /* only track 4 can be drums (M) */
        for (t = 0; t < 4; t++) { voices_set_engine(t, e[t]); PV[P_TYP0 + t] = (int16_t)e[t]; }
        for (t = 0; t < 4; t++) if (e[t] == ENG_VAC) voices_set_vac_owner(t);
    }
    for (i = 0; i < P_COUNT; i++) {
        const pdesc_t *d = &PD[i];
        if (i >= P_TYP0 && i <= P_TYP3) continue;
        if ((int)(rnd() % 100u) < density) PV[i] = (int16_t)(dst_kind_of(i) ? dst_at(voices_engine(voices_sel()), dst_kind_of(i) == 2, rr(0, dst_count(voices_engine(voices_sel()), dst_kind_of(i) == 2) - 1)) : rr(d->min, d->max));
    }
    for (t = 0; t < 4; t++) {                  /* the other tracks' matrices: destinations of their own engine; the drum lanes */
        for (i = 0; i < MODT_N; i++) {
            int sq = i >= MODM_N, r = sq ? i - MODM_N : i, isdst = sq ? (r % 4 == 0) : (r % 5 == 0);
            const pdesc_t *d = &PD[sq ? P_M4T + r : P_M0T + r];
            if (i == MODM_N + MODQ_N) { if ((int)(rnd() % 100u) < density) SQG[t] = (uint16_t)rr(0, 65535); continue; }
            if (t == mod_view || (int)(rnd() % 100u) >= density) continue;
            if (sq) MODT[t].q[r] = (int16_t)(isdst ? dst_at(voices_engine(t), 1, rr(0, dst_count(voices_engine(t), 1) - 1)) : rr(d->min, d->max));
            else MODT[t].m[r] = (int16_t)(isdst ? dst_at(voices_engine(t), 0, rr(0, dst_count(voices_engine(t), 0) - 1)) : rr(d->min, d->max));
        }
        for (i = 0; i < 6; i++) if ((int)(rnd() % 100u) < density) drum_gates_set(t, i, (uint16_t)rr(0, 65535));
    }
    for (t = 0; t < 4; t++) {
        for (i = 0; i < MG_COUNT; i++) if ((int)(rnd() % 100u) < density) voices_param(t, ENG_MOOG, i, rr(0, 1000));
        for (i = 0; i < PR_COUNT; i++) if ((int)(rnd() % 100u) < density) voices_param(t, ENG_PROPH, i, rr(0, 1000));
        for (i = 0; i < DR_COUNT; i++) if (i % DR_PER != DR_KIT && (int)(rnd() % 100u) < density) voices_param(t, ENG_DRUM, i, rr(0, i % DR_PER == DR_KV ? 2 : i % DR_PER == DR_TYPE ? 7 : 1000));
        for (i = 0; i < ARP_COUNT; i++) if ((int)(rnd() % 100u) < density) voices_arp(t, i, rr(0, 3));
    }
    for (t = 0; t < 4; t++)                    /* arpeggiators running with a latched chord, at some step / phase */
        if ((int)(rnd() % 100u) < density) {
            int n = rr(1, 5), k, r;
            static int32_t tmpb[128];
            voices_arp(t, ARP_LATCH, 1); voices_arp(t, ARP_MODE, rr(1, 8)); voices_arp(t, ARP_OCT, rr(1, 3));
            for (k = 0; k < n; k++) { voices_note(t, rr(40, 80), 1); voices_note(t, 0, 0); }
            r = rr(1, 60);
            while (r--) voices_render(tmpb, 128);
        }
    for (t = 0; t < 4; t++) for (i = 0; i < 96; i++) if ((int)(rnd() % 100u) < density) ui_seq_set_t(t, i / 16, i % 16, rr(-1000, 1000));
}

static uint32_t T = 5000;
static void press(int cb) { ctrl_button(cb, 1, T); T += 40; ctrl_button(cb, 0, T); T += 500; }
static void tick(void) { T += 3000; ctrl_tick(T); }
static void save_preset_ui(void)          /* SAVE, SAVE: the name editor and its commit */
{
    quiet = 0;
    press(CB_SAVE);
    CHECK(ctrl_overlay() == OV_NAME, "name editor not open");
    press(CB_SAVE);
    tick();
}
static void select_preset(int idx)        /* PRESETS knob until entry idx (1-based) */
{
    int guard = 0;
    while (bank_current() != idx && guard++ < 200) { ctrl_side(ROLE_PRESETS, bank_current() < idx ? 1 : -1, T); T += 20; }
    tick();
}
static void scene_save(int w) { cur_bank = -1; ctrl_button(CB_HOME, 1, T); T += 30; ctrl_button(CB_SAVE, 1, T); T += 30; ctrl_button(CB_SAVE, 0, T); T += 30; ctrl_bank_key(w, T); T += 30; ctrl_button(CB_HOME, 0, T); T += 500; tick(); }
static void scene_load(int w) { ctrl_button(CB_HOME, 1, T); T += 30; ctrl_bank_key(w, T); T += 30; ctrl_button(CB_HOME, 0, T); T += 500; tick(); }
static void reboot(void)                  /* RAM pools wiped, everything rebuilt from the flash image */
{
    memset(bpool, 0xA5, sizeof bpool); memset(spool, 0xA5, sizeof spool);
    bank_restore(0); scene_restore(0);
    memset(bpool, 0xA5, sizeof bpool); memset(spool, 0xA5, sizeof spool);
    persist_boot();
}

int main(int argc, char **argv)
{
    static snap_t A, B, C, G;
    int i, t;
    memset(FL, 0xFF, sizeof FL);
    ui_init(); vac_init(); voices_init(); ctrl_init(T);
    printf("P_COUNT %d, ST_MAX %u, BANK_POOL %u, E_HDR %u\n", P_COUNT, ST_MAX, BANK_POOL, E_HDR);

    /* 0. the factory state saves as small as it can */
    {
        uint32_t n = state_save(ebuf, sizeof ebuf, 0), m = state_save(ebuf, sizeof ebuf, 1);
        printf("factory state: preset body %u bytes, scene body %u bytes\n", n, m);
    }

    /* 0a. the step values are per track (0x44..0x47); a legacy 0x06 section (shared by the tracks, no longer written) sets all four */
    {
        uint8_t b[16];
        int tt, bad = 0;
        state_defaults();
        CHECK(ui_seq_get_t(0, 0, 4) == 1000 && ui_seq_get_t(1, 0, 4) == 0, "factory step values: track 1 triangle, track 2 zeros (%d %d)", ui_seq_get_t(0, 0, 4), ui_seq_get_t(1, 0, 4));
        b[0] = 0x06; b[1] = 1; b[2] = 5; b[3] = 0; b[4] = 1; b[5] = 96; b[6] = 21; b[7] = (uint8_t)(420 & 255); b[8] = (uint8_t)(420 >> 8);      /* sparse: value 21 (line 2, step 5) = 420 */
        CHECK(state_load(b, 9) == 1, "legacy 0x06 not applied");
        for (tt = 0; tt < 4; tt++) if (ui_seq_get_t(tt, 1, 5) != 420) bad++;
        CHECK(!bad, "legacy 0x06 set %d tracks of 4", 4 - bad);
        ui_seq_set_t(2, 3, 7, -555);
        { uint32_t n = state_save(ebuf, sizeof ebuf, 0); int k = 0, o = 0; while (o + 4 <= (int)n) { if (ebuf[o] == 0x06) k++; o += 4 + (ebuf[o + 2] | (ebuf[o + 3] << 8)); } CHECK(!k, "0x06 is still written"); }
        state_defaults();
        CHECK(ui_seq_get_t(2, 3, 7) == 0 && ui_seq_get_t(1, 1, 5) == 0, "defaults do not reset the step values");
    }

    /* 0c. the drum lane settings act on the step clock (the ARP page of a drum track: swing / humanize / chance / accent): chance 0 never plays, chance 100 % does */
    {
        static int32_t tmpb[128];
        int k, h0 = 0, h1 = 0, hs = 0;
        state_defaults();
        drum_gates_set(3, 0, 0xFFFF);
        voices_param(3, ENG_DRUM, DR_ID(0, DR_CHANCE), 0);
        voices_play(1);
        for (k = 0; k < 400; k++) { voices_render(tmpb, 128); if (drum_active(3)) h0++; }                 /* 400 blocks = 9 sixteenth notes at 120 bpm */
        voices_play(0);
        voices_param(3, ENG_DRUM, DR_ID(0, DR_CHANCE), 1000);
        voices_play(1);
        for (k = 0; k < 400; k++) { voices_render(tmpb, 128); if (drum_active(3)) h1++; }
        voices_play(0);
        { int v; for (v = 0; v < 6; v++) DRM[3 - DRUM_T0].V[v].on = 0; }                                               /* let the previous hits go */
        drum_gates_set(3, 0, 0x0002);                                                                       /* step 1 only (an odd step: swing delays it) */
        voices_param(3, ENG_DRUM, DR_ID(0, DR_SW), 1000);
        voices_play(1);
        for (k = 0; k < 400; k++) { voices_render(tmpb, 128); if (drum_active(3) && !hs) hs = k; }
        printf("drum lane: chance 0 -> %d blocks sounding, chance 100 %% -> %d; swing 100 %% on step 1: first hit at block %d (step 1 starts at block 43, +.5 step = +22)\n", h0, h1, hs);
        CHECK(h0 == 0, "chance 0 still played (%d blocks)", h0);
        CHECK(h1 > 200, "chance 100 %% did not play (%d blocks)", h1);
        CHECK(hs >= 43 + 20 && hs <= 43 + 24, "swing 100 %% hit at block %d, expected ~65", hs);
        voices_play(0);
        state_defaults();
    }

    /* 0b. destinations follow the engine of the track (dst.c): the lists, the swap of the matrix per track, the validation on an engine change */
    {
        int e, k, bad = 0;
        for (e = 0; e < 4; e++) {               /* every entry of an engine's list is valid for it, in both index spaces; every other engine's entry is not */
            for (k = 0; k < dst_count(e, 0); k++) { int v = dst_at(e, 0, k); if (dst_pos(e, 0, v) != k) bad++; if (v && !((DSTM[v].grp >> e) & 1)) bad++; }
            for (k = 0; k < dst_count(e, 1); k++) { int v = dst_at(e, 1, k); if (dst_pos(e, 1, v) != k) bad++; }
        }
        CHECK(!bad, "destination lists inconsistent (%d)", bad);
        CHECK(dst_count(0, 0) == 70 && dst_pos(0, 0, 10) == 10 && dst_pos(0, 1, 2) == 2, "the Vactrol list is not the one saved presets use");
        voices_set_engine(0, ENG_VAC); voices_set_engine(3, ENG_MOOG);
        voices_select(0); mod_select(0);
        PV[P_M0T] = 3; PV[P_M1T] = 12; PV[P_M4T + 4] = 5 + 3;           /* Freq, Arp gate (any engine but the drums), a sequencer line on FM ratio */
        CHECK(dst_pos(ENG_MOOG, 0, 3) < 0 && dst_pos(ENG_MOOG, 0, 12) >= 0, "Freq must not be a Minimoog destination, Arp gate must");
        mod_select(3); voices_select(3);
        CHECK(PV[P_M0T] == 0 && PV[P_M1T] == 0, "the second track starts with an empty matrix");
        PV[P_M0T] = DSTL1[3];                                          /* a Minimoog destination on track 4 */
        mod_select(0); voices_select(0);
        CHECK(PV[P_M0T] == 3 && PV[P_M1T] == 12, "track 1's matrix did not come back");
        mod_select(3);
        CHECK(PV[P_M0T] == DSTL1[3], "track 4's matrix did not come back");
        voices_set_engine(3, ENG_DRUM);                                  /* the engine of the selected track changes: its destinations are validated */
        CHECK(PV[P_M0T] == 0, "a Minimoog destination survived the change to the drums");
        PV[P_M1T] = DSTL3[5];
        voices_set_engine(3, ENG_PROPH);
        CHECK(PV[P_M1T] == 0, "a drum destination survived the change to the Prophet");
        PV[P_M1T] = 12;                                                  /* Arp gate: valid on the Prophet, not on the drums */
        voices_set_engine(3, ENG_DRUM); CHECK(PV[P_M1T] == 0, "Arp gate survived the change to the drums");
        voices_set_engine(3, ENG_MOOG);
        mod_select(0); voices_select(0);                                  /* a track that is not selected is validated too */
        voices_set_engine(3, ENG_PROPH);
        mod_select(3);
        CHECK(PV[P_M0T] == 0, "unexpected destination on track 4");
        mod_select(0); voices_select(0);
        voices_set_engine(3, ENG_MOOG);
        printf("destination lists: VACT %d, MOOG %d, PROPH %d, DRUM %d entries (LFO), master list %d\n", dst_count(0, 0), dst_count(1, 0), dst_count(2, 0), dst_count(3, 0), N_DST);
    }

    /* 1. preset round trip, every engine: A (engines VAC? random), B, then scramble, reload */
    for (i = 0; i < 4; i++) {                 /* 4 different engine layouts */
        int n0;
        snap_t X, Y;
        randomize(40, 1); take(&A);
        n0 = nbank;
        pi = 0;                               /* "+ NEW" */
        save_preset_ui();
        CHECK(nbank == n0 + 1, "preset %d not saved (bank %d)", i, nbank);
        take(&X);
        CHECK(diff(&A, &X, 0, "save changed live state") == 0, "live state changed by SAVE");
        randomize(70, 1); take(&C);           /* scramble */
        quiet = 0;
        CHECK((quiet = 1, diff(&A, &C, 0, "scramble")) > 0, "scramble did nothing");
        select_preset(bank_current() ? bank_current() : 1);
        (void)Y;
        /* load the preset we just saved: it is the last saved one (pi follows it) */
        {
            int idx = nbank;                  /* find it by k */
            int j, best = 1, kmax = -1;
            for (j = 0; j < nbank; j++) if (bk(j) > kmax) { kmax = bk(j); best = j + 1; }
            idx = best;
            ctrl_side(ROLE_PRESETS, 0, T);
            pi = 0;                            /* force a real load of entry idx */
            select_preset(idx);
        }
        take(&B);
        CHECK(diff(&A, &B, 0, "preset reload") == 0, "preset %d: reload differs", i);
        printf("preset %d: entry %u bytes, engines %d %d %d %d, reload %s\n", i, blen(bank_current() - 1), A.eng[0], A.eng[1], A.eng[2], A.eng[3],
               diff(&A, &B, 0, "x") == 0 ? "identical" : "DIFFERENT");
    }

    /* 1b. every value of every engine of every track changed (density 100), twice, then scrambled the same way */
    for (i = 0; i < 2; i++) {
        pi = 0; state_defaults(); randomize(100, 1); take(&A);
        save_preset_ui();
        randomize(100, 1);
        { int j, best = 1, kmax = -1; for (j = 0; j < nbank; j++) if (bk(j) > kmax) { kmax = bk(j); best = j + 1; } pi = 0; select_preset(best); }
        take(&B);
        CHECK(diff(&A, &B, 0, "full") == 0, "full-density preset %d reload differs", i);
        printf("every value edited, preset reload %s (%u bytes)\n", diff(&A, &B, 0, "x") == 0 ? "identical" : "DIFFERENT", blen(bank_current() - 1));
    }
    /* 1c. a typical preset: one Minimoog edited, a few Vactrol / LFO / FX knobs */
    {
        state_defaults();
        for (i = 0; i < MG_COUNT; i++) voices_param(1, ENG_MOOG, i, rr(0, 1000));
        for (i = P_FOLD; i <= P_MODE; i++) if (i % 2) PV[i] = (int16_t)rr(PD[i].min, PD[i].max);
        PV[P_F1RATE] = 123; PV[P_F6MIX] = 456; PV[P_M0A] = 321;
        printf("typical preset (one Minimoog, 7 Vactrol knobs, 3 FX/LFO knobs): body %u bytes\n", state_save(ebuf, sizeof ebuf, 0));
    }

    /* 2. overwrite: SAVE again on a loaded preset keeps the count and takes the new values */
    {
        int n0 = nbank;
        randomize(30, 0); take(&A);
        save_preset_ui();
        CHECK(nbank == n0, "overwrite changed the count (%d -> %d)", n0, nbank);
        randomize(80, 0);
        pi = 0; select_preset(bank_current() ? bank_current() : 1);
    }

    /* 3. key scenes: 4 tracks, volumes included */
    for (i = 0; i < 3; i++) {
        randomize(50, 1); take(&A);
        scene_save(i * 5 + 1);
        CHECK(((scn_used >> (i * 5 + 1)) & 1), "scene %d not stored", i * 5 + 1);
    }
    {
        snap_t S[3];
        uint16_t used = scn_used;
        for (i = 0; i < 3; i++) { randomize(60, 1); take(&A); scene_save(i * 5 + 1); S[i] = A; }   /* overwrite them with known states */
        CHECK(scn_used == used, "scene mask changed by overwrite");
        for (i = 0; i < 3; i++) {
            randomize(90, 1);
            scene_load(i * 5 + 1);
            take(&B);
            CHECK(diff(&S[i], &B, 1, "scene") == 0, "scene %d reload differs", i * 5 + 1);
            printf("scene %d: entry %u bytes, reload %s\n", i * 5 + 1, e_len(spool + soff[i * 5 + 1]), diff(&S[i], &B, 1, "x") == 0 ? "identical" : "DIFFERENT");
        }
        /* an empty scene resets to the defaults: engines VAC MOOG PROPH DRUM and the factory values */
        randomize(90, 1);
        scene_load(14);
        take(&B);
        for (t = 0; t < 4; t++) CHECK(B.eng[t] == t, "default scene: track %d engine %d", t, B.eng[t]);
        CHECK(B.owner == 0, "default scene: owner");
        for (i = 0; i < MG_COUNT; i++) CHECK(B.mg[2][i] == st_def_mg[i], "default moog %d", i);
        for (i = 0; i < P_COUNT; i++) if (i >= P_FOLD && i <= P_MODE) CHECK(B.pv[i] == PD[i].def, "default PV %d", i);
    }

    /* 4. reboot: the pools are rebuilt from flash alone */
    {
        int nb = nbank, k;
        uint16_t used = scn_used;
        static snap_t S2;
        char names[BANK_MAX][16];
        for (k = 0; k < nb; k++) strcpy(names[k], bname(k));
        /* the scene content before: load scene 6 */
        randomize(50, 1); scene_load(6); take(&S2);
        reboot();
        CHECK(nbank == nb, "reboot: %d presets, was %d", nbank, nb);
        for (k = 0; k < nbank; k++) CHECK(!strcmp(names[k], bname(k)), "reboot: name %d", k);
        CHECK(scn_used == used, "reboot: scenes %x was %x", scn_used, used);
        randomize(90, 1);
        scene_load(6); take(&B);
        CHECK(diff(&S2, &B, 1, "scene after reboot") == 0, "scene 6 differs after reboot");
        printf("reboot: %d presets (%u bytes), scenes mask %04x (%u bytes), scene 6 identical %s\n", nbank, bpool_n, scn_used, spool_n, diff(&S2, &B, 1, "x") == 0 ? "yes" : "NO");
        /* and every preset still loads to what it was: compare entry bytes with a fresh save of itself is not possible; reload preset 1 */
    }

    /* 5. old format (Vactrol only): a record written by the previous persist.c is still read */
    {
        static preset_v1_t old[2];
        static int16_t sc[16][P_COUNT];
        uint8_t hdr[16];
        uint32_t used = (1u << 2) | (1u << 9), s, crc;
        uint8_t *pay;
        int rep;
        memset(FL + 0x93000, 0xFF, 0x4000); memset(FL + 0xE7000, 0xFF, 0x10000); ps_leg0 = 0;
        memset(old, 0, sizeof old);
        strcpy(old[0].n, "OLD ONE"); old[0].type = 0; old[0].k = 1;
        strcpy(old[1].n, "OLD TWO"); old[1].type = 1; old[1].k = 2;
        for (i = 0; i < P_COUNT; i++) { old[0].v[i] = PD[i].def; old[1].v[i] = PD[i].def; }
        old[0].v[P_FOLD] = 777; old[0].v[P_FM] = 333; old[0].v[P_F0DRIVE] = 444; old[1].v[P_FOLD] = 123; old[1].v[P_MODE] = 2; old[1].v[P_BPM] = 99;
        for (rep = 0; rep < 2; rep++) {
            uint32_t base = rep ? 0xE7000u : 0x93000u, ver, len;
            static uint8_t buf[2 + 32 * P_COUNT + 4096];
            if (!rep) { pay = (uint8_t *)old; len = sizeof old; ver = sizeof(preset_v1_t); }
            else {
                memset(sc, 0, sizeof sc);
                for (i = 0; i < P_COUNT; i++) { sc[2][i] = PD[i].def; sc[9][i] = PD[i].def; }
                sc[2][P_FOLD] = 555; sc[9][P_SYM] = 666; sc[9][P_F1RATE] = 321;
                buf[0] = (uint8_t)used; buf[1] = (uint8_t)(used >> 8);
                memcpy(buf + 2, sc, sizeof sc);
                pay = buf; len = 2 + sizeof sc; ver = 0x10000u | P_COUNT;
            }
            ps_put32(hdr, PS_MAGIC); ps_put32(hdr + 4, ver); ps_put32(hdr + 8, len); ps_put32(hdr + 12, ps_crc(pay, len));
            crc = 0;
            for (s = 0; s < PS_HDR + len; s += 1) {
                uint32_t a = ps_addr(base, s);
                FL[a] = s < PS_HDR ? hdr[s] : pay[s - PS_HDR];
            }
            (void)crc;
        }
        reboot();
        CHECK(nbank == 2, "old format: %d presets", nbank);
        CHECK(scn_used == used, "old format: scenes %x", scn_used);
        if (nbank == 2) {
            pi = 0; select_preset(1);
            printf("old format: preset 1 '%s' PV[FOLD]=%d FM=%d F0DRIVE=%d (expected 777 333 444)\n", bname(0), PV[P_FOLD], PV[P_FM], PV[P_F0DRIVE]);
            CHECK(PV[P_FOLD] == 777 && PV[P_FM] == 333 && PV[P_F0DRIVE] == 444, "old preset values");
            pi = 0; select_preset(2);
            CHECK(PV[P_FOLD] == 123 && PV[P_MODE] == 2, "old preset 2 values");
            CHECK(PV[P_BPM] != 99 || PD[P_BPM].def == 99, "old preset must not change the tempo");
        }
        scene_load(2); CHECK(PV[P_FOLD] == 555, "old scene 2 (%d)", PV[P_FOLD]);
        scene_load(9); CHECK(PV[P_SYM] == 666 && PV[P_F1RATE] == 321, "old scene 9");
        printf("old format: 2 presets + 2 scenes converted and loaded\n");
        /* a save now rewrites the region in the new format */
        save_preset_ui();
        {
            uint32_t ver, n;
            CHECK(ps_open(PS_BASE, PS_MAX0, &ver, &n) == 0 && ver == PS_VER_BANK, "rewritten in the new format");
            CHECK(ps_open(PS_BASE_V1, PS_MAX_V1, &ver, &n) != 0 && !ps_leg0, "the first-format copy is erased after the first new save");
            { int nb = nbank; reboot(); CHECK(nbank == nb, "after migration + reboot: %d presets, was %d", nbank, nb); }
        }
    }

    /* 6. power cut at every flash operation of a preset save: old record, new record, or nothing - never a mix */
    {
        int op, outcomes[3] = { 0, 0, 0 }, total;
        static uint8_t flash0[1 << 20];
        static uint8_t old_img[BANK_POOL];
        uint32_t old_n;
        randomize(40, 1); pi = 0; save_preset_ui();
        memcpy(flash0, FL, sizeof FL);
        old_n = bpool_n; memcpy(old_img, bpool, old_n);
        randomize(40, 1);
        /* the new record = old + one more preset */
        for (total = 0; total < 40; total++) {
            uint32_t n_before = nbank;
            memcpy(FL, flash0, sizeof FL);
            fl_ops = 0; fl_fail_at = total;
            {   /* build the new bank in RAM and try to write it */
                int p0 = pi, nb0 = nbank;
                uint32_t bn0 = bpool_n;
                static uint8_t keep[BANK_POOL];
                memcpy(keep, bpool, bpool_n);
                pi = 0; save_preset_ui();
                fl_fail_at = -1;
                {   /* the RAM now holds the new bank; what survives in flash after the cut: */
                    uint32_t new_n = bpool_n;
                    static uint8_t new_img[BANK_POOL];
                    memcpy(new_img, bpool, new_n);
                    reboot();
                    if (bpool_n == old_n && !memcmp(bpool, old_img, old_n) && nbank == (int)n_before) outcomes[0]++;
                    else if (bpool_n == new_n && !memcmp(bpool, new_img, new_n)) outcomes[1]++;
                    else if (nbank == 0 && bpool_n == 0) outcomes[2]++;
                    else { fails++; printf("FAIL power cut at op %d: neither old, new nor empty (%d presets, %u bytes)\n", total, nbank, bpool_n); }
                    /* put RAM back to the old bank for the next try */
                    memcpy(bpool, keep, bn0); bank_restore(bn0); pi = p0; (void)nb0;
                }
            }
            if (fl_ops < total) break;       /* the save needed fewer operations: every cut point has been covered */
        }
        printf("power cut at each of the first %d flash operations: old record %d, new record %d, empty %d, other 0\n", total, outcomes[0], outcomes[1], outcomes[2]);
        fl_fail_at = -1;
    }

    /* 7. capacity: how many random presets / scenes fit */
    {
        int n = 0, ns = 0;
        uint32_t tot = 0;
        static uint8_t keep[BANK_POOL];
        bank_restore(0); scene_restore(0);
        while (n < BANK_MAX) {
            uint32_t b;
            state_defaults(); randomize(25, 1);
            b = state_save(ebuf + E_HDR, sizeof ebuf - E_HDR, 0);
            if (bpool_n + E_HDR + b > BANK_POOL) break;
            tot += E_HDR + b;
            pi = 0; save_preset_ui(); pi = 0; n++;
        }
        printf("25%% of the values edited: %d presets fit (avg %u bytes) before the %u-byte pool is full or BANK_MAX %d\n", n, n ? tot / (uint32_t)n : 0, BANK_POOL, BANK_MAX);
        (void)keep;
        for (i = 0; i < 16; i++) { state_defaults(); randomize(25, 1); scene_save(i); if ((scn_used >> i) & 1) ns++; }
        printf("16 scenes at 25%% edited: %d stored, %u of %u bytes\n", ns, spool_n, SCENE_POOL);
        bank_restore(0); pi = 0;
        n = 0; tot = 0;
        for (;;) {
            uint32_t b;
            randomize(100, 1);
            b = state_save(ebuf + E_HDR, sizeof ebuf - E_HDR, 0);
            if (!n) printf("worst case (every value edited): preset %u bytes + %u header\n", b, E_HDR);
            if (bpool_n + E_HDR + b > BANK_POOL || n >= BANK_MAX) break;
            pi = 0; save_preset_ui(); pi = 0; n++;
        }
        printf("worst case: %d presets fit\n", n);
        scene_restore(0);
        for (i = 0; i < 16; i++) { randomize(100, 1); scene_save(i); }
        printf("worst case: scenes stored %d of 16\n", __builtin_popcount(scn_used));
        /* the bank is full: SAVE must refuse and leave everything as it was */
        {
            uint32_t bn = bpool_n; int nb = nbank;
            randomize(100, 1);
            pi = 0; save_preset_ui();
            printf("save into a full bank: bank %d -> %d presets, %u -> %u bytes, toast '%s'\n", nb, nbank, bn, bpool_n, toast_a);
        }
    }
    /* 7b. M: two drum machines only (tracks 3 and 4), records from before M still load, the Prophet is polyphonic */
    {
        static int32_t tb[128];
        uint8_t body[2 + 64 + 8 + 2 + 240 + 8], *w = body;
        int k, i2, nv, peak = 0, held = 0;
        state_defaults();
        voices_set_engine(0, ENG_DRUM); voices_set_engine(1, ENG_DRUM); voices_set_engine(2, ENG_DRUM);
        CHECK(voices_engine(0) != ENG_DRUM && voices_engine(1) != ENG_DRUM && voices_engine(2) != ENG_DRUM, "the drum engine was accepted on track 1, 2 or 3");
        voices_set_engine(3, ENG_DRUM);
        CHECK(voices_engine(3) == ENG_DRUM, "track 4 refused the drum engine");
        CHECK(drum_get(0, DR_ID(0, DR_LEVEL)) == 0 && drum_gates(2, 0) == 0, "drum data exists for a track that cannot be a drum track");
        /* an old record: TRK says drums on track 1, DSQ rev 1 holds four drum tracks: track 1 becomes a Minimoog, the lanes of tracks 3 and 4 are kept */
        *w++ = 0x08; *w++ = 1; *w++ = 5; *w++ = 0; *w++ = ENG_DRUM; *w++ = ENG_MOOG; *w++ = ENG_DRUM; *w++ = ENG_DRUM; *w++ = 0;
        *w++ = 0x42; *w++ = 1; *w++ = (uint8_t)((2 + 240) & 255); *w++ = (uint8_t)((2 + 240) >> 8); *w++ = 0; *w++ = 120;
        for (i2 = 0; i2 < 120; i2++) { int v = i2 == 60 ? 0x1234 : i2 == 91 ? 0x00F0 : i2 == 0 ? 0x0F0F : (i2 % 30 >= 6 && (i2 % 30 - 6) % 4 == DR_CHANCE - DR_SW) ? 1000 : 0; *w++ = (uint8_t)v; *w++ = (uint8_t)(v >> 8); }
        state_defaults();
        k = state_load(body, (uint32_t)(w - body));
        CHECK(k == 2, "an old record applied %d sections instead of 2", k);
        CHECK(voices_engine(0) == ENG_MOOG, "track 1 is %d after an old record asked for drums", voices_engine(0));
        CHECK(drum_gates(3, 1) == 0x00F0 && drum_gates(2, 0) == 0, "old DSQ lanes of track 4 kept, track 3 dropped: %04x %04x", drum_gates(3, 1), drum_gates(2, 0));
        /* a rev 2 record (two drum tracks, tracks 3 and 4): the lanes of track 4 are kept, track 3's are dropped */
        {
            uint8_t b2[4 + 2 + 120], *q = b2;
            *q++ = 0x42; *q++ = 2; *q++ = (uint8_t)((2 + 120) & 255); *q++ = (uint8_t)((2 + 120) >> 8); *q++ = 0; *q++ = 60;
            for (i2 = 0; i2 < 60; i2++) { int v = i2 == 0 ? 0x1111 : i2 == 30 ? 0x2222 : (i2 % 30 >= 6 && (i2 % 30 - 6) % 4 == DR_CHANCE - DR_SW) ? 1000 : 0; *q++ = (uint8_t)v; *q++ = (uint8_t)(v >> 8); }
            state_defaults();
            k = state_load(b2, (uint32_t)(q - b2));
            CHECK(k == 1 && drum_gates(3, 0) == 0x2222 && drum_gates(2, 0) == 0, "a rev 2 DSQ: %d sections, lanes %04x %04x", k, drum_gates(3, 0), drum_gates(2, 0));
        }
        /* the Prophet: 5 voices, a 6th note steals, a release frees only its own voice */
        state_defaults();
        voices_set_engine(1, ENG_PROPH);
        for (i2 = 0; i2 < 5; i2++) voices_note(1, 48 + 4 * i2, 1);
        for (k = 0; k < 40; k++) { voices_render(tb, 128); for (i2 = 0; i2 < 128; i2++) { int a = tb[i2] < 0 ? -tb[i2] : tb[i2]; if (a > peak) peak = a; } }
        nv = proph_nvoices(1);
        CHECK(nv == 5, "a five note chord sounds %d voices", nv);
        CHECK(peak > 500 && peak < 32767, "chord peak %d", peak);
        voices_note(1, 80, 1);
        for (k = 0; k < 4; k++) voices_render(tb, 128);
        CHECK(proph_nvoices(1) == 5, "the sixth note did not steal a voice (%d voices)", proph_nvoices(1));
        { int on80 = 0; for (i2 = 0; i2 < PR_NV; i2++) if (PR[1].V[i2].note == 80 && PR[1].V[i2].gate) on80++; CHECK(on80 == 1, "the sixth note has %d voices", on80); }
        voices_note(1, 80, 0);
        for (i2 = 0; i2 < PR_NV; i2++) held += PR[1].V[i2].gate;
        CHECK(held == 4, "releasing one key left %d voices held", held);
        for (k = 0; k < 600; k++) voices_render(tb, 128);                     /* the others still sound (sustain) */
        CHECK(proph_nvoices(1) == 4, "after the release %d voices sound, expected 4", proph_nvoices(1));
        for (i2 = 0; i2 < 5; i2++) voices_note(1, 48 + 4 * i2, 0);
        for (k = 0; k < 1500; k++) voices_render(tb, 128);
        CHECK(!proph_active(1), "the Prophet never went silent after every key was released");
        printf("polyphony: 5 voices, steal on the sixth, per-key release; two drum machines (tracks 3, 4); old records with drums on track 1 / 2 load as Minimoog\n");
        state_defaults();
    }
    /* 7c. M: the track volume (HOME's fader, PV[P_VOL0 + t]) reaches the mix: the rms follows it for every engine, a scene keeps it */
    {
        static int32_t tb[128];
        static const int vols[4] = { 240, 120, 60, 0 };
        static const int eng[3] = { ENG_MOOG, ENG_PROPH, ENG_DRUM };
        int e, vi, k, i2, t;
        double base = 0;
        for (e = 0; e < 3; e++) {
            t = e == 2 ? 3 : 1;
            for (vi = -1; vi < 4; vi++) {                                          /* vi = -1: a warm-up run, not checked (the first note of a fresh engine differs) */
                double sum = 0, r;
                voices_note(t, 36, 0); voices_note(t, 48, 0); voices_note(t, 55, 0);
                for (k = 0; k < 800; k++) voices_render(tb, 128);                          /* every case starts from silence */
                state_defaults();
                voices_set_engine(t, eng[e]);
                for (i2 = 0; i2 < 4; i2++) PV[P_VOL0 + i2] = 0;                        /* the other tracks (a sequencer may play there) stay out of the measure */
                PV[P_VOL0 + t] = (int16_t)vols[vi < 0 ? 0 : vi];
                if (e == 2) { drum_gates_set(t, 0, 0xFFFFu); voices_note(t, 36, 1); }
                else { voices_note(t, 48, 1); voices_note(t, 55, 1); }
                for (k = 0; k < 60; k++) voices_render(tb, 128);                  /* the ramp of the volume, the attack */
                for (k = 0; k < 200; k++) { voices_render(tb, 128); for (i2 = 0; i2 < 128; i2++) sum += (double)tb[i2] * tb[i2]; }
                r = sqrt(sum / (200 * 128));
                if (vi < 0) continue;
                if (vi == 0) base = r;
                CHECK(vi == 0 ? r > 200 : (vols[vi] == 0 ? r < 2 : (r > base * vols[vi] / 240.0 * 0.7 && r < base * vols[vi] / 240.0 * 1.3)),
                      "engine %d volume %d: rms %.0f (volume 240: %.0f)", eng[e], vols[vi], r, base);
            }
        }
        /* the volume of a fader moved while sounding changes the level of the running track, and a scene stores and restores it */
        state_defaults();
        voices_set_engine(1, ENG_MOOG);
        PV[P_VOL1] = 800; voices_note(1, 48, 1);
        for (k = 0; k < 200; k++) voices_render(tb, 128);
        PV[P_VOL1] = 300; PV[P_VOL3] = 650;
        scene_save(3);
        PV[P_VOL1] = 800; PV[P_VOL3] = 800;
        scene_load(3);
        CHECK(PV[P_VOL1] == 300 && PV[P_VOL3] == 650, "scene volumes %d %d after load (300 650 expected)", PV[P_VOL1], PV[P_VOL3]);
        printf("track volume: rms follows the fader on the Minimoog, the Prophet and the drums; scenes keep the volumes\n");
        state_defaults();
    }
    /* 7d. M: SELECT alone on the drum sequencer steps the voice (lane), with wrap; the voice's black key (OPk) lights; ARP + OPk, SEQ + SELECT keep their meaning */
    {
        int v0, i2, n, bk[8], nb = 0, base, k2, ok = 1;
        static const uint8_t WH[12] = { 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1 };
        state_defaults(); km_init();
        voices_select(3); voices_set_engine(3, ENG_DRUM);
        sel = 3; pg = PGN_DSEQ; dv[3] = 0; alg_t = 0; pend = 0; bdn = 0; dq_held = dq_latch = -1;
        base = ui_note_of_key(0);
        for (i2 = 0; i2 < 27; i2++) if (!WH[(base + i2) % 12]) bk[nb++] = i2;
        for (k2 = 0; k2 < 14; k2++) {                                   /* 14 clicks forward: 0 1 2 3 4 5 0 1 ... */
            ctrl_side(ROLE_SELECT, 1, 1000u + 10u * (uint32_t)k2);
            CHECK(ctrl_drum_voice() == (k2 + 1) % 6, "SELECT +1 x%d: voice %d", k2 + 1, ctrl_drum_voice());
        }
        ctrl_side(ROLE_SELECT, -3, 2000); v0 = ctrl_drum_voice();
        CHECK(v0 == ((14 % 6) + 3) % 6, "SELECT -3: voice %d", v0);
        ctrl_side(ROLE_SELECT, -6 * 5 - 1, 2100);
        CHECK(ctrl_drum_voice() == (v0 + 5) % 6, "SELECT wraps below 0: %d", ctrl_drum_voice());
        for (v0 = 0; v0 < 6; v0++) {                                    /* the LED of exactly that voice's black key is lit on DSEQ */
            dv[3] = (uint8_t)v0; fm1_in.notes = 0; fm1_in.buttons = 0; leds_update();
            n = 0; for (i2 = 0; i2 < 27; i2++) if (!WH[(base + i2) % 12] && (fm1_led[14 + i2] & 2)) { n++; if (i2 != bk[v0]) ok = 0; }
            CHECK(n == 1 && ok && (fm1_led[14 + bk[v0]] & 2), "voice %d: %d black keys lit (key %d)", v0, n, bk[v0]);
            ok = 1;
        }
        dv[3] = 2; sel = 3; pg = PGN_EDIT; leds_update();
        CHECK(fm1_led_dim[14 + bk[2]] & 2, "EDIT on a drum track: the selected voice's key glows");
        pg = PGN_DSEQ; dv[3] = 1; bdn = 1u << CB_SEQ;                    /* SEQ held: SELECT = the lane's voice type, not the lane */
        ctrl_side(ROLE_SELECT, 1, 3000);
        CHECK(ctrl_drum_voice() == 1, "SEQ + SELECT changed the lane (%d)", ctrl_drum_voice());
        bdn = 0; dq_latch = 4; dv[3] = 4; ctrl_side(ROLE_SELECT, 1, 4000);
        CHECK(ctrl_drum_voice() == 5 && dq_latch == 5, "a latched lane follows SELECT (%d, %d)", ctrl_drum_voice(), dq_latch);
        dq_latch = -1; sel = 0; pg = PGN_GLO; voices_select(0);
        printf("drum sequencer: SELECT steps the voice with wrap, its OP key lights, SEQ + SELECT unchanged\n");
        state_defaults();
    }
    /* 7e. M: the debug indicator: HOME held + REC toggles it, REC alone still records */
    {
        state_defaults();
        hud.on = 0; rec = 0;
        ctrl_button(CB_HOME, 1, 5000); ctrl_button(CB_REC, 1, 5010); ctrl_button(CB_REC, 0, 5050); ctrl_button(CB_HOME, 0, 5100);
        CHECK(hud.on == 1 && rec == 0 && ctrl_page() != PGN_HOME, "HOME + REC: indicator %d, rec %d, page %d", hud.on, rec, ctrl_page());
        ctrl_button(CB_HOME, 1, 6000); ctrl_button(CB_REC, 1, 6010); ctrl_button(CB_REC, 0, 6050); ctrl_button(CB_HOME, 0, 6100);
        CHECK(hud.on == 0, "HOME + REC again does not hide the indicator");
        ctrl_button(CB_REC, 1, 7000); ctrl_button(CB_REC, 0, 7050);
        CHECK(rec == 1 && hud.on == 0, "REC alone: rec %d", rec);
        rec = 0; pg = PGN_GLO;
        printf("debug indicator: HOME + REC toggles it\n");
#if FEATURE_MAXTRACKS
        { int k, seq[5], t0 = 8000;                                  /* MAXTRACKS: HOME held + PLAY steps 4 -> 3 -> 2 -> 1 -> 4, never saved */
          max_tracks = 4;
          for (k = 0; k < 5; k++) { ctrl_button(CB_HOME, 1, t0); ctrl_button(CB_PLAY, 1, t0 + 10); ctrl_button(CB_PLAY, 0, t0 + 50); ctrl_button(CB_HOME, 0, t0 + 100); seq[k] = max_tracks; t0 += 1000; }
          CHECK(seq[0] == 3 && seq[1] == 2 && seq[2] == 1 && seq[3] == 4 && seq[4] == 3, "MAX TRACKS cycle %d %d %d %d %d", seq[0], seq[1], seq[2], seq[3], seq[4]);
          max_tracks = 4; printf("MAX TRACKS: HOME + PLAY cycles 4-3-2-1\n"); }
#endif
    }
    /* 8. a record written by tools/mkflashrec.py (an independent encoder) boots and loads: run with the image as argv[1] */
    if (argc > 1) {
        FILE *f = fopen(argv[1], "rb");
        if (f && fread(FL, 1, sizeof FL, f) == sizeof FL) {
            reboot();
            CHECK(nbank == 1 && !strcmp(bname(0), "RESTORED"), "mkflashrec preset: %d presets '%s'", nbank, nbank ? bname(0) : "");
            pi = 0; select_preset(1);
            CHECK(PV[P_FOLD] == 900 && voices_param_get(1, ENG_MOOG, MG_CUT) == 123 && voices_engine(0) == 1 && voices_engine(1) == 1 && voices_engine(2) == 2 && voices_engine(3) == 3,
                  "mkflashrec preset values (fold %d, cut %d, engines %d %d %d %d)", PV[P_FOLD], voices_param_get(1, ENG_MOOG, MG_CUT), voices_engine(0), voices_engine(1), voices_engine(2), voices_engine(3));
            randomize(90, 1); scene_load(1);
            CHECK(PV[P_FOLD] == 900 && voices_param_get(1, ENG_MOOG, MG_CUT) == 123 && voices_engine(0) == 1, "mkflashrec scene values");
            printf("tools/mkflashrec.py image: preset + scene read and applied\n");
        } else { fails++; printf("FAIL: cannot read %s\n", argv[1]); }
        if (f) fclose(f);
    }
    printf("flash protocol violations (bad erase/program ranges, over-programming): %d\n", fl_bad);
    CHECK(fl_bad == 0, "flash protocol");
    printf("%s (%d failures)\n", fails ? "FAILED" : "ALL OK", fails);
    return fails != 0;
}
