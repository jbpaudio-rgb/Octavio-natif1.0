/* SPDX-License-Identifier: MPL-2.0 */
/* Voice layer of the prototype: 4 tracks, one monophonic engine each (Vactrol, Minimoog, Prophet, drums), the keys
 * play the selected track (kOnT / kOffT), an arpeggiator per track in front of the engine, and the master mix
 * tanh(sum(track) * .8 * 1.5 * .7). Parameter values are thousandths (selects: indices), like params.h. */
#pragma once
#include <stdint.h>
enum { ENG_VAC, ENG_MOOG, ENG_PROPH, ENG_DRUM, ENG_N };
enum { MG_W1, MG_R1, MG_T1, MG_P1, MG_L1, MG_W2, MG_R2, MG_T2, MG_P2, MG_L2, MG_W3, MG_R3, MG_T3, MG_P3, MG_L3,
       MG_NTYPE, MG_NTONE, MG_NCRUSH, MG_NLEV, MG_CUT, MG_RES, MG_ENV, MG_GLIDE, MG_REL, MG_ATK, MG_DEC, MG_SUS,
       MG_FATK, MG_FDEC, MG_FSUS, MG_LVL, MG_COUNT };
enum { ARP_MODE, ARP_OCT, ARP_RATE, ARP_GATE, ARP_SW, ARP_LATCH, ARP_ACC, ARP_CHANCE, ARP_TR, ARP_REP, ARP_HUM, ARP_PAT,
       ARP_COUNT };
void voices_init(void);
void voices_keys(uint32_t notes);                    /* the 27 key bits, every audio block: edges become kOnT / kOffT */
void voices_note(int t, int note, int on);           /* kOnT / kOffT of a track directly (MIDI, sequencer) */
void voices_render(int32_t *out, int n);             /* Q15 mono: engines -> mix -> tanh */
void voices_select(int t);
int  voices_sel(void);
void voices_set_engine(int t, int eng);
int  voices_engine(int t);
void voices_param(int t, int eng, int id, int v);    /* engine parameter (MG_*, PR_*, DR_*) of a track */
int  voices_param_get(int t, int eng, int id);
void voices_arp(int t, int id, int v);               /* ARP_* */
int  voices_arp_get(int t, int id);
void voices_bpm(int bpm);
void voices_track_level(int t, int v1000);           /* the track volume (prototype: .8) */
uint32_t voices_gate_mask(void);                     /* bit t: track t is sounding (for the LEDs) */
int  voices_arp_note(int t);                        /* the note the arpeggiator is playing now (-1: none) */
uint32_t voices_held_keys(int t, int base);          /* bit k: note base+k is held (keys / arp latch), k = 0..26 */
void voices_arp_pos(int t, int *cur, int *len);
/* engines */
void moog_init(int t);
void moog_set(int t, int id, int v);
int  moog_get(int t, int id);
void moog_on(int t, int note, int vel15);
void moog_off(int t);
int  moog_active(int t);
void moog_render(int t, int32_t *out, int n);
