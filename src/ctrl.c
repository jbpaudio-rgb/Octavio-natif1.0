/* SPDX-License-Identifier: MPL-2.0 */
/* The prototype's panel logic, ported line by line from its script (btn(), the buttons' pointer handlers, vk(),
 * savePreset / commitSave / sortBank, showList, scSave / scLoad, toast). */
#include "params.h"
#include "ctrl.h"
#include "ui.h"
#include "voices.h"

#define LATCH_MS 350u            /* a second press this soon after a release latches the button */
#define LIST_MS 1800u
#define SCMODE_MS 8000u
#undef Q
#define Q 16                     /* knob positions: Q16, 0..1 */
#define ONE (1 << Q)

static const char NCH[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -";   /* the name editor's alphabet (39) */
#define NCHN 39

/* ---- state ---- */
static int pg = PGN_GLO, src, sel, fxo, oct, auto_play, rec;
static uint16_t lt, bdn, dn_;                      /* locked (lk), held (bdn), physically down (b.dn): bit CB_* */
static uint32_t lu[CB_COUNT];                      /* each button's last release (0 after a chord) */
static int chord, sel_down, sel_mv;
static uint8_t dv[4];                              /* each track's selected drum voice (the prototype's eng.dv) */
static uint32_t now_ms;
static uint32_t ver;
#include "state.c"
/* the bank (presets): entries packed in bpool, kept sorted by (type, k); an entry is   u16 len | char name[13] | u8 type | u16 k |
 * state body (state.c)   - the same bytes persist.c writes to flash. boff[i] = where entry i starts. */
#define E_HDR 18u
static uint8_t bpool[BANK_POOL];
static uint32_t bpool_n;
static uint16_t boff[BANK_MAX];
static int nbank, pi;                              /* pi: 1-based current, 0 = none */
/* the key scenes: spool holds   u16 len | u8 slot | state body (with the track volumes)   sorted by slot */
#define S_HDR 3u
static uint8_t spool[SCENE_POOL];
static uint32_t spool_n;
static int16_t soff[16];                           /* -1: the scene was never saved */
static uint8_t ebuf[ST_MAX + E_HDR + 8];           /* an entry being built (save) */
static int pp_q, ap_q, pp_ws = -1, ap_ws = -1, sel_ws = -1;   /* the knobs' unrounded positions (Q16), -1 = none */
static int pp_wq, ap_wq, sel_wq;
/* list overlay */
static uint32_t list_until;
static char list_title[20], list_note[24];
static int list_cur, list_n, list_kind;            /* kind 0 presets, 1 algorithm */
/* toast */
static uint32_t toast_until;
static char toast_a[24], toast_b[24];
/* name editor */
static int pend, pend_ow, pend_k, nmP;
static char nmA[NAME_LEN];
/* scenes (banks) */
static int cur_bank = -1, sc_mode;
static uint32_t sc_until;

static void bump(void) { ver++; }
static int clip_q(int32_t n) { return n < 0 ? 0 : n > ONE ? ONE : (int)n; }
static int round_q(int32_t q, int mul) { return (int)((q * mul + (ONE >> 1)) >> Q); }    /* Math.round for n >= 0 */

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void scopy(char *d, const char *s, int max) { int i = 0; while (s[i] && i < max - 1) { d[i] = s[i]; i++; } d[i] = 0; }
static void sappend(char *d, const char *s, int max) { int n = slen(d); scopy(d + n, s, max - n); }
static void itoa2(char *o, int v, int pad)         /* v >= 0, zero padded to `pad` */
{
    char t[8]; int n = 0, i;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n < pad) t[n++] = '0';
    for (i = 0; i < n; i++) o[i] = t[n - 1 - i];
    o[n] = 0;
}

static const char *const ENG_TAG[CENG_N] = { "VACT", "MOOG", "PROP", "DRUM" };   /* dn(): the display name of each engine */
static const char *etag(int t) { return t >= 0 && t < CENG_N ? ENG_TAG[t] : "???"; }
static int cur_engine(void) { return voices_engine(sel); }

