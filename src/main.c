/* SPDX-License-Identifier: MPL-2.0
 * octavio-native: FM-1 firmware written from scratch after the "FM-1 Engine Test Bench" prototype.
 * Hardware layer: felucca-bsp (MPL-2.0, bsp/). Everything else (audio engine, screen, pages) is new code.
 * Boot, guard, timer and update plumbing follow the BSP's hwtest example. */
#include <stdint.h>
#define FELUCCA_CDC 0
#define FELUCCA_OTA 1             /* the update entry: the installer can replace this firmware over USB-MIDI */
#define FELUCCA_ID "FM-1_2000"
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#define PERSIST_FLASH 1
#include "fm1_time.h"
#include "fm1_sys.h"
#include "fm1_irq.h"
#include "fm1_guard.h"
#include "fm1_timer.h"
#include "fm1_input.h"
#include "fm1_adc.h"
#include "fm1_lcd_hw.h"
#include "fm1_flash.h"
#include "fm1_audio.h"
#include "libc.c"
#include "usb.c"
#include "ota.c"
#include "octavio.h"
#include "params.h"
#include "ui.h"
#include "mathfx.c"
#include "tables_gen.c"
#include "assets_gen.c"
#include "gfx.c"
#include "params.c"
#include "fxstate.c"
#include "vactrol.c"
#include "dst.c"
#include "voices.c"
#include "ctrl.c"
#include "overlay.c"
#include "leds.c"
#include "ui.c"
#include "fxdsp.c"
#include "lfo.c"
#include "loadtest.c"

#ifndef NO_PERF
#define PERF_ENC(s) (perf.enc_steps += (uint32_t)((s) < 0 ? -(s) : (s)))
#else
#define PERF_ENC(s) ((void)0)
#endif
extern uint32_t _data_start[], _data_end[], _data_load[], _bss_start[], _bss_end[];
extern uint32_t _rt_start[], _rt_end[], _rt_load[];

static volatile uint32_t ms;
static uint32_t boots_ok;
static struct { uint32_t magic, pending, failed; } guard __attribute__((section(".noinit")));
/* Boot trace for a start that never reaches the main loop (first real-FM-1 test: after a power-off the FM-1 stays dark and ends in the chip's update mode):
 * [0] magic, [1] the stage this attempt has reached, [2] the stage the previous attempt had reached, [3] attempts since the last good start.  Read with tools/fm1_peek.py in update mode. */
volatile uint32_t octavio_boot[4] __attribute__((section(".noinit")));
#ifdef BOOTLOG
/* flash trail of every start (survives UBOOT and power loss): sector 0xE7000, one byte per event, appended, never erased
 * until full.  A start = 0xA0, the reset reason (P33 RST_SRC & 0x7F), stages 3..17 as they are reached, 0xB0 at the healthy point. */
#define BL_BASE 0xE7000u
static uint32_t bl_pos = 0xFFFFFFFFu;
static void boot_log(uint32_t code)
{
    uint8_t b = (uint8_t)code, c[64];
    uint32_t f, took, i;
    if (bl_pos == 0xFFFFFFFFu) {
        bl_pos = 0;
        while (bl_pos < 0x1000u) {
            f = irq_save();
            FL_FAR(fl_read_ram)(BL_BASE + bl_pos, c, 64);
            irq_restore(f);
            for (i = 0; i < 64u && c[i] != 0xFFu; i++)
                ;
            bl_pos += i;
            if (i < 64u)
                break;
        }
        if (bl_pos >= 0xF00u) {
            f = irq_save();
            FL_FAR(fl_erase4k_ram)(BL_BASE, &took);
            irq_restore(f);
            bl_pos = 0;
        }
    }
    if (bl_pos >= 0x1000u)
        return;
    f = irq_save();
    FL_FAR(fl_prog_ram)(BL_BASE + bl_pos, &b, 1, &took);
    irq_restore(f);
    bl_pos++;
}
#define BOOT_STAGE(n) do { octavio_boot[1] = (n); fm1_wdt_feed(); boot_log(n); fm1_wdt_feed(); } while (0)
#else
#define BOOT_STAGE(n) do { octavio_boot[1] = (n); fm1_wdt_feed(); } while (0)
#endif

