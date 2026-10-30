/* SPDX-License-Identifier: MPL-2.0 */
/* Host bench of the modulation "listening test" (NOTES-K): the same sources as main.c (engines, matrix, effects) driven the
 * way the audio IRQ does (fx_begin, mod_apply, fx_admit, voices_render, mod_restore, blocks of 128), from a script.
 *   modtest SCRIPT OUT.f32 [PROBE.csv]
 * Script (one command per line, '#' comments; "at SEC cmd..." delays a command to that time; "end SEC" ends the run):
 *   eng T E                  engine of track T (0 Vactrol 1 Minimoog 2 Prophet 3 Drums); T 0 is the selected track (its matrix is PV[])
 *   pv NAME VAL              PV[P_NAME] = VAL (thousandths)
 *   ep T NAME VAL            engine parameter of track T (MG_* PR_* DR_* ARP_*; DR_TUNE.2 = voice 2)
 *   lfo T S DST SHAPE RATE AMT SYNC   source S (0..3 LFO1..4, 4..5 S&H1..2) of track T: master destination NAME (or number), shape (S&H: smoothing),
 *                            rate / amount in thousandths, sync 0/1
 *   fx T SLOT E              effect E in slot SLOT of track T's chain;  fxk T NAME VAL  one of its knobs
 *   note T N V | hit T V     key down (V 1) / up (V 0) of track T;  a drum voice hit
 *   arp T NAME VAL           arpeggiator parameter (ARP_*)
 *   dg T V BITS              drum lane of voice V (bit s = step s)
 *   play 0|1 | bpm N | sqg T BITS | trkvol T VAL | seq STEP LINE VAL
 *   qline T L DST KEY AMT GLIDE     sequencer line L of track T (DST: 0 none 1..3 pitch, else master + 3)
 *   probe pv NAME | probe ep T NAME | probe fx T NAME | probe lvl T | probe sqg T      recorded each block after mod_apply into PROBE.csv
 * Output: float32 mono, 44,100 Hz, the master mix before the panel's master gain.  */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
static uint32_t tick_fake;
static inline uint32_t fm1_ticks(void) { return tick_fake += 3; }
#include "../src/octavio.h"
#include "../src/params.h"
#include "../src/ui.h"
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
#include "modnames.h"

void ui_params_changed(void) {}
static int ctrl_bank_key_cb(int key) { (void)key; return 0; }
static void ctrl_key_rel_cb(int key) { (void)key; }

static int lookup(const void *tab, const char *n, int *found)
{
    const struct { const char *n; int v; } *e = tab;
    for (; e->n; e++) if (!strcmp(e->n, n)) { *found = 1; return e->v; }
    *found = 0; return 0;
}
static int name_pv(const char *n) { int f; int v = lookup(PVN, n[0] == 'P' && n[1] == '_' ? n : (char[64]){0}, &f); if (!f) { char b[80]; snprintf(b, sizeof b, "P_%s", n); v = lookup(PVN, b, &f); } if (!f) { fprintf(stderr, "unknown pv %s\n", n); exit(2); } return v; }
static int name_ep(const char *n)
{
    char b[64]; int f, v, voice = -1;
    char *dot;
    snprintf(b, sizeof b, "%s", n);
    dot = strchr(b, '.');
    if (dot) { *dot = 0; voice = atoi(dot + 1); }
    v = lookup(EPN, b, &f);
    if (!f) { fprintf(stderr, "unknown engine parameter %s\n", n); exit(2); }
    return voice >= 0 ? DR_ID(voice, v) : v;
}
static int name_dst(const char *n)
{
    int f, v;
    if (n[0] >= '0' && n[0] <= '9') return atoi(n);
    v = lookup(DSN, n, &f);
    if (!f) { fprintf(stderr, "unknown destination %s\n", n); exit(2); }
    return v;
}
static int fxk_index(const char *n)             /* P_F6SIZE -> index in FXT[].p */
{
    return name_pv(n) - P_F0DRIVE;
}
/* every parameter the matrix touched: min / max over the run of its value right after mod_apply, against the base after mod_restore */
enum { CH_PV = 0, CH_MG = CH_PV + P_COUNT, CH_PR = CH_MG + 4 * MG_COUNT, CH_DR = CH_PR + 4 * PR_COUNT, CH_ARP = CH_DR + 4 * DR_COUNT, CH_FX = CH_ARP + 4 * ARP_COUNT, CH_LVL = CH_FX + 4 * FX_NP, CH_SQG = CH_LVL + 4, CH_N = CH_SQG + 4 };
static int chmin[CH_N], chmax[CH_N], chbase[CH_N], chinit;
static void ch_snap(int *v)
{
    int t, i;
    for (i = 0; i < P_COUNT; i++) v[CH_PV + i] = PV[i];
    for (t = 0; t < 4; t++) {
        for (i = 0; i < MG_COUNT; i++) v[CH_MG + t * MG_COUNT + i] = moog_get(t, i);
        for (i = 0; i < PR_COUNT; i++) v[CH_PR + t * PR_COUNT + i] = proph_get(t, i);
        for (i = 0; i < DR_COUNT; i++) v[CH_DR + t * DR_COUNT + i] = drum_get(t, i);
        for (i = 0; i < ARP_COUNT; i++) v[CH_ARP + t * ARP_COUNT + i] = voices_arp_get(t, i);
        for (i = 0; i < FX_NP; i++) v[CH_FX + t * FX_NP + i] = FEFF[t][i];
        v[CH_LVL + t] = TK[t].lvl;
        v[CH_SQG + t] = sq_glen[t];
    }
}
static void ch_name(int k, char *b)
{
    int t;
    if (k < CH_MG) { for (t = 0; PVN[t].n; t++) if (PVN[t].v == k) { snprintf(b, 64, "%s", PVN[t].n); return; } snprintf(b, 64, "PV%d", k); }
    else if (k < CH_PR) snprintf(b, 64, "t%d MG[%d]", (k - CH_MG) / MG_COUNT, (k - CH_MG) % MG_COUNT);
    else if (k < CH_DR) snprintf(b, 64, "t%d PR[%d]", (k - CH_PR) / PR_COUNT, (k - CH_PR) % PR_COUNT);
    else if (k < CH_ARP) snprintf(b, 64, "t%d DR[v%d,%d]", (k - CH_DR) / DR_COUNT, ((k - CH_DR) % DR_COUNT) / DR_PER, ((k - CH_DR) % DR_COUNT) % DR_PER);
    else if (k < CH_FX) snprintf(b, 64, "t%d ARP[%d]", (k - CH_ARP) / ARP_COUNT, (k - CH_ARP) % ARP_COUNT);
    else if (k < CH_LVL) { t = (k - CH_FX) / FX_NP; snprintf(b, 64, "t%d FX[%d]", t, (k - CH_FX) % FX_NP); }
    else if (k < CH_SQG) snprintf(b, 64, "t%d LEVEL", k - CH_LVL);
    else snprintf(b, 64, "t%d SEQGATE", k - CH_SQG);
}
typedef struct { double at; char line[200]; } ev_t;
static ev_t EV[4096];
static int nev;
static struct { int kind, t, id; } PRB[16];
static int nprb;