/* ---- toast / list ---- */
static void toast(const char *a, const char *b, uint32_t ms)
{
    scopy(toast_a, a, sizeof toast_a);
    scopy(toast_b, b ? b : "", sizeof toast_b);
    toast_until = ms + LIST_MS;
    bump();
}
static void show_list(int kind, const char *title, int cur, int n, const char *note, uint32_t ms)
{
    list_kind = kind;
    scopy(list_title, title, sizeof list_title);
    scopy(list_note, note, sizeof list_note);
    list_cur = cur;
    list_n = n;
    list_until = ms + LIST_MS;
    bump();
}
static void presets_list(const char *note, uint32_t ms)       /* showList("PRESETS", plist(), pi, note) */
{
    char nb[24];
    if (!note) {                                               /* (cur+1) + " / " + n */
        itoa2(nb, pi + 1, 0); sappend(nb, " / ", sizeof nb); { char t[8]; itoa2(t, nbank + 1, 0); sappend(nb, t, sizeof nb); }
        note = nb;
    }
    show_list(0, "PRESETS", pi, nbank + 1, note, ms);
}

/* ---- the bank ---- */
static uint32_t e_len(const uint8_t *e) { return (uint32_t)(e[0] | (e[1] << 8)); }
static const char *bname(int i) { return (const char *)bpool + boff[i] + 2; }
static int btype(int i) { return bpool[boff[i] + 15]; }
static int bk(int i) { return bpool[boff[i] + 16] | (bpool[boff[i] + 17] << 8); }
static uint32_t blen(int i) { return e_len(bpool + boff[i]); }
static int b_pos(int type, int k)                  /* where an entry (type, k) goes: after the equal ones */
{
    int j;
    for (j = 0; j < nbank; j++) if (btype(j) > type || (btype(j) == type && bk(j) > k)) break;
    return j;
}
static void b_remove(int i)
{
    uint32_t l = blen(i), o = boff[i], j;
    for (j = o; j + l < bpool_n; j++) bpool[j] = bpool[j + l];
    bpool_n -= l;
    for (j = (uint32_t)i; j + 1 < (uint32_t)nbank; j++) boff[j] = (uint16_t)(boff[j + 1] - l);
    nbank--;
}
static int b_insert(const uint8_t *e, uint32_t l)  /* the index it got, -1: no room */
{
    int at = b_pos(e[15], e[16] | (e[17] << 8)), j;
    uint32_t o = at < nbank ? boff[at] : bpool_n, m;
    if (nbank >= BANK_MAX || bpool_n + l > BANK_POOL) return -1;
    for (m = bpool_n; m > o; m--) bpool[m - 1 + l] = bpool[m - 1];
    for (m = 0; m < l; m++) bpool[o + m] = e[m];
    for (j = nbank; j > at; j--) boff[j] = (uint16_t)(boff[j - 1] + l);
    boff[at] = (uint16_t)o;
    nbank++;
    bpool_n += l;
    return at;
}
int bank_count(void) { return nbank; }
int bank_current(void) { return pi; }

static void load_preset(int i)                     /* loadPreset(): the engines of the tracks and all their values */
{
    state_load(bpool + boff[i] + E_HDR, blen(i) - E_HDR);
    ui_params_changed();
    bump();
}

/* the persistence hooks (flash.c on the target; the host test stubs them) */
#ifdef PERSIST_FLASH
void bank_store(const void *p, uint32_t n);          /* persist.c */
void scene_store(const void *p, uint32_t n);
#else
__attribute__((weak)) void bank_store(const void *p, uint32_t n) { (void)p; (void)n; }
__attribute__((weak)) void scene_store(const void *p, uint32_t n) { (void)p; (void)n; }
#endif
static void save_bank(void) { bank_store(bpool, bpool_n); }