/* ---- audio: ALNK0 double buffer, 128 frames a half ---- */
#define HALF_FRAMES 128u
#define HALF_WORDS (HALF_FRAMES * 2u)
static int32_t abuf[2u * HALF_WORDS] __attribute__((aligned(4)));
static volatile uint32_t master_q12 = 2800;
static volatile uint8_t params_dirty = 1;
extern void isr_alnk0(void);

void ui_params_changed(void) { params_dirty = 1; }
static const int8_t CBOF[14] = { -1, -1, CB_FX, CB_SEL, CB_ENV, CB_LFO, CB_EDIT, CB_GLO, CB_HOME, CB_SAVE, CB_ARP, CB_SEQ, CB_PLAY, CB_REC };      /* button id -> controller button */
static int ctrl_bank_key_cb(int key)                  /* HOME held / SAVE BANK: the white keys (F-based keyboard: key 0 = note 53) pick a bank */
{
    static const uint8_t WH[12] = { 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1 };
    int k, w = 0;
    {                                                     /* chords: the buttons physically down now count as held even if the main loop (busy redrawing under load) has not taken their press yet */
        uint32_t b = fm1_in.buttons, m = 0, i;
        for (i = 2; i < 14u; i++) if ((b >> i) & 1u) m |= 1u << CBOF[i];
        ctrl_hw_held(m);
    }
    if (!WH[(53 + key) % 12]) {                           /* a black key: the OP key o (chords with EDIT / GLO / ENV held) */
        int o = 0;
        for (k = 0; k < key; k++) o += !WH[(53 + k) % 12];
        return ctrl_op_key(o);
    }
    for (k = 0; k < key; k++) w += WH[(53 + k) % 12];
    if (ctrl_fx_key(w, ms)) return 1;
    if (ctrl_dseq_key(w)) return 1;                       /* the drum sequencer: the white key toggles a step of the selected lane */
    if (ctrl_seq_key(w, ms)) return 1;                    /* the pitch sequencer page: the white key selects / sets a step */
    if (!ctrl_bank_mode_key()) return 0;
    return ctrl_bank_key(w, ms);
}

static void ctrl_key_rel_cb(int key)                  /* a key that was a chord (no note) is released: a black key = an OP key let go */
{
    static const uint8_t WH[12] = { 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1 };
    int k, o = 0;
    if (WH[(53 + key) % 12]) {                            /* a white key: the SEQ page's step key (tap / hold) */
        for (k = 0; k < key; k++) o += WH[(53 + k) % 12];
        ctrl_seq_release(o, ms);
        return;
    }
    for (k = 0; k < key; k++) o += !WH[(53 + k) % 12];
    ctrl_op_release(o);
}

