/* SPDX-License-Identifier: MPL-2.0
 * persist.c - the bank (presets) kept in flash across power cycles.
 *
 * Region: flash 0x93000..0x97000 (FL_XDATA0: between the app area, which the loader writes only below 0x93000,
 * and Felucca's store). Four 4 KiB sectors; the stream [header | presets] is cut in 0xF00-byte pieces, one per
 * sector, so bytes 0xF00..0xFFF of every sector stay erased (the update loader clears an "AT"+CRC record there).
 * Header: magic, version (size of one preset), count, payload CRC-32. Pages are programmed from the END of the
 * stream to the start, so the header (which makes the record valid) goes last: a cut power leaves either the old
 * record unreadable (sectors were erased) or the new one complete - never a half record that passes the CRC.
 * IRQs are off around each flash call (the fl_*_ram functions run from RAM). */
#define PS_BASE   0x93000u               /* record 0: the presets, 4 sectors */
#define PS_SECT   4u
#define PS_BASE1  0xE7000u               /* record 1: the 16 key scenes (FL_XDATA1), 2 sectors */
#define PS_SECT1  2u
#define PS_PIECE  0xF00u
#define PS_HDR    16u
#define PS_MAGIC  0x4F435442u            /* "OCTB" */
#define PS_MAX0   (PS_SECT * PS_PIECE - PS_HDR)
#define PS_MAX1   (PS_SECT1 * PS_PIECE - PS_HDR)

typedef char ps_fits[(sizeof(preset_t) * BANK_MAX <= PS_MAX0 && 16 * P_COUNT * 2 + 2 <= PS_MAX1) ? 1 : -1];

static uint8_t ps_buf[PS_HDR + sizeof(preset_t) * BANK_MAX];

static uint32_t ps_crc(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFu, i;
    int k;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}
static void ps_put32(uint8_t *d, uint32_t v) { d[0] = (uint8_t)v; d[1] = (uint8_t)(v >> 8); d[2] = (uint8_t)(v >> 16); d[3] = (uint8_t)(v >> 24); }
static uint32_t ps_get32(const uint8_t *d) { return d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24); }

/* flash address of stream byte s */
static uint32_t ps_addr(uint32_t base, uint32_t s) { return base + (s / PS_PIECE) * 0x1000u + s % PS_PIECE; }

static int ps_read(uint32_t base, uint32_t s, uint8_t *dst, uint32_t n)
{
    while (n) {
        uint32_t c = PS_PIECE - s % PS_PIECE, f;
        int rc;
        if (c > n) c = n;
        if (c > 256u) c = 256u;
        f = irq_save();
        rc = FL_FAR(fl_read_ram)(ps_addr(base, s), dst, c);
        irq_restore(f);
        if (rc) return rc;
        s += c; dst += c; n -= c;
    }
    return 0;
}

/* 0 = ok */
static int ps_write(uint32_t base, uint32_t nsect, uint32_t mx, uint32_t ver, const void *p, uint32_t n)
{
    uint32_t total = PS_HDR + n, sect = (total + PS_PIECE - 1) / PS_PIECE, i, page, took;
    if (!flash_ok || n > mx) return -1;
    for (i = 0; i < n; i++) ps_buf[PS_HDR + i] = ((const uint8_t *)p)[i];
    ps_put32(ps_buf, PS_MAGIC);
    ps_put32(ps_buf + 4, ver);
    ps_put32(ps_buf + 8, n);
    ps_put32(ps_buf + 12, ps_crc(ps_buf + PS_HDR, n));
    for (i = 0; i < nsect; i++) {                      /* erase all, so a shorter record leaves no stale tail */
        uint32_t f = irq_save();
        int rc = FL_FAR(fl_erase4k_ram)(base + i * 0x1000u, &took);
        irq_restore(f);
        if (rc) return rc;
    }
    (void)sect;
    /* pages from the last to the first; a page never crosses a sector's 0xF00 piece (0xF00 = 15 * 256) */
    for (page = (total + 255u) / 256u; page-- > 0;) {
        uint32_t s = page * 256u, c = total - s, f;
        int rc;
        if (c > 256u) c = 256u;
        f = irq_save();
        rc = FL_FAR(fl_prog_ram)(ps_addr(base, s), ps_buf + s, c, &took);
        irq_restore(f);
        if (rc) return rc;
    }
    return 0;
}

/* 0 = a valid record was read into ps_buf (payload at ps_buf + PS_HDR), *n its length */
static int ps_load(uint32_t base, uint32_t mx, uint32_t ver, uint32_t unit, uint32_t *n)
{
    uint32_t len;
    if (!flash_ok || ps_read(base, 0, ps_buf, PS_HDR)) return -1;
    if (ps_get32(ps_buf) != PS_MAGIC || ps_get32(ps_buf + 4) != ver) return -2;
    len = ps_get32(ps_buf + 8);
    if (len > mx || len % unit) return -3;
    if (ps_read(base, PS_HDR, ps_buf + PS_HDR, len)) return -4;
    if (ps_crc(ps_buf + PS_HDR, len) != ps_get32(ps_buf + 12)) return -5;
    *n = len;
    return 0;
}

/* the controller's hooks */
void bank_store(const void *p, uint32_t n) { (void)ps_write(PS_BASE, PS_SECT, PS_MAX0, (uint32_t)sizeof(preset_t), p, n); }
/* scenes: used-mask (2 bytes) + 16 x P_COUNT int16 */
void scene_store(const void *p, uint32_t n) { (void)ps_write(PS_BASE1, PS_SECT1, PS_MAX1, 0x10000u | P_COUNT, p, n); }

/* at boot, after ctrl_init (which seeds the factory bank): replace it with the saved ones */
void persist_boot(void)
{
    uint32_t n;
    if (ps_load(PS_BASE, PS_MAX0, (uint32_t)sizeof(preset_t), (uint32_t)sizeof(preset_t), &n) == 0) bank_restore(ps_buf + PS_HDR, n);
    if (ps_load(PS_BASE1, PS_MAX1, 0x10000u | P_COUNT, 1, &n) == 0) scene_restore(ps_buf + PS_HDR, n);
}