/* ---- name editor ---- */
static void nm_set(const char *t)
{
    int i, n = slen(t);
    for (i = 0; i < NAME_LEN; i++) {
        char c = i < n ? t[i] : ' ';
        int ok = 0, j;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        for (j = 0; j < NCHN; j++) if (NCH[j] == c) ok = 1;
        nmA[i] = ok ? c : ' ';
    }
    nmP = 0;
}
static void save_preset(void)                      /* savePreset(): open the editor */
{
    int i, kmax = 0;
    char nm[NAME_LEN + 1], t[8];
    pend_ow = pi > 0;
    for (i = 0; i < nbank; i++) if (bk(i) > kmax) kmax = bk(i);
    pend_k = kmax + 1;
    pend = 1;
    if (pend_ow) scopy(nm, bname(pi - 1), sizeof nm);
    else { itoa2(t, pend_k, 2); scopy(nm, etag(cur_engine()), sizeof nm); sappend(nm, " ", sizeof nm); sappend(nm, t, sizeof nm); }
    list_until = 0;
    nm_set(nm);
    bump();
}
static void commit_save(uint32_t ms)
{
    char v[NAME_LEN + 1], t[8];
    int i, a = 0, b, k;
    uint32_t bl, l, room;
    if (!pend) return;
    for (i = 0; i < NAME_LEN; i++) v[i] = nmA[i];
    v[NAME_LEN] = 0;
    b = NAME_LEN;
    while (b > 0 && v[b - 1] == ' ') b--;
    while (a < b && v[a] == ' ') a++;
    for (i = 0; i < b - a; i++) v[i] = v[a + i];
    v[b - a] = 0;
    if (!v[0]) { itoa2(t, pend_k, 2); scopy(v, etag(cur_engine()), sizeof v); sappend(v, " ", sizeof v); sappend(v, t, sizeof v); }
    k = pend_ow ? bk(pi - 1) : pend_k;
    for (i = 0; i < (int)E_HDR; i++) ebuf[i] = 0;
    for (i = 0; i < NAME_LEN && v[i]; i++) ebuf[2 + i] = (uint8_t)v[i];
    ebuf[15] = (uint8_t)cur_engine();
    ebuf[16] = (uint8_t)k; ebuf[17] = (uint8_t)(k >> 8);
    bl = state_save(ebuf + E_HDR, sizeof ebuf - E_HDR, 0);
    l = E_HDR + bl;
    room = BANK_POOL - bpool_n + (pend_ow ? blen(pi - 1) : 0);
    pend = 0;
    if (!bl || l > room || (!pend_ow && nbank >= BANK_MAX)) {      /* nothing is changed: the old preset stays */
        toast("BANK FULL", "NOT SAVED", ms);
        return;
    }
    ebuf[0] = (uint8_t)l; ebuf[1] = (uint8_t)(l >> 8);
    if (pend_ow) b_remove(pi - 1);
    pi = b_insert(ebuf, l) + 1;
    save_bank();
    {   /* showList("PRESETS", plist(), pi, "SAVED · VACT") */
        char nb[24];
        scopy(nb, "SAVED \x02 ", sizeof nb); sappend(nb, etag(cur_engine()), sizeof nb);
        presets_list(nb, ms);
    }
}

