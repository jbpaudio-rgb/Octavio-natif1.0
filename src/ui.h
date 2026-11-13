/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include <stdint.h>
void ui_init(void);
void ui_encoder(int knob, int steps);       /* KNOB1..4 (0..3) */
/* the synth's modulation: 3 LFOs, 1 S&H and 4 sequencer lines (the other slots of the saved layout stay but are not reachable and do not run) */
#define LFO_NEXT(s) ((s) == 0 ? 1 : (s) == 1 ? 2 : (s) == 2 ? 4 : 0)      /* the sources in order: internal indexes 0, 1, 2 (LFO 1..3), 4 (S&H 1) */
#define LFO_LOG(s) ((s) == 4 ? 3 : (s) > 2 ? 0 : (s))
static const uint8_t LFO_PHYS[4] = { 0, 1, 2, 4 };
#define SEQ_LINES 4
int  ui_sel_tap(void);                         /* SEL tapped: 1 if the page used it (the VC3 patch matrix) */
void ui_button(int id);                      /* a button went down (BTN_*) */
void ui_tick(uint32_t ms);                   /* time: animation phase */
int  ui_scene(int *y0, int *y1);             /* what to redraw: rows [y0,y1) */
void ui_prepare(void);                       /* once per frame, before the bands */
void ui_draw(int y0, int y1);                /* the scene, into the current band (gfx_band) */
void ui_octave(int d);
int  ui_note_of_key(int key);                /* key 0..26 -> MIDI note with the octave */
/* the panel (fm1_input.h ids) */
enum { BTN_OCTM = 0, BTN_OCTP = 1, BTN_FX = 2, BTN_SEL = 3, BTN_ENV = 4, BTN_LFO = 5, BTN_EDIT = 6, BTN_GLO = 7,
       BTN_HOME = 8, BTN_SAVE = 9, BTN_ARP = 10, BTN_SEQ = 11, BTN_PLAY = 12, BTN_REC = 13 };
/* the pages of the prototype (ui_set_page: for the OP keys -> ARP2/ARP3, the HOME+EDIT / HOME+GLO chords -> MENU/MENU2/TEMPO, SEL -> FXO ...) */
enum { PG_GLO, PG_EDIT, PG_ENV, PG_LFO, PG_FX, PG_ARP, PG_ARP2, PG_ARP3, PG_TEMPO, PG_MENU, PG_MENU2, PG_HOME, PG_SEQ, PG_FXO,
       PG_OSC1, PG_OSC2, PG_OSC3, PG_OSC4, PG_ENV2, PG_GLO2, PG_DSEQ, PG_COUNT };       /* (the Minimoog / Prophet / drum pages: engine_pages.c) */
void ui_set_page(int page);
int  ui_get_page(void);
void ui_set_track(int t);                   /* the selected track 0..3 (HOME's red frame) */
int  ui_get_track(void);
void ui_peak(int t, int32_t pk_q15);         /* output peak of track t (0..32767, post volume not applied) for HOME's meters */
/* SEQ page: 6 lines x 16 steps (thousandths, -1000..1000), gate bits, selected line/step, the playing step */
void ui_seq_select(int line, int step);
int  ui_seq_line(void);
int  ui_seq_step(void);
void ui_seq_set(int line, int step, int v);
int  ui_seq_get(int line, int step);                 /* the selected track's */
int  ui_seq_get_t(int t, int line, int step);          /* track t's step values (the prototype's TRX[t].SQ) */
void ui_seq_set_t(int t, int line, int step, int v);
void ui_seq_gate_toggle(int step);
int  ui_seq_gate(int step);
int  ui_seq_note(int t, int s);                    /* the note-step page (synth tracks): the semitone offset of a step, its gates (bit s), a gate flipped */
void ui_seq_note_set(int t, int s, int v);
unsigned ui_seq_gates(int t);
void ui_seq_gate_flip(int t, int s);
void ui_seq_cur(int step);                   /* -1: stopped */