static void cmd(char *line)
{
    char *a[12]; int n = 0;
    char *p = strtok(line, " \t\r\n");
    while (p && n < 12) { a[n++] = p; p = strtok(0, " \t\r\n"); }
    if (!n || a[0][0] == '#') return;
    if (!strcmp(a[0], "eng")) { int t = atoi(a[1]), e = atoi(a[2]); voices_set_engine(t, e); if (e == ENG_VAC) voices_set_vac_owner(t); }
    else if (!strcmp(a[0], "pv")) { PV[name_pv(a[1])] = (int16_t)atoi(a[2]); vac_update(); }
    else if (!strcmp(a[0], "ep")) { int t = atoi(a[1]); voices_param(t, voices_engine(t), name_ep(a[2]), atoi(a[3])); }
    else if (!strcmp(a[0], "lfo")) {
        int t = atoi(a[1]), s = atoi(a[2]), v[5];
        v[0] = name_dst(a[3]); v[1] = atoi(a[4]); v[2] = atoi(a[5]); v[3] = atoi(a[6]); v[4] = atoi(a[7]);
        if (t == mod_view) { int k; for (k = 0; k < 5; k++) PV[LFO_BASE[s] + k] = (int16_t)v[k]; }
        else { int k; for (k = 0; k < 5; k++) MODT[t].m[(LFO_BASE[s] - P_M0T) + k] = (int16_t)v[k]; }
    }
    else if (!strcmp(a[0], "qline")) {
        int t = atoi(a[1]), l = atoi(a[2]), v[4];
        v[0] = atoi(a[3]) ; v[1] = atoi(a[4]); v[2] = atoi(a[5]); v[3] = atoi(a[6]);
        { int k; for (k = 0; k < 4; k++) { if (t == mod_view) PV[P_M4T + 4 * l + k] = (int16_t)v[k]; else MODT[t].q[4 * l + k] = (int16_t)v[k]; } }
    }
    else if (!strcmp(a[0], "fx")) { FXT[atoi(a[1])].ord[atoi(a[2])] = (uint8_t)atoi(a[3]); if (atoi(a[1]) == fx_view) {} }
    else if (!strcmp(a[0], "fxk")) { int t = atoi(a[1]), k = fxk_index(a[2]), v = atoi(a[3]); FXT[t].p[k] = (int16_t)v; if (t == fx_view) PV[P_F0DRIVE + k] = (int16_t)v; }
    else if (!strcmp(a[0], "note")) voices_note(atoi(a[1]), atoi(a[2]), atoi(a[3]));
    else if (!strcmp(a[0], "hit")) drum_hit(atoi(a[1]), atoi(a[2]), 32767);
    else if (!strcmp(a[0], "arp")) voices_arp(atoi(a[1]), name_ep(a[2]), atoi(a[3]));
    else if (!strcmp(a[0], "dg")) drum_gates_set(atoi(a[1]), atoi(a[2]), (uint16_t)strtoul(a[3], 0, 0));
    else if (!strcmp(a[0], "play")) voices_play(atoi(a[1]));
    else if (!strcmp(a[0], "bpm")) PV[P_BPM] = (int16_t)atoi(a[1]);
    else if (!strcmp(a[0], "sqg")) SQG[atoi(a[1])] = (uint16_t)strtoul(a[2], 0, 0);
    else if (!strcmp(a[0], "trkvol")) voices_track_level(atoi(a[1]), atoi(a[2]));
    else if (!strcmp(a[0], "seq")) ui_seq_set(atoi(a[2]), atoi(a[1]), atoi(a[3]));
    else if (!strcmp(a[0], "probe")) {
        if (!strcmp(a[1], "pv")) { PRB[nprb].kind = 0; PRB[nprb].id = name_pv(a[2]); }
        else if (!strcmp(a[1], "ep")) { PRB[nprb].kind = 1; PRB[nprb].t = atoi(a[2]); PRB[nprb].id = name_ep(a[3]); }
        else if (!strcmp(a[1], "fx")) { PRB[nprb].kind = 2; PRB[nprb].t = atoi(a[2]); PRB[nprb].id = fxk_index(a[3]); }
        else if (!strcmp(a[1], "lvl")) { PRB[nprb].kind = 3; PRB[nprb].t = atoi(a[2]); }
        else if (!strcmp(a[1], "sqg")) { PRB[nprb].kind = 4; PRB[nprb].t = atoi(a[2]); }
        else if (!strcmp(a[1], "arp")) { PRB[nprb].kind = 5; PRB[nprb].t = atoi(a[2]); PRB[nprb].id = name_ep(a[3]); }
        nprb++;
    }
    else { fprintf(stderr, "unknown command %s\n", a[0]); exit(2); }
}
int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "r"), *o, *pc = argc > 3 ? fopen(argv[3], "w") : 0;
    char line[256];
    double end = 2.0;
    long blk = 0, nblk, e = 0;
    float *buf;
    if (!f) return 1;
    ui_init(); vac_init(); voices_init();
    voices_select(0);
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ') s++;
        if (!strncmp(s, "end ", 4)) { end = atof(s + 4); continue; }
        if (!strncmp(s, "at ", 3)) { char *q; EV[nev].at = strtod(s + 3, &q); snprintf(EV[nev].line, sizeof EV[nev].line, "%s", q); nev++; continue; }
        cmd(s);
    }
    fclose(f);
    vac_update();
    fx_commit();
    fx_load_view();
    nblk = (long)(end * FS / 128.0);
    buf = calloc((size_t)nblk * 128, 4);
    if (pc) { int k; fprintf(pc, "t"); for (k = 0; k < nprb; k++) fprintf(pc, ",p%d", k); fprintf(pc, "\n"); }
    for (blk = 0; blk < nblk; blk++) {
        int32_t tmp[128];
        int i;
        double now = blk * 128.0 / FS;
        while (e < nev && EV[e].at <= now) { char l2[200]; snprintf(l2, sizeof l2, "%s", EV[e].line); cmd(l2); e++; vac_update(); }
        vac_update();
        voices_keys(0);
        fx_begin();
        mod_apply(128);
        fx_admit();
        if (pc) {
            int k;
            fprintf(pc, "%.6f", now);
            for (k = 0; k < nprb; k++) {
                int v = 0;
                switch (PRB[k].kind) {
                case 0: v = PV[PRB[k].id]; break;
                case 1: v = voices_param_get(PRB[k].t, voices_engine(PRB[k].t), PRB[k].id); break;
                case 2: v = FEFF[PRB[k].t][PRB[k].id]; break;
                case 3: v = TK[PRB[k].t].lvl; break;
                case 4: v = sq_glen[PRB[k].t]; break;
                case 5: v = voices_arp_get(PRB[k].t, PRB[k].id); break;
                }
                fprintf(pc, ",%d", v);
            }
            fprintf(pc, "\n");
        }
        {
            static int cur[CH_N];
            int k;
            ch_snap(cur);
            if (!chinit) { for (k = 0; k < CH_N; k++) chmin[k] = chmax[k] = cur[k]; chinit = 1; }
            for (k = 0; k < CH_N; k++) { if (cur[k] < chmin[k]) chmin[k] = cur[k]; if (cur[k] > chmax[k]) chmax[k] = cur[k]; }
        }
        voices_render(tmp, 128);
        mod_restore();
        ch_snap(chbase);
        for (i = 0; i < 128; i++) buf[blk * 128 + i] = tmp[i] / 32768.0f;
    }
    o = fopen(argv[2], "wb");
    fwrite(buf, 4, (size_t)nblk * 128, o);
    fclose(o);
    if (pc) fclose(pc);
    if (getenv("CHG")) {                                 /* the parameters the modulation moved: name, base, min, max */
        FILE *c = fopen(getenv("CHG"), "w");
        int k;
        for (k = 0; k < CH_N; k++)
            if (chmin[k] != chmax[k]) { char nm[64]; ch_name(k, nm); fprintf(c, "%s,%d,%d,%d\n", nm, chbase[k], chmin[k], chmax[k]); }
        fclose(c);
    }
    return 0;
}