void fm1_alnk0_irq(void)
{
    uint8_t p = fm1_audio_pending();
    fm1_audio_ack_aux(p);
    if (p & FM1_AUDIO_HALF) {
        uint32_t half = fm1_audio_free_half(), i, now = fm1_in.notes, t_blk = fm1_ticks();
        int32_t *o = &abuf[half * HALF_WORDS], tmp[HALF_FRAMES];
        if (params_dirty) {
            params_dirty = 0;
            vac_update();
        }
#ifndef NO_PERF
        if (perf.blocks > PERF_WARM) {              /* note keys closed since the last block: ticks from the first scan that saw them closed to the start of this block (NOTES-J) */
            uint32_t pr = now & ~keys_prev_v, k;
            for (k = 0; pr && k < 27u; k++, pr >>= 1) if (pr & 1u) { uint32_t l = t_blk - fm1_in.note_t0[k]; perf.key_n++; perf.key_sum += l; if (l > perf.key_max) perf.key_max = l; if (!perf.gov[6]) { perf.gov[6] = perf.blocks + 1u; perf.gov[7] = l; } }
        }
#endif
        voices_keys(now);                           /* kOnT / kOffT of the selected track (the bank keys: voices.c) */
        { uint32_t m = now; int c = 0; hud.keys = now; while (m) { c += (int)(m & 1u); m >>= 1; } if (c > hud.kmax) hud.kmax = (uint8_t)c; hud.btn = fm1_in.buttons; }
#ifdef BENCH
        bench_tick();
#endif
        fx_begin();                                 /* the tracks' FX knobs for this block (fxdsp.c) */
        mod_apply(HALF_FRAMES);                     /* LFOs / S&H -> destinations (lfo.c), put back by mod_restore */
        fx_admit();                                 /* the CPU budget: which active effect slots run (fxdsp.c) */
        voices_render(tmp, HALF_FRAMES);
        mod_restore();
        for (i = 0; i < HALF_FRAMES; i++) {
            int32_t s = (int32_t)(((int64_t)tmp[i] * (int32_t)master_q12) >> 12);
            s = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
            o[2u * i] = o[2u * i + 1u] = s * (1 << 7);          /* Q15 -> 24 bit, -6 dBFS ceiling */
#ifdef MOD_TEST
            o[2u * i] = s * (1 << 7); o[2u * i + 1u] = mod_tap * 32 * (1 << 7);          /* test builds: R = PV[MOD_ID] (modulated), x16 in the wav */
#elif defined(FX_TEST)                                         /* test builds: L = the bus after the effects, R = before, raw Q15 / 2 (no master, no tanh) */
            { int32_t w = fx_wet_tap[i] >> 1, d = fx_dry_tap[i] >> 1; w = w > 32767 ? 32767 : w < -32768 ? -32768 : w; d = d > 32767 ? 32767 : d < -32768 ? -32768 : d; o[2u * i] = w * (1 << 7); o[2u * i + 1u] = d * (1 << 7); }
#endif
        }
        fm1_audio_ack_half();
        perf.audio_last = fm1_ticks() - t_blk;                     /* CPU measure of the whole audio half-block (69,660 ticks = 100 %) */
        perf.audio_sum += perf.audio_last;
        if (perf.audio_last > perf.audio_max) perf.audio_max = perf.audio_last;
        if (++perf.blocks > 200 && perf.audio_last > perf.audio_max_w) perf.audio_max_w = perf.audio_last;      /* worst block after the first 0.6 s (allocation and start-up excluded) */
#ifndef NO_GOV
        hud_block(t_blk, perf.audio_last);
#endif
        gov_block(perf.audio_last > fx_alloc_blk ? perf.audio_last - fx_alloc_blk : perf.audio_last);       /* the CPU governor (fxstate.c); a chain's one-off allocation is not load */
        fx_alloc_blk = 0;
#ifndef NO_PERF
        perf.gov[0] = gov.fx; perf.gov[1] = gov.voices; perf.gov[2] = gov.steals; perf.gov[3] = gov.cap_min; perf.gov[4] = (uint32_t)dv_cap; perf.gov[5] = gov_cool;
        perf_audio(t_blk, perf.audio_last, fx_blk_ticks);       /* histogram, lost boundaries (entry gap >= 2 blocks), ISR spacing */
#endif
    }
}

/* ---- TIMER5, 10 kHz: key scan, milliseconds, USB at 2 kHz ---- */
void fm1_timer5_irq(void)
{
    static uint32_t sub, last, acc;
    uint32_t t = fm1_ticks();
    fm1_timer5_ack();
    fm1_input_tick();
    acc += t - last;
    last = t;
    while (acc >= 1000u * FM1_TICKS_PER_US) {
        acc -= 1000u * FM1_TICKS_PER_US;
        ms++;
    }
    if (++sub == 5u) {
        sub = 0;
        usb_poll();
    }
}
extern void isr_timer5(void);

/* ---- the ST7789 screen, 240 x 240, RGB565 high byte first ---- */
static void lcd_cmd(uint8_t c, const uint8_t *d, uint32_t n)
{
    static uint8_t buf[4];
    uint32_t i;
    fm1_lcd_send_cmd(c);
    fm1_lcd_wait();
    fm1_lcd_deselect();
    if (!n)
        return;
    for (i = 0; i < n; i++)
        buf[i] = d[i];
    fm1_lcd_send_data(buf, n);
    fm1_lcd_wait();
    fm1_lcd_deselect();
}