/* ---- scenes: the bank of keys (HOME held, or SAVE BANK): all 4 tracks, like the prototype's scSave ---- */
static uint16_t scn_used;                          /* bit w: scene w was saved, its entry starts at soff[w] */
static void s_reindex(void)                        /* walk spool: offsets, used mask; a damaged tail is dropped */
{
    uint32_t o = 0;
    int last = -1;
    scn_used = 0;
    while (o + S_HDR <= spool_n) {
        uint32_t l = e_len(spool + o);
        int w = spool[o + 2];
        if (l < S_HDR || o + l > spool_n || w > 15 || w <= last) break;
        soff[w] = (int16_t)o; scn_used |= (uint16_t)(1u << w); last = w;
        o += l;
    }
    spool_n = o;
}
static void s_remove(int w)
{
    uint32_t l = e_len(spool + soff[w]), j;
    for (j = (uint32_t)soff[w]; j + l < spool_n; j++) spool[j] = spool[j + l];
    spool_n -= l;
    s_reindex();
}
static int s_insert(const uint8_t *e, uint32_t l)  /* 0 = no room; the slot must not be in the pool */
{
    int w = e[2], j;
    uint32_t o = spool_n, m;
    if (spool_n + l > SCENE_POOL) return 0;
    for (j = w + 1; j < 16; j++) if ((scn_used >> j) & 1) { o = (uint32_t)soff[j]; break; }
    for (m = spool_n; m > o; m--) spool[m - 1 + l] = spool[m - 1];
    for (m = 0; m < l; m++) spool[o + m] = e[m];
    spool_n += l;
    s_reindex();
    return 1;
}
static void save_scenes(void) { scene_store(spool, spool_n); }
static void sc_save(int w, uint32_t ms)
{
    char a[16];
    uint32_t bl, l, old = (scn_used >> w) & 1 ? e_len(spool + soff[w]) : 0;
    sc_mode = 0;
    ebuf[2] = (uint8_t)w;
    bl = state_save(ebuf + S_HDR, sizeof ebuf - S_HDR, 1);
    l = S_HDR + bl;
    if (!bl || spool_n - old + l > SCENE_POOL) { toast("BANK FULL", "NOT SAVED", ms); return; }
    ebuf[0] = (uint8_t)l; ebuf[1] = (uint8_t)(l >> 8);
    if (old) s_remove(w);
    s_insert(ebuf, l);
    save_scenes();
    cur_bank = w;
    scopy(a, "BANK ", sizeof a); { char t[4]; itoa2(t, w + 1, 0); sappend(a, t, sizeof a); } sappend(a, " SAVED", sizeof a);
    toast(a, "4 TRACKS", ms);
}
static void sc_load(int w, uint32_t ms)             /* scLoad(): an empty bank resets the tracks to their defaults ("DEFAULT") */
{
    char a[16];
    int had = (scn_used >> w) & 1;
    cur_bank = w;
    if (had) state_load(spool + soff[w] + S_HDR, e_len(spool + soff[w]) - S_HDR);
    else state_defaults();
    ui_params_changed();
    scopy(a, "BANK ", sizeof a); { char t[4]; itoa2(t, w + 1, 0); sappend(a, t, sizeof a); }
    toast(a, had ? "LOADED" : "DEFAULT", ms);
    pi = 0;
    bump();
}
int ctrl_bank_mode(void) { return ((bdn >> CB_HOME) & 1) || sc_mode; }
int ctrl_bank_cur(void) { return cur_bank; }
int ctrl_bank_key(int w, uint32_t ms)
{
    if (!ctrl_bank_mode() || w < 0 || w > 15) return 0;
    chord = 1;
    if (sc_mode) sc_save(w, ms); else sc_load(w, ms);
    bump();
    return 1;
}

/* ---- buttons ---- */

static void btn(int t, uint32_t ms)                /* btn(): a button's action (the Vactrol engine) */
{
    if (t == CB_SAVE && ((bdn >> CB_HOME) & 1) && !pend) {
        chord = 1;
        if (cur_bank >= 0) { sc_save(cur_bank, ms); return; }
        sc_mode = 1; sc_until = ms + SCMODE_MS;
        toast("SAVE BANK", "PRESS A KEY", ms);
        return;
    }
    if (pend) {
        if (t == CB_SEL) { if (nmP < NAME_LEN - 1) nmP++; bump(); return; }
        if (t == CB_SAVE) { commit_save(ms); return; }
        if (t == CB_HOME) { pend = 0; bump(); }
    }
    if (t == CB_ENV) pg = PGN_ENV;
    else if (t == CB_EDIT) pg = cur_engine() == ENG_VAC ? PGN_GLO : PGN_EDIT;     /* Vactrol: EDIT shows the FM page, GLO the global one; the others: EDIT = filter / drum voice, GLO = mixer */
    else if (t == CB_GLO) pg = cur_engine() == ENG_VAC ? PGN_EDIT : cur_engine() == ENG_DRUM ? (pg == PGN_GLO ? PGN_GLO2 : PGN_GLO) : PGN_GLO;
    else if (t == CB_FX) { if (pg == PGN_FX) fxo = (fxo + 1) % FX_NSLOT; else pg = PGN_FX; }
    else if (t == CB_LFO) { if (pg == PGN_LFO) src = (src + 1) % 6; else pg = PGN_LFO; }
    else if (t == CB_HOME) pg = PGN_HOME;
    else if (t == CB_SEQ) pg = PGN_SEQ;
    else if (t == CB_PLAY) { auto_play = !auto_play; }
    else if (t == CB_REC) { rec = !rec; }
    else if (t == CB_ARP) pg = PGN_ARP;
    else if (t == CB_SEL && pg == PGN_FXO) fxo = (fxo + 1) % FX_NSLOT;
    else if (t == CB_SAVE) { save_preset(); return; }
    else return;                                    /* "Not assigned" */
    bump();
    ui_goto(pg);
}

