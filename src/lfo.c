/* SPDX-License-Identifier: MPL-2.0 */
/* The prototype's modulation: LFO 1..4 + S&H 1..2 (the LFO page's six sources, native order LFO1, LFO2, LFO3, LFO4, S&H1, S&H2 =
 * prototype keys m0, m1, m2, m10, m3, m11) and the matrix that sends each to one destination (BuchlaEngine.mod()).
 * Included from main.c after fxdsp.c (it uses fx_exp2, rnd32, INC_MHZ, LFO_BASE, bpm_v of the same unit).
 *
 * Per audio block (128 frames, the prototype steps 32): the sources advance, then for each source with a destination and an amount
 * the destination's knob position n (0..1, as the knob shows it) becomes clamp(n + source * amount^2); this is the prototype's
 * modv(): a linear or log range is just the knob's own scale.  The modulated values are written into PV (and the arpeggiator's
 * table) for the length of the block's rendering and put back afterwards: the IRQ is atomic with respect to the main loop, so the
 * pages and presets only ever see the base values.
 *
 * Destinations (dst.c, pext_gen.h): each track has its own matrix (PV[] for the selected one, MODT[t] for the others) and acts on its own
 * engine: a destination is an entry of the master list, valid for the engines whose group bit it carries (DSTM[].grp): Vactrol
 * parameters (PV), the engines' own parameters (voices_param: Minimoog / Prophet / the 6 drum voices incl. their lane settings), the
 * envelope times, the arpeggiator (12..16), the 52 effect knobs of the track (fxdsp.c) and Level (a gain on the track's engine).
 * The sequencer lines (m4..m9) are sources while the step clock runs (the line's step value x amount; destinations master + 3, the pitch ones are used by
 * the synth sequencer when it plays a note, voices.c). "Seq gate" (DK_SQG, not in the prototype) offsets the sequenced notes' gate length. */

enum { MOD_N = 6, MOD_SAVE = 32 };
static struct { uint32_t ph; int32_t shv, shs; } MS_[4][MOD_N];       /* per track and source: LFO phase (turns, Q32); S&H: held value and smoothed value (Q15) */
static struct { uint8_t kind, t, eng; int16_t id, base; } mod_sv[MOD_SAVE];      /* kind 0 PV, 1 arpeggiator table, 2 engine parameter */
static int mod_nsv, mod_vac_dirty, mod_ready;
uint32_t mod_t0;                           /* CPU: worst mod_apply + mod_restore (timer ticks) */

static int32_t lfo_wf(uint32_t ph, int shape)             /* wf(): sine / triangle / saw / square, Q15 */
{
    switch (shape) {
    case 0: return sin_q15(ph);
    case 1: { int32_t d = (int32_t)(ph - 0x80000000u); if (d < 0) d = -d; return 32768 - (d >> 15); }
    case 2: return (int32_t)(ph >> 16) - 32768;
    default: return ph < 0x80000000u ? 32767 : -32768;
    }
}
static uint32_t lfo_mhz(const int16_t *M, int b)          /* the source's rate, milli-Hz: sync = bpm / 60 * DIV[floor(rate * 8)], else .05 * 800^rate Hz */
{
    int v = M[b + 2];
    if (M[b + 4]) {
        static const uint16_t DIVQ[8] = { 64, 128, 256, 512, 1024, 2048, 4096, 8192 };      /* DIV * 1024 */
        int di = v * 8 / 1000;
        return (uint32_t)bpm_v * DIVQ[di > 7 ? 7 : di] * 25u / 1536u;
    }
    return (50u * fx_exp2_24(DIV1000((int64_t)v * 632020 * 256), 8)) >> 8;            /* 50 mHz * 800^rate */
}
static int mod_cur(int kind, int t, int eng, int id)
{
    return kind == 0 ? PV[id] : kind == 1 ? voices_arp_get(t, id) : voices_param_get(t, eng, id);
}
static void mod_put(int kind, int t, int eng, int id, int v)
{
    if (kind == 0) PV[id] = (int16_t)v; else if (kind == 1) voices_arp(t, id, v); else voices_param(t, eng, id, v);
}
static void mod_set(int kind, int t, int eng, int id, int newv)         /* write a modulated value, remembering the base once */
{
    int k;
    for (k = 0; k < mod_nsv; k++) if (mod_sv[k].kind == kind && mod_sv[k].t == t && mod_sv[k].id == id) break;
    if (k == mod_nsv) {
        if (mod_nsv >= MOD_SAVE) return;
        mod_sv[k].kind = (uint8_t)kind; mod_sv[k].t = (uint8_t)t; mod_sv[k].eng = (uint8_t)eng; mod_sv[k].id = (int16_t)id;
        mod_sv[k].base = (int16_t)mod_cur(kind, t, eng, id);
        mod_nsv++;
    }
    mod_put(kind, t, eng, id, newv);
}
/* a destination of the master list on the engine of track t -> what to modulate; 0: nothing (not this engine's, or a no-op as in the prototype) */
static int mod_target(int t, int eng, int d, int *kind, int *id)
{
    if (d <= 0 || d >= N_DST || !((DSTM[d].grp >> eng) & 1)) return 0;
    switch (DSTM[d].kind) {
    case DK_PV: if (eng != ENG_VAC || t != voices_vac_owner()) return 0; *kind = 0; *id = DSTM[d].a; return 1;
    case DK_ARP: *kind = 1; *id = DSTM[d].a; return 1;
    case DK_ENG: case DK_DRUM: *kind = 2; *id = DSTM[d].a; return 1;
    case DK_ENV: {                                                  /* atk, edec, sus, rel: each engine's own envelope */
        static const uint8_t MGE[4] = { MG_ATK, MG_DEC, MG_SUS, MG_REL }, PRE[4] = { PR_ATK, PR_DEC, PR_SUS, PR_REL };
        int e = DSTM[d].a;
        if (eng == ENG_VAC) { if (e > 1) return 0; if (t != voices_vac_owner()) return 0; *kind = 0; *id = e ? P_EDEC : P_ATK; return 1; }
        if (eng == ENG_MOOG && e == 3) return 0;                    /* the prototype's Minimoog reads mgrel (a switch), not rel: "Env release" does nothing there */
        *kind = 2; *id = eng == ENG_MOOG ? MGE[e] : PRE[e]; return 1;
    }
    }
    return 0;
}

