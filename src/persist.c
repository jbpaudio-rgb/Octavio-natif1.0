/* SPDX-License-Identifier: MPL-2.0
 * persist.c - the bank (presets) and the 16 key scenes kept in flash across power cycles.
 *
 * Regions (FL_XDATA1 of the BSP is 0xE7000..0xFC000, "a firmware's own data"): scenes 0xE7000..0xEB000 (4 sectors), presets
 * 0xEB000..0xF1000 (6 sectors); 0xF1000..0xFC000 stays free. The first format kept its presets at 0x93000 (FL_XDATA0, between
 * the app area and Felucca's store): still read there, and erased once the new region has been written. 4 KiB sectors; the stream
 * [header | entries] is cut in 0xF00-byte pieces, one per sector, so bytes 0xF00..0xFFF of every sector stay erased (the
 * update loader clears an "AT"+CRC record there).
 * Header (16 bytes): magic "OCTB", version, payload length, payload CRC-32.
 *   version PS_VER_BANK / PS_VER_SCENES: the entries ctrl.c keeps in RAM (u16 len + header + the sections of state.c), written
 *     and read straight from / to the RAM pool: no copy of the record is made.
 *   version sizeof(preset_v1_t) / 0x10000 | P_COUNT: the first format (the Vactrol's PV[] only); still read (presets at
 *     PS_BASE_V1, scenes at PS_BASE1), converted to the new entries at boot, rewritten in the new format at the next save.
 * Only the sectors the record needs are erased. Pages are programmed from the END of the stream to the start, so the
 * header (which makes the record valid) goes last: a cut power leaves either no readable record or the new one complete -
 * never a half record that passes the CRC. IRQs are off around each flash call (the fl_*_ram functions run from RAM). */
#define PS_BASE   0xEB000u               /* record 0: the presets (FL_XDATA1), 6 sectors: 0xEB000..0xF1000 */
#define PS_SECT   6u
#define PS_BASE1  0xE7000u               /* record 1: the 16 key scenes (FL_XDATA1), 4 sectors: 0xE7000..0xEB000 */
#define PS_SECT1  4u
#define PS_BASE_V1 0x93000u              /* the first format's presets (FL_XDATA0, 4 sectors): read at boot, erased after the first new save */
#define PS_MAX_V1 (4u * PS_PIECE - 16u)
#define PS_PIECE  0xF00u
#define PS_HDR    16u
#define PS_MAGIC  0x4F435442u            /* "OCTB" */
#define PS_MAX0   (PS_SECT * PS_PIECE - PS_HDR)
#define PS_MAX1   (PS_SECT1 * PS_PIECE - PS_HDR)
#define PS_VER_BANK   0x00020001u        /* format 2, revision 1 */
#define PS_VER_SCENES 0x00030001u

typedef char ps_fits[(BANK_POOL == PS_MAX0 && SCENE_POOL == PS_MAX1 && PS_BASE1 + PS_SECT1 * 0x1000u <= PS_BASE && PS_BASE + PS_SECT * 0x1000u <= 0xFC000u) ? 1 : -1];
static uint8_t ps_leg0;                  /* a first-format preset record is still at PS_BASE_V1 */

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

