/* SPDX-License-Identifier: MPL-2.0
 * octavio-native: FM-1 firmware written from scratch after the "FM-1 Engine Test Bench" prototype.
 * Hardware layer: felucca-bsp (MPL-2.0, bsp/). Everything else (audio engine, screen, pages) is new code.
 * Boot, guard, timer and update plumbing follow the BSP's hwtest example. */
#include <stdint.h>
#define FELUCCA_CDC 0
#define FELUCCA_OTA 1             /* the update entry: the installer can replace this firmware over USB-MIDI */
#define FELUCCA_ID "FM-1_2000"
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
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
#include "vactrol.c"
#include "voices.c"
#include "ctrl.c"
#include "overlay.c"
#include "leds.c"
#include "ui.c"
#include "fxdsp.c"

extern uint32_t _data_start[], _data_end[], _data_load[], _bss_start[], _bss_end[];
extern uint32_t _rt_start[], _rt_end[], _rt_load[];

static volatile uint32_t ms;
static uint32_t boots_ok;
static struct { uint32_t magic, pending, failed; } guard __attribute__((section(".noinit")));

/* ---- audio: ALNK0 double buffer, 128 frames a half ---- */
#define HALF_FRAMES 128u
#define HALF_WORDS (HALF_FRAMES * 2u)
static int32_t abuf[2u * HALF_WORDS] __attribute__((aligned(4)));
static volatile uint32_t master_q12 = 2800;
static volatile uint8_t params_dirty = 1;
extern void isr_alnk0(void);

void ui_params_changed(void) { params_dirty = 1; }
static int ctrl_bank_key_cb(int key)                  /* HOME held / SAVE BANK: the white keys (F-based keyboard: key 0 = note 53) pick a bank */
{
    static const uint8_t WH[12] = { 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1 };
    int k, w = 0;
    if (!ctrl_bank_mode() || !WH[(53 + key) % 12]) return 0;
    for (k = 0; k < key; k++) w += WH[(53 + k) % 12];
    return ctrl_bank_key(w, ms);
}

void fm1_alnk0_irq(void)
{
    uint8_t p = fm1_audio_pending();
    fm1_audio_ack_aux(p);
    if (p & FM1_AUDIO_HALF) {
        uint32_t half = fm1_audio_free_half(), i, now = fm1_in.notes;
        int32_t *o = &abuf[half * HALF_WORDS], tmp[HALF_FRAMES];
        if (params_dirty) {
            params_dirty = 0;
            vac_update();
        }
        voices_keys(now);                           /* kOnT / kOffT of the selected track (the bank keys: voices.c) */
        voices_render(tmp, HALF_FRAMES);
        for (i = 0; i < HALF_FRAMES; i++) {
            int32_t s = (int32_t)(((int64_t)tmp[i] * (int32_t)master_q12) >> 12);
            s = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
            o[2u * i] = o[2u * i + 1u] = s * (1 << 7);          /* Q15 -> 24 bit, -6 dBFS ceiling */
#ifdef FX_TEST                                         /* test builds: L = the bus after the effects, R = before, raw Q15 / 2 (no master, no tanh) */
            { int32_t w = fx_wet_tap[i] >> 1, d = fx_dry_tap[i] >> 1; w = w > 32767 ? 32767 : w < -32768 ? -32768 : w; d = d > 32767 ? 32767 : d < -32768 ? -32768 : d; o[2u * i] = w * (1 << 7); o[2u * i + 1u] = d * (1 << 7); }
#endif
        }
        fm1_audio_ack_half();
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

static void app_main(void)
{
    uint32_t i, last_adc = 0, last_frame = 0;
    {
        uint32_t f = irq_save();
        flash_ok = FL_FAR(fl_jedec_ram)() == 0x856014u;
        irq_restore(f);
        if (flash_ok)
            ota_boot_cleanup();
    }
    lcd_init();
    ui_init();
    vac_init();
    voices_init();
    ctrl_init(ms);
    fm1_input_init();
    fm1_adc_init();
    for (i = 0; i < 2u * HALF_WORDS; i++)
        abuf[i] = 0;
    fm1_audio_init(abuf, HALF_WORDS, isr_alnk0, 3);
    usb_start();
    fm1_timer5_start(isr_timer5, 4);
    fm1_guard_lock_top();
    fm1_irq_enable_all();
    lcd_frame(0, SCR_H);
    fm1_lcd_backlight(1);
    for (;;) {
        uint32_t now = ms, pr, rel;
        fm1_wdt_feed();
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
            static const int8_t CBOF[14] = { -1, -1, CB_FX, CB_SEL, CB_ENV, CB_LFO, CB_EDIT, CB_GLO, CB_HOME, CB_SAVE, CB_ARP, CB_SEQ, CB_PLAY, CB_REC };
            if ((pr >> i) & 1u) {
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
            if ((s = fm1_enc_take(0u)) != 0) ctrl_side(ROLE_SELECT, (int)s, now);
            if ((s = fm1_enc_take(1u)) != 0) ctrl_side(ROLE_ALGO, (int)s, now);
            if ((s = fm1_enc_take(6u)) != 0) ctrl_side(ROLE_PRESETS, (int)s, now);
            ctrl_tick(now);
        }
        for (i = 0; i < 4u; i++) {
            int32_t s = fm1_enc_take(2u + i);
            if (s) {
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
                lcd_frame(y0, y1);
            }
        }
    }
}

void fm1_cstart(void)
{
    uint32_t *s, *d;
    fm1_time_init();
    fm1_wdt_arm(0x0D);
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
    fm1_irq_init();
    for (d = _bss_start; d < _bss_end; d++)
        *d = 0;
    for (s = _data_load, d = _data_start; d < _data_end; s++, d++)
        *d = *s;
    for (s = _rt_load, d = _rt_start; d < _rt_end; s++, d++)
        *d = *s;
    fm1_guard_enable(FM1_GUARD_STACK | FM1_GUARD_WRITE | FM1_GUARD_BUS | FM1_GUARD_PC);
    app_main();
    for (;;)
        ;
}
