/* SPDX-License-Identifier: MPL-2.0 */
/* octavio-J worst-case load test (NOTES-J).  Built only with -DLOADTEST (tools/loadtest.py does it); the normal firmware does not contain it.
 * lt_setup() runs once in app_main, after persist_boot() and before the audio starts, and puts the whole machine in a chosen heavy state:
 *   - the four tracks' engines (LT_ENG), each with an arpeggiator on (latched 8-note chord, 4 octaves, 32nd-note-ish rate, legato) AND the
 *     step clock running (PLAY): the synth tracks' gate steps all on, the drum tracks' six lanes full on 16 steps with long decays;
 *   - the four tracks' effect chains (LT_FX) with Mix 1000 and the knobs at BENCH_KNOB (as NOTES-H's bench);
 *   - the six modulation sources of every track (4 LFO + 2 S&H, fast, deep) routed: 2 engine parameters, then the knobs of the chain's effects
 *     (first knob of slots 1 and 3, second knob of slots 2 and 4); drums have no FX destinations in the prototype, so six drum parameters.
 * Knobs defaults: LT_FXMASK (bit s: slot s has Mix > 0, default 15), LT_NOMOD (no routes), LT_BPM (default 180), LT_NOSEQ (no step clock), LT_NOARP.
 * Everything else (pages, screen, keys, encoders, presets) is the real firmware, driven by the emulator's --keys script. */
#ifdef LOADTEST
#ifndef LT_ENG
#define LT_ENG { ENG_VAC, ENG_MOOG, ENG_PROPH, ENG_DRUM }
#endif
#ifndef LT_FX                                       /* effect ids in FXD order: 5 Delay, 6 Reverb, 7 Granular, 12 Reverse, 13 Freeze, 14 Tape, 15 Shimmer */
#define LT_FX { { 7, 6, 15, 13 }, { 6, 15, 14, 5 }, { 7, 14, 12, 5 }, { 6, 15, 13, 14 } }
#endif
#ifndef LT_FXMASK
#define LT_FXMASK 15
#endif
#ifndef LT_BPM
#define LT_BPM 180
#endif

static void lt_setup(void)
{
    static const uint8_t ENG[4] = LT_ENG;
    static const uint8_t ord[4][FX_NSLOT] = LT_FX;
    static const uint8_t SDST[3][2] = { { 1, 3 }, { 81, 78 }, { 98, 88 } };            /* engine parameters: Vactrol fold / freq, Moog cutoff / osc 1 pitch, Prophet cutoff / wave */
    static const uint8_t DDST[6] = { 104, 118, 132, 151, 163, 170 };                    /* drums: tune v0, decay v1, tone v2, filter v3, drive v4, decay v5 */
    int t, s, k, routes = 0, i;
    for (t = 0; t < 4; t++) {
        int e = ENG[t];
        voices_set_engine(t, e);
        for (s = 0; s < FX_NSLOT; s++) {                                              /* effect chain and knobs */
            int f = ord[t][s];
            FXT[t].ord[s] = (uint8_t)f;
            for (k = 0; k < FX_P[f].n; k++) {
                int id = FX_FIRST[f] + k - P_F0DRIVE;
                FXT[t].p[id] = (FX_P[f].l[k] == L_MIX || FX_P[f].l[k] == L_HOLD) ? ((LT_FXMASK >> s & 1) ? BENCH_MIX : 0) : BENCH_KNOB;
            }
        }
    }
    for (t = 0; t < 4; t++) {                                                          /* the mod matrix */
        int e = ENG[t];
#ifndef LT_NOMOD
        for (s = 0; s < MOD_N; s++) {
            int b = LFO_BASE[s] - P_M0T, dst = 0;
            if (e == ENG_DRUM) dst = DDST[s];
            else if (s < 2) dst = SDST[e == ENG_VAC ? 0 : e == ENG_MOOG ? 1 : 2][s];
            else {
                int f = ord[t][s - 2], j = FX_P[f].n > 2 && (s & 1) ? 1 : 0;           /* slots 1..4 of the chain */
                dst = 17 + (FX_FIRST[f] - P_F0DRIVE) + j;
            }
            if (dst_pos(e, 0, dst) < 0) continue;
            MODT[t].m[b] = (int16_t)dst;
            MODT[t].m[b + 1] = s < 4 ? (int16_t)((s + t) & 3) : 200;                  /* shape / smoothing */
            MODT[t].m[b + 2] = (int16_t)(550 + 60 * s);                               /* rate: a few Hz .. tens of Hz */
            MODT[t].m[b + 3] = (int16_t)(750 + 30 * s);                               /* amount */
            MODT[t].m[b + 4] = 0;
            routes++;
        }
#endif
        SQG[t] = 0xFFFFu;                                                              /* the synth step gates, every 16th */
        if (e == ENG_DRUM) {
            int v;
            drum_set(t, DR_ID(0, DR_KIT), t % 3);
            for (v = 0; v < 6; v++) {
                drum_gates_set(t, v, 0xFFFFu);
                drum_set(t, DR_ID(v, DR_DECAY), 900);                                   /* long decays: the six voices overlap */
                drum_set(t, DR_ID(v, DR_CHANCE), 1000);
                drum_set(t, DR_ID(v, DR_LEVEL), 700);
            }
        } else {
#ifndef LT_NOARP
            voices_arp(t, ARP_MODE, 3);                                                /* up-down */
            voices_arp(t, ARP_OCT, 4);
            voices_arp(t, ARP_RATE, 5);
            voices_arp(t, ARP_GATE, 990);                                              /* legato: the engine never rests */
            voices_arp(t, ARP_LATCH, 1);
            for (i = 0; i < 8; i++) voices_note(t, 48 + 2 * i + t, 1);
#endif
        }
    }
    mod_load_view();                                                                   /* the selected track (0)'s matrix and chain into PV[] */
    fx_load_view();
    PV[P_F5SYNC] = FXT[0].p[FX_NP - 1];
    PV[P_BPM] = LT_BPM;
    perf.lt[0] = (uint32_t)routes; perf.lt[1] = LT_BPM;
    for (t = 0; t < 4; t++) perf.lt[2] |= (uint32_t)ENG[t] << (4 * t);
    fx_check();
#ifndef LT_NOSEQ
    voices_play(1);
#endif
}
#endif