/* the CRC-32 of n bytes at flash stream offset s (PS_HDR..), read in 256-byte pieces */
static int ps_crc_flash(uint32_t base, uint32_t n, uint32_t *crc)
{
    static uint8_t t[256];
    uint32_t c = 0xFFFFFFFFu, s = PS_HDR, i;
    int k;
    while (n) {
        uint32_t m = n > 256u ? 256u : n;
        int rc = ps_read(base, s, t, m);
        if (rc) return rc;
        for (i = 0; i < m; i++) {
            c ^= t[i];
            for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
        s += m; n -= m;
    }
    *crc = ~c;
    return 0;
}

/* 0 = ok: a record with the magic, a payload length <= mx and a good CRC is in the region; *ver, *n describe it */
static int ps_open(uint32_t base, uint32_t mx, uint32_t *ver, uint32_t *n)
{
    uint8_t h[PS_HDR];
    uint32_t crc;
    int rc;
    if (!flash_ok || (rc = ps_read(base, 0, h, PS_HDR)) != 0) return -1;
    if (ps_get32(h) != PS_MAGIC) return -2;
    *ver = ps_get32(h + 4);
    *n = ps_get32(h + 8);
    if (*n > mx) return -3;
    if (ps_crc_flash(base, *n, &crc)) return -4;
    return crc == ps_get32(h + 12) ? 0 : -5;
}

/* 0 = ok. The stream is [16-byte header | p[0..n)]; sectors needed are erased, then the pages go from the last to the first. */
static int ps_write(uint32_t base, uint32_t mx, uint32_t ver, const void *p, uint32_t n)
{
    static uint8_t pg[256];
    uint8_t h[PS_HDR];
    const uint8_t *src = p;
    uint32_t total = PS_HDR + n, nsect = (total + PS_PIECE - 1) / PS_PIECE, i, page, took;
    if (!flash_ok || n > mx) return -1;
    ps_put32(h, PS_MAGIC);
    ps_put32(h + 4, ver);
    ps_put32(h + 8, n);
    ps_put32(h + 12, ps_crc(src, n));
    for (i = 0; i < nsect; i++) {                      /* the record's length tells where it ends: a stale tail beyond is never read */
        uint32_t f = irq_save();
        int rc = FL_FAR(fl_erase4k_ram)(base + i * 0x1000u, &took);
        irq_restore(f);
        if (rc) return rc;
    }
    /* pages from the last to the first; a page never crosses a sector's 0xF00 piece (0xF00 = 15 * 256) */
    for (page = (total + 255u) / 256u; page-- > 0;) {
        uint32_t s = page * 256u, c = total - s, f, j;
        int rc;
        if (c > 256u) c = 256u;
        for (j = 0; j < c; j++) pg[j] = (s + j) < PS_HDR ? h[s + j] : src[s + j - PS_HDR];
        f = irq_save();
        rc = FL_FAR(fl_prog_ram)(ps_addr(base, s), pg, c, &took);
        irq_restore(f);
        if (rc) return rc;
    }
    return 0;
}

/* the controller's hooks: the pools of ctrl.c as they are */
void bank_store(const void *p, uint32_t n)
{
    if (ps_write(PS_BASE, PS_MAX0, PS_VER_BANK, p, n) == 0 && ps_leg0) {      /* the old copy must not come back if the new one is lost */
        uint32_t took, f = irq_save();
        int rc = FL_FAR(fl_erase4k_ram)(PS_BASE_V1, &took);
        irq_restore(f);
        if (rc == 0) ps_leg0 = 0;
    }
}
void scene_store(const void *p, uint32_t n) { (void)ps_write(PS_BASE1, PS_MAX1, PS_VER_SCENES, p, n); }

/* at boot, after ctrl_init (which seeds the factory bank): replace it with the saved ones */
void persist_boot(void)
{
    uint32_t ver, n, cap, o;
    uint8_t *pool;
    if (flash_ok && ps_open(PS_BASE_V1, PS_MAX_V1, &ver, &n) == 0 && ver == (uint32_t)sizeof(preset_v1_t) && n % sizeof(preset_v1_t) == 0) ps_leg0 = 1;
    if (ps_open(PS_BASE, PS_MAX0, &ver, &n) == 0 && ver == PS_VER_BANK) {
        pool = bank_pool(&cap);
        if (n <= cap && ps_read(PS_BASE, PS_HDR, pool, n) == 0) bank_restore(n);
    } else if (ps_leg0 && ps_open(PS_BASE_V1, PS_MAX_V1, &ver, &n) == 0) {
        static uint8_t one[sizeof(preset_v1_t)] __attribute__((aligned(4)));
        bank_restore(0);
        for (o = 0; o < n; o += (uint32_t)sizeof one)
            if (ps_read(PS_BASE_V1, PS_HDR + o, one, (uint32_t)sizeof one) == 0) bank_restore_v1(one);
    }
    if (ps_open(PS_BASE1, PS_MAX1, &ver, &n) == 0) {
        pool = scene_pool(&cap);
        if (ver == PS_VER_SCENES && n <= cap) {
            if (ps_read(PS_BASE1, PS_HDR, pool, n) == 0) scene_restore(n);
        } else if (ver == (0x10000u | P_COUNT) && n == 2u + 32u * P_COUNT) {
            static int16_t sv[P_COUNT];
            uint8_t m[2];
            int w;
            scene_restore(0);
            if (ps_read(PS_BASE1, PS_HDR, m, 2) == 0)
                for (w = 0; w < 16; w++)
                    if (((m[0] | (m[1] << 8)) >> w) & 1)
                        if (ps_read(PS_BASE1, PS_HDR + 2u + 2u * P_COUNT * (uint32_t)w, (uint8_t *)sv, 2u * P_COUNT) == 0) scene_restore_v1(w, sv);
        }
    }
}
