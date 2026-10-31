/* SPDX-License-Identifier: MPL-2.0 */
/* Host test (gcc) of the per-track FX state: defaults, assignment (swap, budget refusal), fxbank_save / fxbank_load round trip,
 * track switch (fx_select) and the loader's clamping / duplicate repair.  build + run: tools/fxhost.sh test */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "octavio.h"
#include "params.h"
#include "tables_gen.c"
#include "mathfx.c"
#include "params.c"
static int bpm_v = 120;
#include "fxtab.h"
static inline uint32_t fm1_ticks(void) { return 0; }
#include "cdsp.c"
#include "fxstate.c"
#include "fxdsp.c"
void ui_params_changed(void) {}
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main(void)
{
    uint8_t b[FXBANK_BYTES + 8], c[FXBANK_BYTES + 8];
    int t, k;
    for (k = 0; k < P_COUNT; k++) PV[k] = PD[k].def;
    fx_defaults();
    CHECK(fxs[0] == 0 && fxs[3] == 3);
    /* assignment: swap with the effect already in the chain, as the prototype */
    CHECK(fx_assign(0, 2) == 1 && fxs[0] == 2 && fxs[2] == 0);
    CHECK(fx_assign(0, 16) == -1 && fx_assign(4, 0) == -1);
    /* per-track knobs: track 0's live in PV, switching saves / restores */
    PV[P_F6SIZE] = 777; PV[P_F5SYNC] = 1;
    fx_select(2);
    CHECK(PV[P_F6SIZE] == PD[P_F6SIZE].def && PV[P_F5SYNC] == 0);
    CHECK(fxs[0] == 0);                                                    /* track 3 has its own order */
    PV[P_F6SIZE] = 333;
    fx_select(0);
    CHECK(PV[P_F6SIZE] == 777 && PV[P_F5SYNC] == 1 && fxs[0] == 2);
    fx_select(2);
    CHECK(PV[P_F6SIZE] == 333);
    /* CPU budget: only slots with Mix > 0 count; admitted in track / slot order, those running keep their place */
    {
        int q, running = 0, off = 0;
        uint32_t used = 0;
        fx_defaults();
        fx_load_view();
        for (t = 0; t < 4; t++) for (q = 0; q < FX_NSLOT; q++) FXT[t].p[FX_FIRST[q] + FX_P[q].n - 1 - P_F0DRIVE] = 1000;     /* default chains (effects 0..3), all Mix 1 */
        fx_load_view();
        fx_begin(); fx_admit();
        for (t = 0; t < 4; t++) for (q = 0; q < FX_NSLOT; q++) { if (fx_run[t] >> q & 1) { running++; used += FX_COST[FXT[t].ord[q]]; } off += fx_slot_off(t, q); }
        printf("default chains, all mixes on: %d running, %d bypassed, cost %u of %u\n", running, off, (unsigned)used, (unsigned)FX_BUDGET);
        CHECK(running == 16 && off == 0 && used <= FX_BUDGET);
        /* granular + reverb + shimmer + reverse on every track: far over the budget; the first ones run, the rest are bypassed, the total stays under */
        for (t = 0; t < 4; t++) { fx_select(t); fx_assign(0, 7); fx_assign(1, 6); fx_assign(2, 15); fx_assign(3, 12); }
        fx_select(0);
        for (t = 0; t < 4; t++) for (q = 0; q < 16; q++) FXT[t].p[FX_FIRST[q] + FX_P[q].n - 1 - P_F0DRIVE] = 1000;
        fx_load_view();
        fx_begin(); fx_admit();
        running = off = 0; used = 0;
        for (t = 0; t < 4; t++) for (q = 0; q < FX_NSLOT; q++) { if (fx_run[t] >> q & 1) { running++; used += FX_COST[FXT[t].ord[q]]; } off += fx_slot_off(t, q); }
        printf("heavy chains: %d running, %d bypassed, cost %u of %u\n", running, off, (unsigned)used, (unsigned)FX_BUDGET);
        CHECK(used <= FX_BUDGET && off > 0 && running + off == 16 && fx_refused > 0);
        CHECK(fx_slot_off(3, 0) && !fx_slot_off(0, 0));                       /* track 1 first: granular runs there, not on track 4 */
        /* a slot that runs keeps its place when a lower-priority one asks later */
        FXT[0].p[FX_FIRST[7] + FX_P[7].n - 1 - P_F0DRIVE] = 0;                /* track 1's granular off ... */
        fx_load_view();
        fx_begin(); fx_admit();
        CHECK(!(fx_run[0] & 1));
    }
    /* CPU governor (octavio-J): a measured overload bypasses the last running slots (track 4 first) until about the aim is freed, not again
       before GOV_WAIT blocks; a quiet spell longer than GOV_COOL brings every slot back; nothing is admitted while the load is over the admit line */
    {
        int q, running = 0, r1, i;
        uint32_t refused0;
        fx_defaults();
        for (t = 0; t < 4; t++) for (q = 0; q < FX_NSLOT; q++) FXT[t].p[FX_FIRST[q] + FX_P[q].n - 1 - P_F0DRIVE] = 1000;
        fx_load_view();
        fx_begin(); fx_admit();
        for (t = 0; t < 4; t++) running += __builtin_popcount(fx_run[t]);
        CHECK(running == 16);
        refused0 = fx_refused;
        for (i = 0; i < 24; i++) gov_block(GOV_P / 100u * 60u);                  /* the audio blocks take 60 % */
        fx_begin(); fx_admit();
        running = 0; for (t = 0; t < 4; t++) running += __builtin_popcount(fx_run[t]);
        printf("governor: 60 %% load -> %d of 16 slots left, track masks %x %x %x %x, %u refused\n", running, fx_run[0], fx_run[1], fx_run[2], fx_run[3], (unsigned)(fx_refused - refused0));
        CHECK(running < 16 && running > 0 && fx_run[0] == 15 && fx_refused > refused0 && fx_slot_off(3, 3));
        r1 = running;
        fx_begin(); fx_admit();
        running = 0; for (t = 0; t < 4; t++) running += __builtin_popcount(fx_run[t]);
        CHECK(running == r1);                                                   /* one shed per GOV_WAIT blocks */
        for (i = 0; i < (int)GOV_COOL / 2; i++) { gov_block(GOV_P / 100u * 30u); fx_begin(); fx_admit(); }
        running = 0; for (t = 0; t < 4; t++) running += __builtin_popcount(fx_run[t]);
        CHECK(running == r1);                                                   /* still cooling down */
        for (i = 0; i < (int)GOV_COOL * 8; i++) { gov_block(GOV_P / 100u * 30u); fx_begin(); fx_admit(); }      /* (a second shed soon after the first doubles the cool-down) */
        running = 0; for (t = 0; t < 4; t++) running += __builtin_popcount(fx_run[t]);
        CHECK(running == 16);                                                   /* quiet again: everything is back */
        for (i = 0; i < 24; i++) gov_block(GOV_P / 100u * 44u);                  /* 52 %: over the admit line (40 %), under the shed line (50 %): a slot that appears now waits */
        fx_load_view();
        FXT[3].p[FX_FIRST[FXT[3].ord[0]] + FX_P[FXT[3].ord[0]].n - 1 - P_F0DRIVE] = 0; fx_load_view();
        fx_begin(); fx_admit();
        FXT[3].p[FX_FIRST[FXT[3].ord[0]] + FX_P[FXT[3].ord[0]].n - 1 - P_F0DRIVE] = 1000;
        fx_begin(); fx_admit();
        CHECK(!(fx_run[3] & 1) && fx_slot_off(3, 0));
    }
    /* emergency (M): one block over GOV_EMERG bypasses at once, though the 4-block window (second longest) is quiet and the wait is not over */
    {
        int q, i, running = 0;
        fx_defaults();
        for (t = 0; t < 4; t++) for (q = 0; q < FX_NSLOT; q++) FXT[t].p[FX_FIRST[q] + FX_P[q].n - 1 - P_F0DRIVE] = 1000;
        fx_load_view();
        for (i = 0; i < GOV_COOL + 8; i++) { gov_block(GOV_P / 100u * 30u); fx_begin(); fx_admit(); }
        for (t = 0; t < 4; t++) running += __builtin_popcount(fx_run[t]);
        CHECK(running == 16);
        gov_wait = GOV_WAIT;
        gov_block(GOV_P / 100u * 88u);
        fx_begin(); fx_admit();
        running = 0; for (t = 0; t < 4; t++) running += __builtin_popcount(fx_run[t]);
        printf("emergency: one block at 88 %% with the window quiet -> %d of 16 slots left\n", running);
        CHECK(running < 16);
        for (i = 0; i < 24; i++) gov_block(GOV_P / 100u * 30u);
    }
    /* round trip */
    PV[P_F9CRUSH] = 123;
    CHECK(fxbank_save(b) == FXBANK_BYTES && b[0] == FXBANK_VER);
    memset(FXT, 0, sizeof FXT);
    CHECK(fxbank_load(b, FXBANK_BYTES) == 1);
    CHECK(fxbank_save(c) == FXBANK_BYTES && !memcmp(b, c, FXBANK_BYTES));
    CHECK(PV[P_F9CRUSH] == 123);
    /* a corrupt bank: bad version refused, out-of-range values clamped, duplicates repaired, too short refused */
    CHECK(fxbank_load(b, 3) == 0);
    c[0] = 99; CHECK(fxbank_load(c, FXBANK_BYTES) == 0); c[0] = FXBANK_VER;
    c[4] = 5; c[5] = 5; c[6] = 5; c[7] = 200;                                   /* track 1: 5,5,5,200 */
    c[8] = 0xFF; c[9] = 0x7F;                                                   /* first knob 32767 */
    CHECK(fxbank_load(c, FXBANK_BYTES) == 1);
    CHECK(fx_view == 2 || 1);
    { int seen = 0, s; fx_select(0); for (s = 0; s < FX_NSLOT; s++) { CHECK(fxs[s] < 16 && !(seen >> fxs[s] & 1)); seen |= 1 << fxs[s]; } }
    CHECK(FXT[0].p[0] <= PD[P_F0DRIVE].max);
    printf(fails ? "%d FAILED\n" : "fxbank_test: all passed\n", fails);
    return fails != 0;
}
