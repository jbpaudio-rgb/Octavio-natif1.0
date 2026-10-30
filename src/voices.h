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
enum { PR_WAVE, PR_RNG, PR_TUNE, PR_PW, PR_BWAVE, PR_BRNG, PR_BTUNE, PR_BPW, PR_LA, PR_LB, PR_SW, PR_SO, PR_ST, PR_SP, PR_SL,
       PR_NTYPE, PR_NTONE, PR_NCRUSH, PR_NLEV, PR_CUT, PR_RES, PR_ENV, PR_PM, PR_ATK, PR_DEC, PR_SUS, PR_REL, PR_FATK,
       PR_FDEC, PR_FSUS, PR_FREL, PR_LVL, PR_COUNT };
/* drums: per voice v (0..5) the controls of the prototype, id = DR_x + v * DR_STRIDE... see drum_set() */
enum { DR_KIT, DR_TYPE, DR_KV, DR_TUNE, DR_DECAY, DR_TONE, DR_LEVEL, DR_ATK, DR_PEAK, DR_HOLD, DR_DRIVE, DR_FILT, DR_PER = 12 };
#define DR_ID(v, x) ((v) * DR_PER + (x))              /* parameter id of control x of voice v (DR_KIT = all voices' kit) */
#define DR_COUNT (6 * DR_PER)
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
#define ARP_STATE_N 17                              /* bytes of one track's arpeggiator position: nheld, held[8], i u16, cur u16, cnt i32 */
void voices_arp_state_get(int t, uint8_t *o);        /* the held notes (latch only), step, phase: for presets */
void voices_arp_state_set(int t, const uint8_t *b);
/* engines */
void moog_init(int t);
void moog_set(int t, int id, int v);
int  moog_get(int t, int id);
void moog_on(int t, int note, int vel15);
void moog_off(int t);
int  moog_active(int t);
void moog_render(int t, int32_t *out, int n);
void proph_init(int t);
void proph_set(int t, int id, int v);
int  proph_get(int t, int id);
void proph_on(int t, int note, int vel15);
void proph_off(int t);
int  proph_active(int t);
void proph_render(int t, int32_t *out, int n);
void drum_init(int t);
void drum_set(int t, int id, int v);
int  drum_get(int t, int id);
void drum_hit(int t, int v, int vel15);              /* voice 0..5 */
void drum_on(int t, int note, int vel15);            /* the prototype's noteOn: pitch class -> voice */
int  drum_active(int t);
void drum_render(int t, int32_t *out, int n);