static void mod_dest(int t, int eng, int dst, int32_t xq, int32_t *lvl, int32_t *sqg, int *vacp)     /* one source value (Q15) onto destination dst of track t */
{
    int id, k, kind;
    int32_t nn, cur, mn, mx;
    if (DSTM[dst].kind == DK_FX) { if ((DSTM[dst].grp >> eng) & 1) fx_mod_t(t, DSTM[dst].a, xq); return; }
            if (DSTM[dst].kind == DK_SQG) { if ((DSTM[dst].grp >> eng) & 1) sqg[t] += xq; return; }
    if (DSTM[dst].kind == DK_LVL) { lvl[t] = (int32_t)(((int64_t)lvl[t] * (xq + 32768 < 0 ? 0 : xq + 32768 > 32768 ? 32768 : xq + 32768)) >> 15); return; }
            if (!mod_target(t, eng, dst, &kind, &id)) return;
            if (kind == 0) { mn = PD[id].min; mx = PD[id].max; *vacp = 1; }
            else if (kind == 1) { mn = id == ARP_GATE ? 50 : 0; mx = 1000; }          /* the prototype's MP: the arpeggiator gate runs .05 .. 1 */
            else { mn = 0; mx = 1000; }
            cur = mod_cur(kind, t, eng, id);
            if (mx == mn) return;
            if (kind == 0 && id == P_FM) {                           /* the prototype's fm is knob^2 * 2 and its range [0, 2] is linear in that: modulate the square, not the knob */
                nn = (int32_t)(((int64_t)cur * cur << 15) / 1000000) + xq;
                nn = nn < 0 ? 0 : nn > 32768 ? 32768 : nn;
                k = nn >= 32768 ? 1000 : (int)((isqrt32((uint32_t)nn << 17) * 1000u) >> 16);
            } else {
                nn = (int32_t)(((int64_t)(cur - mn) << 15) / (mx - mn)) + xq;
                nn = nn < 0 ? 0 : nn > 32768 ? 32768 : nn;
                k = (int)(mn + (((int64_t)nn * (mx - mn)) >> 15));
            }
            mod_set(kind, t, eng, id, k);
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
    int s, t, vac = 0;
    int32_t lvl[4], sqg[4];
#ifdef MOD_TEST
    static uint8_t once;
    if (!once) { once = 1; mod_test_init(); }
#endif
    if (!mod_ready) {                                                /* the prototype's start phases .33 / .66 / .15 */
        static const uint32_t P0[MOD_N] = { 0, 1417339207u, 2834678415u, 644245094u, 0, 0 };
        for (t = 0; t < 4; t++) for (s = 0; s < MOD_N; s++) MS_[t][s].ph = P0[s];
        mod_ready = 1;
    }
    mod_t0 = fm1_ticks();
    mod_nsv = 0;
    for (t = 0; t < 4; t++) { lvl[t] = 32768; sqg[t] = 0; }
    for (t = 0; t < 4; t++) {                                        /* every track has its own matrix and acts on its own engine */
        const int16_t *M = t == mod_view ? &PV[P_M0T] : MODT[t].m;
        int eng = voices_engine(t);
        for (s = 0; s < MOD_N; s++) {
            int b = 5 * s, dst = M[b], x, id, k, kind;
            int32_t src, amt, xq, nn, cur, mn, mx;
            uint32_t ph0;
            if (dst <= 0 || dst >= N_DST || !M[b + 3]) continue;
            ph0 = MS_[t][s].ph;
            MS_[t][s].ph += INC_MHZ(lfo_mhz(M, b)) * (uint32_t)n;
            if (s < 4) src = lfo_wf(MS_[t][s].ph, M[b + 1]);
            else {                                                  /* sample and hold: a new value each time the phase wraps (the first block too) */
                uint32_t xq16 = 1902220u / (uint32_t)(20 + 4 * M[b + 1]);          /* dt / (.002 + smooth * .4), dt = 128 / 44100 */
                if (MS_[t][s].ph < ph0 || (!ph0 && !MS_[t][s].shv)) MS_[t][s].shv = (int32_t)(rnd32() >> 16) - 32768;
                MS_[t][s].shs += (int32_t)(((int64_t)(MS_[t][s].shv - MS_[t][s].shs) * c_expm(xq16 * (uint32_t)n / 128u)) >> 16);
                src = MS_[t][s].shs;
            }
            amt = (M[b + 3] * M[b + 3] * 2147) >> 16;               /* amount^2, Q15 */
            xq = (int32_t)(((int64_t)src * amt) >> 15);
            mod_dest(t, eng, dst, xq, lvl, sqg, &vac);
        }
        if (voices_seq_cur() >= 0) {                                 /* the sequencer lines (destination master + 3): the step value times the amount */
            const int16_t *QL = t == mod_view ? &PV[P_M4T] : MODT[t].q;
            for (s = 0; s < 6; s++) {
                int d = QL[4 * s] - 3;
                if (d <= 0 || !QL[4 * s + 2]) continue;
                mod_dest(t, eng, d, (int32_t)(((int64_t)ui_seq_get(s, voices_seq_cur()) * 32768 / 1000) * QL[4 * s + 2] / 1000), lvl, sqg, &vac);
            }
        }
    }
    for (t = 0; t < 4; t++) { voices_lvl_mod(t, lvl[t]); voices_sqg_mod(t, sqg[t]); }
    if (vac || mod_vac_dirty) { vac_update(); mod_vac_dirty = 0; }
#ifdef MOD_TEST
    mod_tap = MOD_ID >= P_F0DRIVE && MOD_ID <= P_F15MIX ? FEFF[fx_view][MOD_ID - P_F0DRIVE] : PV[MOD_ID];
#endif
}
void mod_restore(void)
{
    int k, vac = 0;
    for (k = mod_nsv - 1; k >= 0; k--) {
        mod_put(mod_sv[k].kind, mod_sv[k].t, mod_sv[k].eng, mod_sv[k].id, mod_sv[k].base);
        if (mod_sv[k].kind == 0) vac = 1;
    }
    mod_nsv = 0;
    if (vac) mod_vac_dirty = 1;                       /* the derived values stay modulated until the next block's mod_apply recomputes them (once per block) */
    {
        uint32_t d = fm1_ticks() - mod_t0;
        if (d > mod_ticks_max) mod_ticks_max = d;
    }
}