static void lcd_init(void)
{
    static const uint8_t colmod = 0x55, madctl = 0x00;
    fm1_lcd_hw_init();
    fm1_delay_ms(50);
    lcd_cmd(0x01, 0, 0);
    fm1_delay_ms(150);
    lcd_cmd(0x11, 0, 0);
    fm1_delay_ms(120);
    lcd_cmd(0x3A, &colmod, 1);
    lcd_cmd(0x36, &madctl, 1);
    lcd_cmd(0x21, 0, 0);                        /* INVON: this panel is IPS */
    lcd_cmd(0x13, 0, 0);
    lcd_cmd(0x29, 0, 0);
    fm1_lcd_baud(4);                            /* 12 MHz */
}

#define BAND_H 8
static uint16_t band_buf[2][SCR_W * BAND_H] __attribute__((aligned(4)));
static uint8_t dma_busy;

static void lcd_sync(void)
{
    if (dma_busy) {
        fm1_lcd_wait();
        fm1_lcd_deselect();
        dma_busy = 0;
    }
}

static void lcd_frame(int y0, int y1)           /* rows [y0, y1): drawn band by band, the DMA of one overlaps the next */
{
    uint8_t a[4];
    int y, k = 0;
    ui_prepare();
    a[0] = 0; a[1] = 0; a[2] = 0; a[3] = 239;
    lcd_cmd(0x2A, a, 4);
    a[1] = (uint8_t)y0; a[3] = (uint8_t)(y1 - 1);
    lcd_cmd(0x2B, a, 4);
    lcd_cmd(0x2C, 0, 0);
    for (y = y0; y < y1; y += BAND_H, k++) {
        int h = y1 - y < BAND_H ? y1 - y : BAND_H, i;
        uint16_t *b = band_buf[k & 1];
        gfx_band(b, y, y + h);
        ui_draw(y, y + h);
        for (i = 0; i < h * SCR_W; i++)                 /* the panel wants the high byte first */
            b[i] = (uint16_t)((b[i] >> 8) | (b[i] << 8));
        lcd_sync();
        fm1_lcd_send_data(b, (uint32_t)(h * SCR_W * 2));
        dma_busy = 1;
    }
    lcd_sync();
}

/* ---- the update (ota.c): the installer's M-UPGRADE session over USB-MIDI ---- */
static uint8_t flash_ok;
#include "persist.c"
static uint32_t ota_now_ms(void) { return ms; }
static void ota_idle(void) { fm1_wdt_feed(); }
static int ota_in_area(uint32_t off, uint32_t n) { return FL_IN(off, n, OTA_AREA, OTA_AREA + OTA_AREA_LEN); }
static int ota_erase(uint32_t off)
{
    uint32_t took;
    if (!ota_in_area(off, 0x1000u) || (off & 0xFFFu))
        return -8;
    return fl_erase4k(off, &took);
}
static int ota_prog(uint32_t off, const void *p, uint32_t n)
{
    if (!ota_in_area(off, n))
        return -8;
    return fl_write(off, p, n);
}
static int ota_fread(uint32_t off, void *p, uint32_t n)
{
    uint8_t *d = p;
    while (n) {
        uint32_t k = n > 256u ? 256u : n, f = irq_save();
        int rc = FL_FAR(fl_read_ram)(off, d, k);
        irq_restore(f);
        if (rc)
            return rc;
        off += k;
        d += k;
        n -= k;
    }
    return 0;
}
static void ota_show(uint32_t step, int32_t code)   /* progress as a bar across the bottom */
{
    static uint16_t row[SCR_W];
    uint8_t a[4];
    uint32_t i, w = step < 9u ? step * 26u : 240u;
    uint16_t c = step < 9u ? 0x07FFu : (code == 1 ? 0x07E0u : 0xF800u);
    c = (uint16_t)((c >> 8) | (c << 8));
    for (i = 0; i < SCR_W; i++)
        row[i] = i < w ? c : 0;
    lcd_sync();
    a[0] = 0; a[1] = 0; a[2] = 0; a[3] = 239;
    lcd_cmd(0x2A, a, 4);
    a[1] = 220; a[3] = 229;
    lcd_cmd(0x2B, a, 4);
    lcd_cmd(0x2C, 0, 0);
    for (i = 0; i < 10u; i++) {
        fm1_lcd_send_data(row, SCR_W * 2u);
        fm1_lcd_wait();
        fm1_lcd_deselect();
    }
}
static void ota_commit(const uint8_t *parm)
{
    guard.pending = 0;
    guard.failed = 0;
    usb_detach();
    fm1_delay_ms(30);
    fm1_enter_update(parm);
}

