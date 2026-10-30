/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include <stdint.h>
/* A parameter is one control of the prototype: a range slider (value in thousandths, snapped to its step, exactly
 * like an <input type=range>), a select (index) or a checkbox. The knobs move them in 1/50 of their range. */
enum { PK_RANGE, PK_SELECT, PK_CHECK };
typedef struct {
    const char *label;          /* the long label of the prototype */
    uint8_t kind, nopt, fmt;    /* fmt: how the output element prints it (FMT_*) */
    int16_t min, max, step, def;/* thousandths (select: indices) */
    const char *const *opts;
} pdesc_t;
enum { FMT_PCT, FMT_RATIO, FMT_FREQ, FMT_MS, FMT_TEXT, FMT_CUT, FMT_RATE, FMT_INT, FMT_STEP };
enum { P_FOLD, P_SYM, P_DECAY, P_WAVE, P_FREQ, P_MWAVE, P_RATIO, P_FM, P_FENV, P_EAD, P_ATK, P_EDEC, P_OPEN, P_MODE,
       #include "pext_ids.h"            /* page parameters (tools/gen_pext.py) */
       P_COUNT };
extern const pdesc_t PD[P_COUNT];
extern int16_t PV[P_COUNT];                 /* the current values */
void param_reset(void);
int32_t param_n_q16(int id);                /* normalised position 0..1 (Q16), as the knob shows it */
void param_set_n(int id, int32_t n_q16);    /* setn(): value = min + n * range, snapped to the step */
void param_text(int id, char *out);         /* what the LCD prints for it */

/* engine (vactrol.c) */
void vac_init(void);
void vac_update(void);                      /* after any parameter change */
void vac_note_on(int note);
void vac_note_off(void);
void vac_render(int32_t *out, int n);       /* Q15 mono */
int32_t vac_wave_q15(uint32_t ph, int t);
int32_t vac_scope(uint32_t u, int folded, int32_t *carrier);   /* the scope's f(x): FM osc or folded, Q15 */
