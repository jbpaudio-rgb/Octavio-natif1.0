/* SPDX-License-Identifier: MPL-2.0 */
/* The prototype's modulation: LFO 1..4 + S&H 1..2 (the LFO page's six sources, native order LFO1, LFO2, LFO3, LFO4, S&H1, S&H2 =
 * prototype keys m0, m1, m2, m10, m3, m11) and the matrix that sends each to one destination (BuchlaEngine.mod()).
 * Included from main.c after fxdsp.c (it uses fx_exp2, rnd32, INC_MHZ, PVI, LFO_BASE, bpm_v of the same unit).
 *
 * Per audio block (128 frames, the prototype steps 32): the sources advance, then for each source with a destination and an amount
 * the destination's knob position n (0..1, as the knob shows it) becomes clamp(n + source * amount^2); this is the prototype's
 * modv(): a linear or log range is just the knob's own scale.  The modulated values are written into PV (and the arpeggiator's
 * table) for the length of the block's rendering and put back afterwards: the IRQ is atomic with respect to the main loop, so the
 * pages and presets only ever see the base values.
 *
 * Destinations (OPT_DST, the Vactrol list): 1 Fold 2 Sym 3 Freq 4 FM env 5 FM ratio 6 FM amount 7 Env attack 8 Env decay 9 Vact decay
 * 10 Cutoff, 12..16 arp gate / swing / accent / chance / humanize, 17..68 the 52 effect knobs (FXD order).  11 "Level" (the
 * prototype's per-engine level) is not routed: native has no per-engine level knob (track volume is separate).
 * The sequencer lanes (m4..m9) are not sources here: they need the sequencer's step values. */

enum { MOD_N = 6, MOD_SAVE = 8 };
static struct { uint32_t ph; int32_t shv, shs; } MS_[MOD_N] = { { 0, 0, 0 }, { 1417339207u, 0, 0 }, { 2834678415u, 0, 0 }, { 644245094u, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } };   /* the prototype's start phases .33 / .66 / .15 */          /* LFO phase (turns, Q32); S&H: held value and smoothed value (Q15) */
static struct { int16_t id; int16_t base; uint8_t arp; } mod_sv[MOD_SAVE];
static int mod_nsv, mod_vac_dirty;
static const uint8_t MOD_DSTID[17] = { 255, P_FOLD, P_SYM, P_FREQ, P_FENV, P_RATIO, P_FM, P_ATK, P_EDEC, P_DECAY, P_OPEN, 255,
                                       P_ARPGATE, P_ARPSW, P_ARPACC, P_ARPCHANCE, P_ARPHUM };
static const uint8_t MOD_ARPID[17] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, ARP_GATE, ARP_SW, ARP_ACC, ARP_CHANCE, ARP_HUM };
uint32_t mod_ticks_max, mod_t0;                           /* CPU: worst mod_apply + mod_restore (timer ticks) */

static int32_t lfo_wf(uint32_t ph, int shape)             /* wf(): sine / triangle / saw / square, Q15 */
{
    switch (shape) {
    case 0: return sin_q15(ph);
    case 1: { int32_t d = (int32_t)(ph - 0x80000000u); if (d < 0) d = -d; return 32768 - (d >> 15); }
    case 2: return (int32_t)(ph >> 16) - 32768;
    default: return ph < 0x80000000u ? 32767 : -32768;
    }
}
static uint32_t lfo_mhz(int src)                          /* the source's rate, milli-Hz: sync = bpm / 60 * DIV[floor(rate * 8)], else .05 * 800^rate Hz */
{
    int b = LFO_BASE[src], v = PV[b + 2];
    if (PV[b + 4]) {
        static const uint16_t DIVQ[8] = { 64, 128, 256, 512, 1024, 2048, 4096, 8192 };      /* DIV * 1024 */
        int di = v * 8 / 1000;
        return (uint32_t)bpm_v * DIVQ[di > 7 ? 7 : di] * 25u / 1536u;
    }
    return (50u * fx_exp2_24(DIV1000((int64_t)v * 632020 * 256), 8)) >> 8;            /* 50 mHz * 800^rate */
}
static int mod_set(int id, int arp, int16_t newv)         /* write a modulated value, remembering the base once */
{
    int k;
    for (k = 0; k < mod_nsv; k++) if (mod_sv[k].id == id && mod_sv[k].arp == arp) break;
    if (k == mod_nsv) {
        if (mod_nsv >= MOD_SAVE) return 0;
        mod_sv[k].id = (int16_t)id; mod_sv[k].arp = (uint8_t)arp;
        mod_sv[k].base = arp ? (int16_t)voices_arp_get(voices_sel(), id) : PV[id];
        mod_nsv++;
    }
    if (arp) voices_arp(voices_sel(), id, newv); else PV[id] = newv;
    return 1;
}

