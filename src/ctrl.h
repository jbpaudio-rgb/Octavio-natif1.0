/* SPDX-License-Identifier: MPL-2.0 */
/* The panel's behaviour as the prototype has it: the buttons (press / hold / double-tap latch / chords), the side knobs
 * (SELECT, ALGORITHM, PRESETS), the preset bank with its name editor, and the transient overlays (preset list, toast).
 * Pure logic: no drawing, no hardware (overlay.c draws, main.c feeds the events). */
#pragma once
#include <stdint.h>

/* the prototype's `pg` names */
enum { PGN_GLO, PGN_EDIT, PGN_ENV, PGN_LFO, PGN_FX, PGN_FXO, PGN_ARP, PGN_SEQ, PGN_HOME, PGN_MENU, PGN_MENU2, PGN_TEMPO, PGN_COUNT };
/* the prototype's buttons, in the order of its #bt row (ui.h BTN_*: the panel ids) */
enum { CB_FX, CB_SEL, CB_ENV, CB_LFO, CB_EDIT, CB_GLO, CB_HOME, CB_SAVE, CB_ARP, CB_SEQ, CB_PLAY, CB_REC, CB_COUNT };
enum { ROLE_SELECT, ROLE_ALGO, ROLE_PRESETS };      /* the left cluster's encoders */

#define NAME_LEN 12
#define BANK_MAX 48
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
int  ctrl_fx_slot(void);                            /* FX pressed on FX: the slot 0..5 */
int  ctrl_octave_val(void);
int  ctrl_playing(void), ctrl_rec(void);
uint32_t ctrl_lit(void);                            /* buttons whose LED is on: bit CB_* (current page, PLAY, REC, locks) */
uint32_t ctrl_locked(void);                         /* bit CB_*: latched by a double tap */
int  ctrl_bank_mode(void);                          /* HOME held or SAVE BANK waiting: the keys pick a bank */
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

/* hooks the rest of the firmware provides */
void ui_goto(int pgn);                              /* ui.c: show page PGN_* (the ones it has) */
void ui_params_changed(void);                       /* main.c: re-run vac_update() */
