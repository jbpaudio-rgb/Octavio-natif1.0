/* SPDX-License-Identifier: MPL-2.0 */
/* The whole firmware as a WebAssembly module (clang --target=wasm32): the same sources as the device, with the hardware layer
 * replaced by a few exported functions the web page calls: buttons, encoders, note keys, one audio block, one screen frame.
 * Presets live in an in-memory flash (they are lost when the page closes). Not the real chip: no CPU-time measure (fm1_ticks = 0,
 * so the CPU governor never limits anything here) and 44.1 kHz audio. build: tools/webbuild.sh */
#include <stdint.h>
#include "wlibc.h"
#define PERSIST_FLASH 1
#include "../src/octavio.h"
#include "../src/params.h"
#include "../src/ui.h"
static inline uint32_t fm1_ticks(void) { return 0; }
static volatile uint32_t ms;
static struct { uint32_t notes, buttons; } fm1_in;
#include "../src/mathfx.c"
#include "../src/tables_gen.c"
#include "../src/assets_gen.c"
#include "../src/gfx.c"
#include "../src/params.c"
#include "../src/fxstate.c"
#include "../src/vactrol.c"
#include "../src/dst.c"
#include "../src/voices.c"
#include "../src/ctrl.c"
#include "../src/overlay.c"
#include "../src/ui.c"
#include "../src/fxdsp.c"
#include "../src/lfo.c"
/* in-memory NOR flash for persist.c */
static uint8_t FL[1 << 20];
static uint8_t flash_ok = 1;
static uint32_t irq_save(void) { return 0; }
static void irq_restore(uint32_t f) { (void)f; }
#define FL_FAR(fn) (fn)
static int fl_read_ram(uint32_t off, uint8_t *dst, uint32_t n) { if (off + n > sizeof FL) return -1; memcpy(dst, FL + off, n); return 0; }
static int fl_erase4k_ram(uint32_t off, uint32_t *t) { *t = 0; if (off + 0x1000 > sizeof FL) return -1; memset(FL + off, 0xFF, 0x1000); return 0; }
static int fl_prog_ram(uint32_t off, const uint8_t *src, uint32_t n, uint32_t *t) { uint32_t i; *t = 0; if (off + n > sizeof FL) return -1; for (i = 0; i < n; i++) FL[off + i] &= src[i]; return 0; }
#include "../src/persist.c"

#define EX __attribute__((visibility("default")))
#define HALF 128
static int16_t AUD[HALF];
static uint16_t BAND[SCR_W * 8];
static uint8_t RGB[SCR_W * SCR_H * 4];
static const int8_t CBOF[14] = { -1, -1, CB_FX, CB_SEL, CB_ENV, CB_LFO, CB_EDIT, CB_GLO, CB_HOME, CB_SAVE, CB_ARP, CB_SEQ, CB_PLAY, CB_REC };
static uint32_t master_q12 = 2800;
static uint32_t params_dirty = 1;
void ui_params_changed(void) { params_dirty = 1; }
static int ctrl_bank_key_cb(int key)
{
    static const uint8_t WH[12] = { 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1 };
    int k, w = 0;
    { uint32_t b = fm1_in.buttons, m = 0, i; for (i = 2; i < 14u; i++) if ((b >> i) & 1u) m |= 1u << CBOF[i]; ctrl_hw_held(m); }
    if (!WH[(53 + key) % 12]) { int o = 0; for (k = 0; k < key; k++) o += !WH[(53 + k) % 12]; return ctrl_op_key(o); }
    for (k = 0; k < key; k++) w += WH[(53 + k) % 12];
    if (ctrl_fx_key(w, ms)) return 1;
    if (ctrl_dseq_key(w)) return 1;
    if (ctrl_seq_key(w, ms)) return 1;
    if (!ctrl_bank_mode_key()) return 0;
    return ctrl_bank_key(w, ms);
}
static void ctrl_key_rel_cb(int key)
{
    static const uint8_t WH[12] = { 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1 };
    int k, o = 0;
    if (WH[(53 + key) % 12]) { for (k = 0; k < key; k++) o += WH[(53 + k) % 12]; ctrl_seq_release(o, ms); return; }
    for (k = 0; k < key; k++) o += !WH[(53 + k) % 12];
    ctrl_op_release(o);
}
EX void fw_init(void)
{
    ui_init(); vac_init(); voices_init(); ctrl_init(ms);
    persist_boot();
}
EX int16_t *fw_audio_ptr(void) { return AUD; }
EX uint8_t *fw_frame_ptr(void) { return RGB; }
/* one control-rate step: the clock, the keys, the screen state; call about every 5 ms */
EX void fw_tick(uint32_t now_ms, uint32_t keys)
{
    ms = now_ms; fm1_in.notes = keys;
    ctrl_tick(now_ms);
    ui_tick(now_ms);
}
EX void fw_button(int id, int down)                 /* a panel button (ui.h BTN_*) */
{
    if (down) { if (id == BTN_OCTM) ui_octave(-1), ctrl_octave(-1); else if (id == BTN_OCTP) ui_octave(1), ctrl_octave(1); else { fm1_in.buttons |= 1u << id; ctrl_button(CBOF[id], 1, ms); } }
    else if (CBOF[id] >= 0) { fm1_in.buttons &= ~(1u << id); ctrl_button(CBOF[id], 0, ms); }
}
EX void fw_side(int role, int steps) { ctrl_side(role, steps, ms); }      /* 0 SELECT, 1 ALGORITHM, 2 PRESETS */
EX void fw_knob(int k, int steps) { ui_encoder(k, steps); params_dirty = 1; }
EX void fw_master(int q12) { master_q12 = (uint32_t)q12; }
/* one audio block (128 frames, mono Q15) -> AUD */
EX void fw_audio(void)
{
    int32_t tmp[HALF]; int i;
    voices_keys(fm1_in.notes);
    if (params_dirty) { params_dirty = 0; vac_update(); }
    fx_begin();
    mod_apply(HALF);
    fx_admit();
    voices_render(tmp, HALF);
    mod_restore();
    for (i = 0; i < HALF; i++) {
        int32_t s = (int32_t)(((int64_t)tmp[i] * (int32_t)master_q12) >> 12);
        AUD[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
    }
}
/* the screen: RGBA 240 x 240 */
EX void fw_frame(void)
{
    int y, i;
    ui_prepare();
    for (y = 0; y < SCR_H; y += 8) {
        gfx_band(BAND, y, y + 8);
        ui_draw(y, y + 8);
        for (i = 0; i < SCR_W * 8; i++) {
            uint16_t p = BAND[i]; uint32_t r = (p >> 8) & 0xF8, g = (p >> 3) & 0xFC, b = (p << 3) & 0xF8;
            uint8_t *o = RGB + ((y * SCR_W) + i) * 4;
            o[0] = (uint8_t)(r | (r >> 5)); o[1] = (uint8_t)(g | (g >> 6)); o[2] = (uint8_t)(b | (b >> 5)); o[3] = 255;
        }
    }
}
EX uint32_t fw_leds(void) { return ctrl_lit(); }