static void fm1_fault(const fm1_crash_t *c)     /* a fault: red screen, then reset */
{
    static uint16_t row[SCR_W];
    uint8_t a[4];
    uint32_t i;
    (void)c;
    fm1_audio_stop();
    for (i = 0; i < SCR_W; i++)
        row[i] = 0x00F8u;                       /* red, byte-swapped */
    a[0] = 0; a[1] = 0; a[2] = 0; a[3] = 239;
    lcd_cmd(0x2A, a, 4);
    lcd_cmd(0x2B, a, 4);
    lcd_cmd(0x2C, 0, 0);
    for (i = 0; i < SCR_H; i++) {
        fm1_lcd_send_data(row, SCR_W * 2u);
        fm1_lcd_wait();
        fm1_lcd_deselect();
    }
    fm1_delay_ms(1500);
    fm1_reboot();
}

#ifndef NO_PERF
extern char _ustack_lo[], _ustack_top[], _sstack_lo[], _sstack_top[];
#define STACK_PAINT 0xA5C3A5C3u
static void perf_stack_paint(void)                   /* boot: both stacks filled with a pattern below the current sp / the interrupt stack's top 256 bytes */
{
    uint32_t *p, *e;
    volatile uint32_t here;
    e = (uint32_t *)((uintptr_t)&here - 256u);
    for (p = (uint32_t *)_ustack_lo; p < e; p++) *p = STACK_PAINT;
    e = (uint32_t *)(_sstack_top - 256);
    for (p = (uint32_t *)_sstack_lo; p < e; p++) *p = STACK_PAINT;
}
static void perf_stack_scan(void)                    /* bytes below the deepest write of each stack that still hold the pattern (the first word that differs from the bottom up) */
{
    uint32_t *p = (uint32_t *)_ustack_lo;
    while (p < (uint32_t *)_ustack_top && *p == STACK_PAINT) p++;
    perf.ustack_free = (uint32_t)((char *)p - _ustack_lo);
    p = (uint32_t *)_sstack_lo;
    while (p < (uint32_t *)_sstack_top && *p == STACK_PAINT) p++;
    perf.sstack_free = (uint32_t)((char *)p - _sstack_lo);
    perf.bss_end = (uint32_t)(uintptr_t)_bss_end;
    perf.ram_free = 0x01C74000u - perf.bss_end;
}
#endif