static int latchable(int t) { return t == CB_HOME || t == CB_EDIT || t == CB_GLO || t == CB_LFO || t == CB_FX || t == CB_ENV || t == CB_ARP || t == CB_SEQ; }
#define LATCHMASK ((1u << CB_HOME) | (1u << CB_EDIT) | (1u << CB_GLO) | (1u << CB_LFO) | (1u << CB_FX) | (1u << CB_ENV) | (1u << CB_ARP) | (1u << CB_SEQ))

void ctrl_button(int t, int down, uint32_t ms)
{
    now_ms = ms;
    if (t < 0 || t >= CB_COUNT) return;
    if (latchable(t)) {
        if (down) {                                 /* onpointerdown */
            if ((lt >> t) & 1) {                    /* a locked button: this press unlocks it */
                lt &= (uint16_t)~(1u << t); bdn &= (uint16_t)~(1u << t);
                if (!(bdn & LATCHMASK)) chord = 0;
                bump();
                return;
            }
            dn_ |= (uint16_t)(1u << t);
            if (ms - lu[t] < LATCH_MS && !chord) { lt |= (uint16_t)(1u << t); }
            bdn |= (uint16_t)(1u << t);
            if (((bdn >> CB_HOME) & 1) && ((bdn >> CB_EDIT) & 1)) { chord = 1; pg = pg == PGN_MENU ? PGN_MENU2 : PGN_MENU; ui_goto(pg); }
            else if (((bdn >> CB_HOME) & 1) && ((bdn >> CB_GLO) & 1)) { chord = 1; pg = PGN_TEMPO; ui_goto(pg); }
            bump();
        } else {                                    /* onpointerup */
            if (!((dn_ >> t) & 1)) return;
            dn_ &= (uint16_t)~(1u << t);
            lu[t] = chord ? 0u : ms;
            if ((lt >> t) & 1) return;
            bdn &= (uint16_t)~(1u << t);
            if (!chord) btn(t, ms);
            if (!(bdn & LATCHMASK)) chord = 0;
            bump();
        }
    } else if (t == CB_SEL) {
        if (down) { sel_down = 1; sel_mv = 0; }
        else if (sel_down) { sel_down = 0; if (!sel_mv) btn(CB_SEL, ms); }
    } else if (!down) {
        btn(t, ms);                                 /* onclick */
    }
}

void ctrl_octave(int d) { oct = oct + d < -2 ? -2 : oct + d > 2 ? 2 : oct + d; bump(); }

/* ---- the side knobs: vk(): wheel steps of the knob's `wstep`, from the unrounded position ---- */
static void side_wheel(int steps, int wstep_q, int32_t (*get)(void), void (*set)(int32_t), int *ws, int *wq)
{
    int i, n = steps < 0 ? -steps : steps;
    for (i = 0; i < n; i++) {
        int32_t g = get();
        int32_t base = (*ws >= 0 && g == *wq) ? *ws : g;
        int32_t nv = clip_q(base + (steps < 0 ? -(int32_t)wstep_q : (int32_t)wstep_q));
        *ws = (int)nv;
        set(nv);
        *wq = (int)get();
    }
}
static uint32_t g_ms;
/* PRESETS: n * bank.length rounds to the entry (0 = none) */
static int32_t pk_get(void)
{
    if (round_q(pp_q, nbank) != pi) pp_q = (int)(((int32_t)pi << Q) / (nbank > 1 ? nbank : 1));
    return pp_q;
}
static void pk_set(int32_t n)
{
    int i = round_q(n, nbank);
    pp_q = (int)n;
    if (i != pi) { pi = i; if (i > 0) load_preset(i - 1); }
    presets_list(nbank ? "" : "PRESS SAVE", g_ms);
}
/* ALGORITHM: the engine of the track (one engine so far) */
static int32_t ak_get(void) { return ((int32_t)cur_engine() << Q) / (CENG_N - 1); }
static void ak_set(int32_t n)
{
    int i = round_q(n, CENG_N - 1);
    char t[20], num[4], nb[24];
    if (i != cur_engine()) { voices_set_engine(sel, i); PV[P_TYP0 + (sel & 3)] = (int16_t)i; ui_params_changed(); }
    ap_q = (int)n;
    scopy(t, "ALGO \x02 T", sizeof t); itoa2(num, sel + 1, 0); sappend(t, num, sizeof t);
    scopy(nb, "TRACK ", sizeof nb); sappend(nb, num, sizeof nb);
    show_list(1, t, cur_engine(), CENG_N, nb, g_ms);
}
/* SELECT: the name editor's letter, HOME's track, else the LFO source */
static int32_t sk_get(void)
{
    if (pend) { int j, k = 0; for (j = 0; j < NCHN; j++) if (NCH[j] == nmA[nmP]) { k = j; break; } return ((int32_t)k << Q) / (NCHN - 1); }
    if (pg == PGN_HOME) return ((int32_t)sel << Q) / 3;
    return ((int32_t)src << Q) / 5;
}
static void sk_set(int32_t n)
{
    if (pend) { nmA[nmP] = NCH[round_q(n, NCHN - 1)]; bump(); return; }
    if (pg == PGN_HOME) { int v = round_q(n, 3); sel = v < 0 ? 0 : v > 3 ? 3 : v; ui_goto(pg); bump(); return; }
    src = round_q(n, 5);
    if (pg == PGN_LFO) ui_goto(pg);
    bump();
}