#ifdef BENCH                                            /* worst-case bench (NOTES-H): all six mod sources routed, the four tracks play and retrigger */
void bench_tick(void)
{
    static uint32_t n;
    static const int16_t M[6][4] = { { 1, 0, 600, 900 }, { 3, 1, 450, 800 }, { 10, 2, 700, 700 }, { 0, 0, 300, 900 }, { 0, 0, 500, 700 }, { 5, 0, 800, 600 } };
    int t, s;
    if (!n) {
        for (s = 0; s < 6; s++) {
            int b = LFO_BASE[s], dst = M[s][0];
            if (s == 3) dst = 17 + (P_F6SIZE - P_F0DRIVE);              /* LFO 4 -> reverb size on every track */
            if (s == 4) dst = 17 + (P_F1RATE - P_F0DRIVE);              /* S&H 1 -> phaser rate */
            PV[b] = (int16_t)dst; PV[b + 1] = M[s][1]; PV[b + 2] = M[s][2]; PV[b + 3] = M[s][3]; PV[b + 4] = 0;
        }
        for (t = 0; t < 4; t++) voices_note(t, 53 + 3 * t, 1);
    }
    if ((n++ & 127) == 64) for (t = 0; t < 4; t++) voices_note(t, 53 + 3 * t + (int)((n >> 7) & 3), 1);
}
#endif
