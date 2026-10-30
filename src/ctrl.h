/* SPDX-License-Identifier: MPL-2.0 */
/* The panel's behaviour as the prototype has it: the buttons (press / hold / double-tap latch / chords), the side knobs
 * (SELECT, ALGORITHM, PRESETS), the preset bank with its name editor, and the transient overlays (preset list, toast).
 * Pure logic: no drawing, no hardware (overlay.c draws, main.c feeds the events). */
#pragma once
#include <stdint.h>

/* the prototype's `pg` names */
enum { PGN_GLO, PGN_EDIT, PGN_ENV, PGN_LFO, PGN_FX, PGN_FXO, PGN_ARP, PGN_SEQ, PGN_HOME, PGN_MENU, PGN_MENU2, PGN_TEMPO,
       PGN_OSC1, PGN_OSC2, PGN_OSC3, PGN_OSC4, PGN_ENV2, PGN_GLO2, PGN_DSEQ, PGN_COUNT };   /* OSC1..4, ENV2, GLO2: the Minimoog / Prophet / drum pages */
/* the prototype's buttons, in the order of its #bt row (ui.h BTN_*: the panel ids) */
enum { CB_FX, CB_SEL, CB_ENV, CB_LFO, CB_EDIT, CB_GLO, CB_HOME, CB_SAVE, CB_ARP, CB_SEQ, CB_PLAY, CB_REC, CB_COUNT };
enum { ROLE_SELECT, ROLE_ALGO, ROLE_PRESETS };      /* the left cluster's encoders */

#define NAME_LEN 12
#define BANK_MAX 48
#define BANK_POOL 30704u                            /* RAM image of the preset record = its flash capacity (persist.c: 8 sectors x 0xF00 - 16) */
#define SCENE_POOL 23024u                           /* same for the 16 key scenes */
#define CENG_N 4                                    /* the engines (voices.h ENG_*: Vactrol, Minimoog, Prophet, Drums) */

void ctrl_init(uint32_t ms);
void ctrl_button(int cb, int down, uint32_t ms);    /* a panel button went down / up (CB_*) */
void ctrl_octave(int d);
void ctrl_side(int role, int steps, uint32_t ms);   /* SELECT / ALGORITHM / PRESETS: detents, + = clockwise */
void ctrl_tick(uint32_t ms);                        /* timeouts: overlays, SAVE BANK */

/* state the pages read */
int  ctrl_page(void);                               /* PGN_* */
int  ctrl_src(void);                                /* the LFO source (SELECT knob, LFO pressed twice) 0..5 */
int  ctrl_track(void);                              /* the selected track (HOME: SELECT knob) 0..3 */
int  ctrl_fx_slot(void);                            /* FX pressed on FX: the slot 0..FX_NSLOT-1 */
int  ctrl_octave_val(void);
int  ctrl_drum_voice(void);                         /* the drum voice 0..5 the EDIT / ENV pages show (ENV / EDIT held + an OP key) */
void ctrl_cpu_full(uint32_t ms);                    /* toast: an effect did not fit the CPU budget */
int  ctrl_fx_key(int w, uint32_t ms);                 /* a white key (0..15) with FX held on the FX page: that effect into the slot; 1 = used */
int  ctrl_seq_key(int w, uint32_t ms);              /* a white key (0..15) on the SEQ (pitch) page: step select / set; 1 = used */
void ctrl_seq_release(int w, uint32_t ms);          /* ... and its release (tap or hold) */
void ctrl_fxo_knob(int steps);                      /* K1 on FX ORDER: move the selection (SEL held: move the effect) */
int  ctrl_arp_sub(void);                            /* the ARP page of a synth: 0 ARP, 1 ARP 2, 2 ARP 3 */
int  ctrl_dseq_key(int w);                          /* a white key (0..15) on the drum sequencer page: toggles that step of the selected lane; 1 = used */
void ctrl_op_release(int o);                        /* a black key was released (the held lane of the drum sequencer) */
int  ctrl_dseq_held(void);                          /* the lane held / latched with its OP key on the DSEQ page, -1 none */
int  ctrl_op_key(int o);                            /* a black key (0..) pressed: 1 when a held button made it a chord (no note) */
int  ctrl_playing(void), ctrl_rec(void);
uint32_t ctrl_lit(void);                            /* buttons whose LED is on: bit CB_* (current page, PLAY, REC, locks) */
uint32_t ctrl_locked(void);                         /* bit CB_*: latched by a double tap */
int  ctrl_bank_mode(void);                          /* HOME held or SAVE BANK waiting: the keys pick a bank */
int  ctrl_bank_mode_key(void);                      /* the same for a note key arriving in the audio ISR (counts the buttons physically down: ctrl_hw_held) */
void ctrl_hw_held(uint32_t cb_mask);                 /* the ISR: the buttons (bit CB_*) physically down now; the chords of the next note keys count them as held */
int  ctrl_bank_cur(void);                           /* the current bank, -1 none */
int  ctrl_bank_key(int w, uint32_t ms);             /* a white key (0..15) in bank mode: load (or SAVE BANK: save); 1 = used */

/* overlays */
enum { OV_NONE, OV_LIST, OV_NAME };
int  ctrl_overlay(void);                            /* OV_* (a toast shows over any) */
int  ctrl_list_rows(char rows[5][24], char tags[5][8], int cur[1], char *title, char *note);   /* the list overlay: rows shown */
int  ctrl_name(char *text, int *pos, char *engine_line);       /* the name editor: 12 chars, caret, "ENGINE: VACT [· OVERWRITE]" */
int  ctrl_toast(char *a, char *b);                  /* 1 while a toast shows */
uint32_t ctrl_version(void);                        /* bumps on every visible change: the screen redraws when it moves */

/* presets (engine values: PV[] of params.h) */
int  bank_count(void);
int  bank_current(void);                            /* 1-based, 0 = none */

/* the record images persist.c reads / writes (entries as state.c / ctrl.c lay them out; see NOTES-G.md) */
uint8_t *bank_pool(uint32_t *cap), *scene_pool(uint32_t *cap);
void bank_restore(uint32_t n), scene_restore(uint32_t n);      /* the pool now holds n bytes read from flash: check and index them */
void bank_restore_v1(const void *preset), scene_restore_v1(int slot, const int16_t *v);   /* the old Vactrol-only records (persist.c hands them over) */

/* hooks the rest of the firmware provides */
void ui_goto(int pgn);                              /* ui.c: show page PGN_* (the ones it has) */
void ui_params_changed(void);
void ui_lfo_sync_toggle(void);                      /* ui.c: the selected LFO's tempo sync switch */