void ctrl_side(int role, int steps, uint32_t ms)
{
    g_ms = ms;
    if (role == ROLE_PRESETS) side_wheel(steps, ONE / 50, pk_get, pk_set, &pp_ws, &pp_wq);
    else if (role == ROLE_ALGO) side_wheel(steps, ONE / 50, ak_get, ak_set, &ap_ws, &ap_wq);
    else {
        int w = pend ? ONE / (NCHN - 1) : pg == PGN_HOME ? ONE / 3 : ONE / 50;
        side_wheel(steps, w, sk_get, sk_set, &sel_ws, &sel_wq);
    }
}

void ctrl_tick(uint32_t ms)
{
    now_ms = ms;
    if (list_until && (int32_t)(ms - list_until) >= 0) { list_until = 0; bump(); }
    if (toast_until && (int32_t)(ms - toast_until) >= 0) { toast_until = 0; bump(); }
    if (sc_mode && (int32_t)(ms - sc_until) >= 0) { sc_mode = 0; bump(); }
}

void ctrl_init(uint32_t ms)
{
    int i;
    pg = PGN_GLO; src = sel = fxo = oct = 0; auto_play = rec = 0; lt = bdn = dn_ = 0; chord = 0; pend = 0;
    for (i = 0; i < CB_COUNT; i++) lu[i] = ms - 1000u;
    pp_ws = ap_ws = sel_ws = -1;
    cur_bank = -1; sc_mode = 0;
    state_init();                                  /* the engines and the sequencer hold their factory values now */
    ver++;
}