static void app_main(void)
{
    uint32_t i, last_adc = 0, last_frame = 0;
    octavio_boot[1] = 2;
    {
        uint32_t f = irq_save();
        flash_ok = FL_FAR(fl_jedec_ram)() == 0x856014u;
        irq_restore(f);
        if (flash_ok)
            ota_boot_cleanup();
    }
#ifdef BOOTLOG
    fm1_reset_reason();
    boot_log(0xA0);
    boot_log(fm1_boot.p3_rst & 0x7Fu);
    boot_log(fm1_boot.wdt_con & 0x7Fu);
#endif
    BOOT_STAGE(3);
    lcd_init();
    BOOT_STAGE(4);
    ui_init();
    BOOT_STAGE(5);
    vac_init();
    BOOT_STAGE(6);
    voices_init();
    BOOT_STAGE(7);
    ctrl_init(ms);
    BOOT_STAGE(8);
    persist_boot();
    BOOT_STAGE(9);
#ifdef LOADTEST
    lt_setup();                                 /* the worst-case state of NOTES-J */
#endif
    fm1_input_init();
    BOOT_STAGE(10);
    fm1_adc_init();
    BOOT_STAGE(11);
    for (i = 0; i < 2u * HALF_WORDS; i++)
        abuf[i] = 0;
    fm1_audio_init(abuf, HALF_WORDS, isr_alnk0, 3);
    BOOT_STAGE(12);
    usb_start();
    BOOT_STAGE(13);
    fm1_timer5_start(isr_timer5, 4);
    BOOT_STAGE(14);
    fm1_guard_lock_top();
    fm1_irq_enable_all();
    BOOT_STAGE(15);
    lcd_frame(0, SCR_H);
    BOOT_STAGE(16);
    fm1_lcd_backlight(1);
    BOOT_STAGE(17);
    for (;;) {
        uint32_t now = ms, pr, rel;
        fm1_wdt_feed();
#ifndef NO_PERF
        {                                           /* main-loop pass: the longest time between two passes is how long a key or an encoder click can wait */
            uint32_t tl = fm1_ticks();
            perf.loop_n++;
            if (perf.last_loop && perf.blocks > PERF_WARM && tl - perf.last_loop > perf.loop_gap_max) perf.loop_gap_max = tl - perf.last_loop;
            perf.last_loop = tl;
            if ((perf.loop_n & 0x3FFu) == 1u) perf_stack_scan();
        }
#endif
        usb_retry(now);
        if (usb.uboot_req) {
            fm1_delay_ms(30);
            usb_detach();
            fm1_enter_uboot();
        }
        ota_service();
        if (usb.ota_req) {
            usb.ota_req = 0;
            if (flash_ok)
                ota_session();
            lcd_frame(0, SCR_H);
        }
        if (!boots_ok && now > 2000u) {
            boots_ok = 1;
            octavio_boot[1] = 20; octavio_boot[3] = 0;
#ifdef BOOTLOG
            boot_log(0xB0);
#endif
            guard.pending = 0;
            guard.failed = 0;
        }
        if (now - last_adc >= 40u) {                /* MASTER: the pot */
            int32_t k = fm1_adc_read(FM1_ADC_MASTER);
            last_adc = now;
            if (k >= 0)
                master_q12 = ((uint32_t)k * (uint32_t)k) >> 8;
        }
        pr = fm1_input_edges(&rel);
        for (i = 0; i < 14u; i++) {
            if ((pr >> i) & 1u) {
#ifndef NO_PERF
                perf.btn_n++;
#endif
                if (i == BTN_OCTM)
                    ui_octave(-1), ctrl_octave(-1);
                else if (i == BTN_OCTP)
                    ui_octave(1), ctrl_octave(1);
                else
                    ctrl_button(CBOF[i], 1, now);
            }
            if (((rel >> i) & 1u) && CBOF[i] >= 0)
                ctrl_button(CBOF[i], 0, now);
        }
        {
            int32_t s;
            if ((s = fm1_enc_take(0u)) != 0) { PERF_ENC(s); ctrl_side(ROLE_SELECT, (int)s, now); }
            if ((s = fm1_enc_take(1u)) != 0) { PERF_ENC(s); ctrl_side(ROLE_ALGO, (int)s, now); }
            if ((s = fm1_enc_take(6u)) != 0) { PERF_ENC(s); ctrl_side(ROLE_PRESETS, (int)s, now); }
            ctrl_tick(now);
            { static uint32_t seen; if (fx_refused != seen) { seen = fx_refused; ctrl_cpu_full(now); } }      /* an effect did not fit the CPU budget */
        }
        for (i = 0; i < 4u; i++) {
            int32_t s = fm1_enc_take(2u + i);
            if (s) {
                PERF_ENC(s);
                ui_encoder((int)i, (int)s);
                params_dirty = 1;
            }
        }
        leds_update();                                               /* key / arpeggiator LEDs, only on a change */
        {
            int y0, y1;
            ui_tick(now);
            if (now - last_frame >= 20u) {
                last_frame = now;
                ui_scene(&y0, &y1);
#ifndef NO_PERF
                {
                    uint32_t tf = fm1_ticks(), d;
                    lcd_frame(y0, y1); hud.nfr++;
                    d = fm1_ticks() - tf;
                    if (perf.blocks > PERF_WARM) {
                        perf.frame_n++; perf.frame_sum += d; perf.rows_sum += (uint32_t)(y1 - y0);
                        if (d > perf.frame_max) perf.frame_max = d;
                        if (y1 - y0 >= SCR_H) { perf.full_n++; if (d > perf.full_max) perf.full_max = d; }
                        if (perf.last_frame && tf - perf.last_frame > perf.frame_gap_max) perf.frame_gap_max = tf - perf.last_frame;
                    }
                    perf.last_frame = tf;
                }
#else
                lcd_frame(y0, y1); hud.nfr++;
#endif
            }
        }
    }
}