#ifdef MOD_TEST                                         /* test builds: -DMOD_TEST="{src,dst,shape,rate,amount,sync,smooth}" -DMOD_ID=<param id>; the right channel carries PV[MOD_ID] */
int32_t mod_tap;
static void mod_test_init(void)
{
    static const int16_t t[] = MOD_TEST;
    int b = LFO_BASE[t[0]];
    PV[b] = t[1]; PV[b + 1] = t[t[0] >= 4 ? 6 : 2]; PV[b + 2] = t[3]; PV[b + 3] = t[4]; PV[b + 4] = t[5];
}
#endif
void mod_apply(int n)
{
    int s, vac = 0;
#ifdef MOD_TEST
    static uint8_t once;
    if (!once) { once = 1; mod_test_init(); }
#endif
    mod_t0 = fm1_ticks();
    mod_nsv = 0;
    for (s = 0; s < MOD_N; s++) {
        int b = LFO_BASE[s], dst = PV[b], x, id, k;
        int32_t src, amt, xq, lo, hi, nn;
        uint32_t inc = INC_MHZ(lfo_mhz(s)) , ph0 = MS_[s].ph;
        MS_[s].ph += inc * (uint32_t)n;
        if (s < 4) src = lfo_wf(MS_[s].ph, PV[b + 1]);
        else {                                                         /* sample and hold: a new value each time the phase wraps (the first block too) */
            if (MS_[s].ph < ph0 || (!ph0 && !MS_[s].shv)) MS_[s].shv = (int32_t)(rnd32() >> 16) - 32768;
            {
                uint32_t xq16 = 1902220u / (uint32_t)(20 + 4 * PV[b + 1]);          /* dt / (.002 + smooth * .4), dt = 128 / 44100 */
                MS_[s].shs += (int32_t)(((int64_t)(MS_[s].shv - MS_[s].shs) * c_expm(xq16 * (uint32_t)n / 128u)) >> 16);
            }
            src = MS_[s].shs;
        }
        if (dst <= 0 || dst > 68 || !PV[b + 3]) continue;
        amt = (PV[b + 3] * PV[b + 3] * 2147) >> 16;                    /* amount^2, Q15 */
        xq = (int32_t)(((int64_t)src * amt) >> 15);
        if (dst >= 17) { id = P_F0DRIVE + (dst - 17); x = 0; }
        else if (dst >= 12) { id = MOD_ARPID[dst]; x = 1; }
        else { id = MOD_DSTID[dst]; x = 0; if (id == 255) continue; vac = 1; }
        {
            const pdesc_t *d = x ? 0 : &PD[id];
            int cur, mn = x ? 0 : d->min, mx = x ? 1000 : d->max;
            cur = x ? voices_arp_get(voices_sel(), id) : PV[id];
            if (mx == mn) continue;
            lo = 0; hi = 32768;
            nn = (int32_t)(((int64_t)(cur - mn) << 15) / (mx - mn)) + xq;
            nn = nn < lo ? lo : nn > hi ? hi : nn;
            k = mn + (int)(((int64_t)nn * (mx - mn)) >> 15);
            mod_set(id, x, (int16_t)k);
        }
    }
    if (vac || mod_vac_dirty) { vac_update(); mod_vac_dirty = 0; }
#ifdef MOD_TEST
    mod_tap = PV[MOD_ID];
#endif
}
void mod_restore(void)
{
    int k, vac = 0;
    for (k = mod_nsv - 1; k >= 0; k--) {
        if (mod_sv[k].arp) voices_arp(voices_sel(), mod_sv[k].id, mod_sv[k].base);
        else { PV[mod_sv[k].id] = mod_sv[k].base; if (mod_sv[k].id < P_F0DRIVE) vac = 1; }
    }
    mod_nsv = 0;
    if (vac) mod_vac_dirty = 1;                       /* the derived values stay modulated until the next block's mod_apply recomputes them (once per block) */
    {
        uint32_t d = fm1_ticks() - mod_t0;
        if (d > mod_ticks_max) mod_ticks_max = d;
    }
}