/* ---- state for the pages and the overlays ---- */
static void norm_pg(void)                          /* page(): a page the track's engine does not have falls back */
{
    int e = cur_engine();
    if (pg == PGN_GLO2 && e != ENG_DRUM) pg = PGN_GLO;
    if (e == ENG_DRUM) {
        if (pg == PGN_ENV2 || (pg >= PGN_OSC1 && pg <= PGN_OSC4)) pg = PGN_EDIT;
    } else if (e == ENG_VAC) {
        if (pg == PGN_ENV2) pg = PGN_ENV;
        if (pg >= PGN_OSC1 && pg <= PGN_OSC4) pg = PGN_GLO;
    }
}
int ctrl_page(void) { norm_pg(); return pg; }
int ctrl_drum_voice(void) { return dv[sel & 3]; }
int ctrl_op_key(int o)                              /* the prototype's black-key chords: EDIT / GLO + OP1..4 = OSC pages, ENV + OP1/2 = ENV / ENV2, drums: ENV / EDIT + OP1..6 = voice */
{
    int e = cur_engine(), h_edit = (bdn >> CB_EDIT) & 1, h_glo = (bdn >> CB_GLO) & 1, h_env = (bdn >> CB_ENV) & 1;
    if (((bdn >> CB_FX) & 1) && (pg == PGN_FX || pg == PGN_FXO) && o < FX_NSLOT) {       /* FX held + OP1..: the slot of the selected track (the prototype has six, native FX_NSLOT) */
        chord = 1; fxo = o; pg = PGN_FX;
    } else if (((bdn >> CB_FX) & 1) && o == 7) {                                       /* FX + OP8: the delay's tempo sync (toggleSync("FX")) */
        chord = 1; PV[P_F5SYNC] = !PV[P_F5SYNC]; ui_params_changed();
    } else if (o == 7 && !h_edit && !h_glo && !h_env) {                                  /* OP8: FX ORDER */
        pg = PGN_FXO;
    } else if (o < 4 && (e == ENG_MOOG || e == ENG_PROPH) && (h_edit || h_glo)) {
        int np = PGN_OSC1 + o;
        chord = 1; pg = pg == np ? PGN_GLO : np;
    } else if ((pg == PGN_ENV || pg == PGN_ENV2) && h_env && o < 2 && (o == 0 || e == ENG_MOOG || e == ENG_PROPH)) {
        chord = 1; pg = o ? PGN_ENV2 : PGN_ENV;
    } else if (e == ENG_DRUM && h_env && o < 6) {
        chord = 1; dv[sel & 3] = (uint8_t)o; pg = PGN_ENV;
    } else if (e == ENG_DRUM && h_edit && o < 6) {
        chord = 1; dv[sel & 3] = (uint8_t)o; pg = PGN_EDIT;
    } else
        return 0;
    bump();
    ui_goto(pg);
    return 1;
}
void ctrl_cpu_full(uint32_t ms) { toast("CPU FULL", "FX BYPASSED", ms); bump(); }
int ctrl_fx_key(int w, uint32_t ms)                 /* a white key with FX held on the FX page: effect w (0..15) into the selected slot of the selected track; 1 = used */
{
    if (!((bdn >> CB_FX) & 1) || pg != PGN_FX || w < 0 || w > 15) return 0;
    chord = 1;
    fx_assign(fxo, w);
    bump(); ui_goto(pg);
    return 1;
}
int ctrl_src(void) { return src; }
int ctrl_track(void) { return sel; }
int ctrl_fx_slot(void) { return fxo; }
int ctrl_octave_val(void) { return oct; }
int ctrl_playing(void) { return auto_play; }
int ctrl_rec(void) { return rec; }
uint32_t ctrl_locked(void) { return lt; }
uint32_t ctrl_lit(void)                             /* #bt .on: the page's button, PLAY/STOP while playing, REC while recording; lk: locks */
{
    uint32_t m = lt;
    int e = cur_engine();
    if (pg == PGN_ENV || pg == PGN_ENV2) m |= 1u << CB_ENV;
    else if (pg == PGN_GLO) m |= 1u << (e == ENG_VAC ? CB_EDIT : CB_GLO);        /* PNf(): the button the page is named after */
    else if (pg == PGN_GLO2 || (pg >= PGN_OSC1 && pg <= PGN_OSC4)) m |= 1u << CB_GLO;
    else if (pg == PGN_EDIT) m |= 1u << (e == ENG_VAC ? CB_GLO : CB_EDIT);
    else if (pg == PGN_LFO) m |= 1u << CB_LFO;
    else if (pg == PGN_FX) m |= 1u << CB_FX;
    else if (pg == PGN_ARP) m |= 1u << CB_ARP;
    else if (pg == PGN_SEQ) m |= 1u << CB_SEQ;
    else if (pg == PGN_HOME) m |= 1u << CB_HOME;
    if (auto_play) m |= 1u << CB_PLAY;
    if (rec) m |= 1u << CB_REC;
    return m;
}
int ctrl_overlay(void) { return pend ? OV_NAME : list_until ? OV_LIST : OV_NONE; }