void fm1_cstart(void)
{
    uint32_t *s, *d;
    fm1_wdt_arm(0x0D);                          /* first of all: whatever hangs after this, the watchdog and the boot guard bring the FM-1 back to update mode */
#ifdef BOOTLOG
    FM1_LCD_PA_OUT &= ~FM1_LCD_BL;              /* beacon: backlight on at the very first instructions of cstart */
    FM1_LCD_PA_DIR &= ~FM1_LCD_BL;
#endif
    fm1_time_init();
    fm1_vdc14_raise(7);                         /* M-VAVE's firmware raises the core supplies to these as it starts; the boot loader */
    fm1_sysvdd_raise(15);                       /* leaves 3 and 11, which some units do not run 360 MHz code on (felucca-bsp fm1_sys.h) */
    if (octavio_boot[0] != 0x4F425354u) { octavio_boot[0] = 0x4F425354u; octavio_boot[1] = 0; octavio_boot[2] = 0; octavio_boot[3] = 0; }
    octavio_boot[2] = octavio_boot[1]; octavio_boot[3]++; octavio_boot[1] = 1;
    if (guard.magic != 0x4F435431u) {
        guard.magic = 0x4F435431u;
        guard.failed = 0;
        guard.pending = 0;
    }
    if (guard.pending)
        guard.failed++;
    guard.pending = 1;
    if (guard.failed >= 2u) {
        guard.failed = 0;
        guard.pending = 0;
        fm1_enter_uboot();
    }
    octavio_boot[1] = 0x31;
    fm1_irq_init();
    octavio_boot[1] = 0x32;
    for (d = _bss_start; d < _bss_end; d++)
        *d = 0;
    octavio_boot[1] = 0x33;
    for (s = _data_load, d = _data_start; d < _data_end; s++, d++)
        *d = *s;
    octavio_boot[1] = 0x34;
    for (s = _rt_load, d = _rt_start; d < _rt_end; s++, d++)
        *d = *s;
#ifdef BOOTLOG
    boot_log(0xC0);                             /* cstart got this far (flash driver is in RAM now) */
    boot_log(fm1_p33_read(FM1_P3_RST_SRC) & 0x7Fu);
    boot_log(*(volatile uint32_t *)0x10014u & 0x7Fu);   /* system clock source */
    boot_log(*(volatile uint32_t *)0x119A8u & 0x7Fu);   /* PLL1 */
    boot_log(*(volatile uint32_t *)0x119ACu & 0x7Fu);
    boot_log((*(volatile uint32_t *)0x119ACu >> 7) & 0x7Fu);
    boot_log(*(volatile uint32_t *)0x10008u & 0x7Fu);   /* HSB/LSB dividers */
    boot_log((*(volatile uint32_t *)0x10008u >> 8) & 0x7Fu);
    boot_log((*(volatile uint32_t *)0x10008u >> 16) & 0x7Fu);
#endif
#ifndef NO_PERF
    perf_stack_paint();
    perf.magic = 0x4A504552u;
#endif
    octavio_boot[1] = 0x35;
    fm1_guard_enable(FM1_GUARD_STACK | FM1_GUARD_WRITE | FM1_GUARD_BUS | FM1_GUARD_PC);
    app_main();
    for (;;)
        ;
}