int ctrl_list_rows(char rows[5][24], char tags[5][8], int cur[1], char *title, char *note)
{
    int n = list_n, a = n <= 5 ? 0 : (list_cur - 2 < 0 ? 0 : list_cur - 2 > n - 5 ? n - 5 : list_cur - 2), i, cnt = 0;
    scopy(title, list_title, 20);
    scopy(note, list_note, 24);
    cur[0] = list_cur - a;
    for (i = a; i < n && cnt < 5; i++, cnt++) {
        tags[cnt][0] = 0;
        if (list_kind == 1) scopy(rows[cnt], etag(i), 24);
        else if (i == 0) scopy(rows[cnt], "+ NEW", 24);
        else { scopy(rows[cnt], bname(i - 1), 24); scopy(tags[cnt], etag(btype(i - 1)), 8); }
    }
    return cnt;
}
int ctrl_name(char *text, int *pos, char *engine_line)
{
    int i;
    if (!pend) return 0;
    for (i = 0; i < NAME_LEN; i++) text[i] = nmA[i];
    text[NAME_LEN] = 0;
    *pos = nmP;
    scopy(engine_line, "ENGINE: ", 32); sappend(engine_line, etag(cur_engine()), 32);
    if (pend_ow) sappend(engine_line, " \x02 OVERWRITE", 32);
    return 1;
}
int ctrl_toast(char *a, char *b)
{
    if (!toast_until) return 0;
    scopy(a, toast_a, 24); scopy(b, toast_b, 24);
    return 1;
}
uint32_t ctrl_version(void) { return ver; }

/* the records' storage: persist.c reads flash straight into the pools, then asks for them to be checked and indexed */
uint8_t *bank_pool(uint32_t *cap) { *cap = BANK_POOL; return bpool; }
uint8_t *scene_pool(uint32_t *cap) { *cap = SCENE_POOL; return spool; }
void bank_restore(uint32_t n)                      /* bpool holds n bytes; n = 0 empties the bank */
{
    uint32_t o = 0;
    nbank = 0;
    while (o + E_HDR <= n && nbank < BANK_MAX) {
        uint32_t l = e_len(bpool + o);
        int j;
        if (l < E_HDR || o + l > n || bpool[o + 15] >= CENG_N) break;
        for (j = 0; j < NAME_LEN && bpool[o + 2 + j]; j++) ;
        if (bpool[o + 2 + j]) break;                  /* name not terminated: erased flash / a foreign record */
        boff[nbank++] = (uint16_t)o;
        o += l;
    }
    bpool_n = o;
    pi = 0;
}
void scene_restore(uint32_t n)
{
    spool_n = n;
    s_reindex();
}
/* the first format (Vactrol only): one preset_t at a time, converted to an entry holding the PV sections. bank_restore(0) first. */
typedef struct { char n[NAME_LEN + 1]; uint8_t type; uint16_t k; int16_t v[P_COUNT]; } preset_v1_t;
void bank_restore_v1(const void *p)
{
    const preset_v1_t *s = p;
    uint32_t bl, l;
    int j;
    if (s->n[0] == (char)0xFF || s->type >= CENG_N) return;
    for (j = 0; j < NAME_LEN && s->n[j]; j++) ;
    if (s->n[j]) return;
    for (j = 0; j < (int)E_HDR; j++) ebuf[j] = 0;
    for (j = 0; j < NAME_LEN && s->n[j]; j++) ebuf[2 + j] = (uint8_t)s->n[j];
    ebuf[15] = s->type; ebuf[16] = (uint8_t)s->k; ebuf[17] = (uint8_t)(s->k >> 8);
    bl = state_save_pv(ebuf + E_HDR, sizeof ebuf - E_HDR, s->v, 0);
    l = E_HDR + bl;
    if (!bl) return;
    ebuf[0] = (uint8_t)l; ebuf[1] = (uint8_t)(l >> 8);
    b_insert(ebuf, l);
    pi = 0;
}
void scene_restore_v1(int w, const int16_t *v)      /* scene_restore(0) first */
{
    uint32_t bl, l;
    if (w < 0 || w > 15 || ((scn_used >> w) & 1)) return;
    ebuf[2] = (uint8_t)w;
    bl = state_save_pv(ebuf + S_HDR, sizeof ebuf - S_HDR, v, 1);
    l = S_HDR + bl;
    if (!bl) return;
    ebuf[0] = (uint8_t)l; ebuf[1] = (uint8_t)(l >> 8);
    s_insert(ebuf, l);
}
