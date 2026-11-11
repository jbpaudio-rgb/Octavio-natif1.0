/* SPDX-License-Identifier: MPL-2.0
 * tools/gen_factory.c - writes src/factory_gen.h: the factory presets of the engines and of the drum machine, as preset records (ctrl.c installs them in the bank).
 *   gcc -O1 -w -Isrc -Ibsp/chip -o /tmp/gen_factory tools/gen_factory.c -lm && /tmp/gen_factory > src/factory_gen.h
 * Each record holds the track list, the engine's parameters on its track and (drums) the sequencer; the other tracks come back at their defaults.  Preset n of an engine has k = FAC_K0 + n, so a preset the user renamed or overwrote is not installed a second time. */
#define NO_FACTORY 1
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "../src/octavio.h"
#include "../src/params.h"
#include "../src/ui.h"
#include "../src/mathfx.c"
#include "../src/tables_gen.c"
#include "../src/assets_gen.c"
#include "../src/gfx.c"
#include "../src/params.c"
#include "../src/fxstate.c"
void fx_track(int t, int32_t *x, int n, int act) { (void)t; (void)x; (void)n; (void)act; }
int fx_awake(int t) { (void)t; return 0; }
#include "../src/vactrol.c"
#include "../src/dst.c"
#include "../src/voices.c"
#include "../src/ctrl.c"
#include "../src/overlay.c"
#include "../src/ui.c"
/* the LED layer on a flat fake key matrix: key id i -> column i, row 1 */
#define FM1_NCOL 41u
static int8_t FM1_KEYMAP[6][FM1_NCOL];
static struct { uint32_t notes, buttons; } fm1_in;
static uint8_t fm1_led[FM1_NCOL], fm1_led_dim[FM1_NCOL], fm1_led_breath[FM1_NCOL];
static int8_t km_init(void) { int p; for (p = 0; p < (int)FM1_NCOL; p++) { FM1_KEYMAP[0][p] = -1; FM1_KEYMAP[1][p] = (int8_t)p; FM1_KEYMAP[2][p] = FM1_KEYMAP[3][p] = FM1_KEYMAP[4][p] = -1; } return 0; }
#include "../src/leds.c"
void ui_params_changed(void) {}
static int ctrl_bank_key_cb(int key) { (void)key; return 0; }
static void ctrl_key_rel_cb(int key) { (void)key; }
void fx_bus(int32_t *x, int n) { (void)x; (void)n; }
typedef struct { int16_t id, v; } kv_t;
typedef struct { const char *name; int eng; const kv_t *p; int kit; uint16_t g[6]; } fp_t;
#define K(...) (const kv_t[]){ __VA_ARGS__, { -1, 0 } }
#define V3P(s, d, a) { V3_PIN(s, d), a }
#define FAC_K0 100

static const fp_t FP[] = {
 /* the Vactrol */
 { "BELL", ENG_VAC, K({P_WAVE,0},{P_FOLD,150},{P_SYM,100},{P_RATIO,3500},{P_FM,380},{P_FENV,650},{P_DECAY,850},{P_EDEC,720},{P_ATK,10}) },
 { "FOLD BASS", ENG_VAC, K({P_FOLD,650},{P_SYM,250},{P_FM,0},{P_DECAY,400},{P_EDEC,450},{P_ATK,3}) },
 { "METAL HIT", ENG_VAC, K({P_RATIO,5170},{P_FM,720},{P_FENV,850},{P_FOLD,350},{P_DECAY,600},{P_EDEC,500},{P_ATK,3}) },
 { "GLASS PAD", ENG_VAC, K({P_FOLD,90},{P_FM,150},{P_RATIO,2000},{P_ATK,650},{P_EDEC,900},{P_DECAY,950},{P_EAD,0}) },
 /* the VCS3: patches (pins) */
 { "RING BELLS", ENG_VC3, K({V3_O1W,100},{V3_O2W,100},{V3_O2T,750},{V3_CUT,800},{V3_RES,250},{V3_ATK,3},{V3_DEC,650},{V3_SUS,0},{V3_REL,600},{V3_LVL,1000},
     V3P(V3S_RING,V3D_FIN,800),V3P(V3S_O1,V3D_FIN,200),V3P(V3S_FILT,V3D_OUT,1000),V3P(V3S_ENV,V3D_CUT,400),V3P(V3S_KEY,V3D_CUT,400)) },
 { "NOISE SWEEP", ENG_VC3, K({V3_O3R,150},{V3_CUT,500},{V3_RES,600},{V3_ATK,300},{V3_DEC,600},{V3_SUS,800},{V3_REL,600},{V3_LVL,1000},
     V3P(V3S_NOI,V3D_FIN,700),V3P(V3S_O1,V3D_FIN,150),V3P(V3S_FILT,V3D_OUT,1000),V3P(V3S_LFO,V3D_CUT,600),V3P(V3S_ENV,V3D_CUT,300),V3P(V3S_KEY,V3D_CUT,300)) },
 { "RESO LEAD", ENG_VC3, K({V3_O1W,500},{V3_O2W,500},{V3_CUT,450},{V3_RES,700},{V3_ATK,10},{V3_DEC,500},{V3_SUS,750},{V3_REL,300},{V3_LVL,1000},{V3_O3R,380},
     V3P(V3S_O1,V3D_FIN,500),V3P(V3S_O2,V3D_FIN,450),V3P(V3S_FILT,V3D_OUT,1000),V3P(V3S_ENV,V3D_CUT,450),V3P(V3S_KEY,V3D_CUT,700),V3P(V3S_FILT,V3D_O1F,40),V3P(V3S_LFO,V3D_O1F,30)) },
 { "SELF TALK", ENG_VC3, K({V3_CUT,500},{V3_RES,600},{V3_ATK,5},{V3_DEC,600},{V3_SUS,700},{V3_REL,350},{V3_LVL,1000},
     V3P(V3S_O1,V3D_O2F,300),V3P(V3S_O2,V3D_O1F,250),V3P(V3S_FILT,V3D_O1F,200),V3P(V3S_FILT,V3D_FIN,250),V3P(V3S_O1,V3D_FIN,350),V3P(V3S_O2,V3D_FIN,350),
     V3P(V3S_FILT,V3D_OUT,1000),V3P(V3S_ENV,V3D_CUT,500),V3P(V3S_KEY,V3D_CUT,500)) },
 /* the Minimoog */
 { "FAT BASS", ENG_MOOG, K({MG_W1,1},{MG_R1,1},{MG_L1,900},{MG_W2,2},{MG_R2,1},{MG_T2,506},{MG_L2,800},{MG_W3,1},{MG_R3,0},{MG_L3,700},{MG_NLEV,0},
     {MG_CUT,330},{MG_RES,420},{MG_ENV,620},{MG_ATK,3},{MG_DEC,320},{MG_SUS,650},{MG_FATK,0},{MG_FDEC,300},{MG_FSUS,100}) },
 { "SOLO LEAD", ENG_MOOG, K({MG_W1,1},{MG_R1,2},{MG_L1,800},{MG_W2,1},{MG_R2,2},{MG_T2,512},{MG_L2,800},{MG_W3,2},{MG_R3,3},{MG_L3,400},{MG_NLEV,0},
     {MG_CUT,650},{MG_RES,350},{MG_ENV,350},{MG_GLIDE,220},{MG_ATK,40},{MG_DEC,600},{MG_SUS,800},{MG_FATK,80},{MG_FDEC,600},{MG_FSUS,600}) },
 { "BRASS", ENG_MOOG, K({MG_W1,1},{MG_R1,2},{MG_L1,800},{MG_W2,1},{MG_R2,2},{MG_T2,514},{MG_L2,800},{MG_W3,1},{MG_R3,2},{MG_T3,488},{MG_L3,700},{MG_NLEV,0},
     {MG_CUT,350},{MG_RES,150},{MG_ENV,650},{MG_ATK,300},{MG_DEC,700},{MG_SUS,800},{MG_FATK,340},{MG_FDEC,650},{MG_FSUS,450}) },
 { "PLUCK", ENG_MOOG, K({MG_W1,2},{MG_R1,2},{MG_L1,850},{MG_W2,1},{MG_R2,2},{MG_T2,507},{MG_L2,600},{MG_L3,0},{MG_NLEV,0},
     {MG_CUT,300},{MG_RES,300},{MG_ENV,760},{MG_ATK,0},{MG_DEC,330},{MG_SUS,0},{MG_FATK,0},{MG_FDEC,300},{MG_FSUS,0}) },
 /* the Prophet */
 { "POLY BRASS", ENG_PROPH, K({PR_WAVE,0},{PR_BWAVE,0},{PR_BTUNE,516},{PR_LA,800},{PR_LB,800},{PR_CUT,380},{PR_RES,150},{PR_ENV,650},
     {PR_ATK,300},{PR_DEC,650},{PR_SUS,800},{PR_REL,350},{PR_FATK,330},{PR_FDEC,650},{PR_FSUS,450},{PR_FREL,350}) },
 { "SOFT PAD", ENG_PROPH, K({PR_WAVE,330},{PR_BWAVE,330},{PR_BTUNE,512},{PR_LA,700},{PR_LB,700},{PR_SL,300},{PR_CUT,420},{PR_RES,100},{PR_ENV,250},
     {PR_ATK,700},{PR_DEC,700},{PR_SUS,850},{PR_REL,750},{PR_FATK,650},{PR_FDEC,800},{PR_FSUS,600},{PR_FREL,750}) },
 { "SYNC LEAD", ENG_PROPH, K({PR_WAVE,1000},{PR_BTUNE,560},{PR_LA,800},{PR_LB,700},{PR_CUT,600},{PR_RES,300},{PR_ENV,400},{PR_PM,150},
     {PR_ATK,20},{PR_DEC,600},{PR_SUS,800},{PR_REL,300},{PR_FATK,60},{PR_FDEC,500},{PR_FSUS,500},{PR_FREL,300}) },
 { "POLY PLUCK", ENG_PROPH, K({PR_BTUNE,505},{PR_CUT,300},{PR_RES,250},{PR_ENV,750},{PR_ATK,0},{PR_DEC,300},{PR_SUS,0},{PR_REL,300},{PR_FATK,0},{PR_FDEC,280},{PR_FSUS,0},{PR_FREL,300}) },
 /* the Juno-106 */
 { "WARM PAD", ENG_JUN, K({JN_SAW,600},{JN_PUL,400},{JN_PW,500},{JN_PWM,450},{JN_LR,220},{JN_SUB,150},{JN_CUT,460},{JN_RES,120},{JN_ENV,180},{JN_KYB,800},
     {JN_ATK,650},{JN_DEC,700},{JN_SUS,850},{JN_REL,750},{JN_CHO,3},{JN_DET,250}) },
 { "JUNO BASS", ENG_JUN, K({JN_SAW,300},{JN_PUL,500},{JN_SUB,800},{JN_CUT,300},{JN_RES,280},{JN_ENV,550},{JN_ATK,0},{JN_DEC,350},{JN_SUS,350},{JN_REL,120},{JN_CHO,0},{JN_DET,100}) },
 { "PWM STRINGS", ENG_JUN, K({JN_SAW,0},{JN_PUL,800},{JN_PW,500},{JN_PWM,600},{JN_LR,280},{JN_CUT,600},{JN_RES,100},{JN_ENV,100},
     {JN_ATK,450},{JN_DEC,700},{JN_SUS,900},{JN_REL,550},{JN_CHO,2},{JN_DET,300}) },
 { "SAW STAB", ENG_JUN, K({JN_SAW,900},{JN_PUL,0},{JN_SUB,300},{JN_HPF,150},{JN_CUT,400},{JN_RES,250},{JN_ENV,650},{JN_ATK,0},{JN_DEC,380},{JN_SUS,250},{JN_REL,200},{JN_CHO,1}) },
 /* the DFAM */
 { "KICK", ENG_DFM, K({DF_V1F,250},{DF_V1W,0},{DF_V1E,650},{DF_VDC,380},{DF_V1L,900},{DF_V2L,0},{DF_NSE,0},{DF_CUT,700},{DF_RES,100},{DF_FEG,100},{DF_FDC,400},{DF_ADC,480}) },
 { "SNARE", ENG_DFM, K({DF_V1F,560},{DF_V1W,0},{DF_V1E,350},{DF_V2F,640},{DF_V2W,1},{DF_V2E,300},{DF_V1L,600},{DF_V2L,450},{DF_NSE,700},{DF_VDC,250},
     {DF_CUT,750},{DF_RES,150},{DF_FEG,300},{DF_FDC,300},{DF_ADC,340}) },
 { "LASER", ENG_DFM, K({DF_V1F,600},{DF_V1W,1},{DF_V1E,1000},{DF_V2F,650},{DF_V2W,0},{DF_V2E,900},{DF_VDC,560},{DF_SYN,1},{DF_FM,450},{DF_V1L,800},{DF_V2L,600},
     {DF_CUT,800},{DF_RES,300},{DF_FEG,0},{DF_ADC,600}) },
 { "HAT", ENG_DFM, K({DF_MODE,1},{DF_CUT,800},{DF_RES,150},{DF_NSE,950},{DF_V1L,250},{DF_V2L,250},{DF_V1F,800},{DF_V2F,860},{DF_V1W,1},{DF_V2W,1},{DF_FM,600},
     {DF_ADC,200},{DF_FDC,200},{DF_FEG,200},{DF_VDC,100}) },
 /* the drum machine: kit and the six lanes of 16 steps (BD SD CH OH CP CB by default) */
 { "808 BASIC", ENG_DRUM, K({-2,0}), 0, { 0x1111, 0x1010, 0x5555, 0x4040, 0x0000, 0x0000 } },
 { "TECHNO", ENG_DRUM, K({-2,0}), 1, { 0x1111, 0x0000, 0x5555, 0x4444, 0x1010, 0x0800 } },
 { "BREAKBEAT", ENG_DRUM, K({-2,0}), 2, { 0x0291, 0x1210, 0x5555, 0x0400, 0x0000, 0x0000 } },
 { "ELECTRO", ENG_DRUM, K({-2,0}), 0, { 0x0925, 0x1010, 0xAAAA, 0x0000, 0x1000, 0x0402 } },
};
#define NFP ((int)(sizeof FP / sizeof FP[0]))

int main(void)
{
    static uint8_t body[ST_MAX + 64];
    int n, i, cnt[8] = { 0 };
    ui_init(); vac_init(); voices_init(); ctrl_init(0);
    printf("/* generated by tools/gen_factory.c - do not edit */\n");
    printf("#define FAC_K0 %d\n", FAC_K0);
    printf("static const uint8_t FAC[] = {\n");
    for (n = 0; n < NFP; n++) {
        const fp_t *f = &FP[n];
        int track = f->eng == ENG_DRUM ? DRUM_T0 : 0, k;
        uint32_t len, w = 0, bl, o;
        secd_t s;
        static uint8_t ent[ST_MAX + 64];
        state_defaults();
        if (f->eng != ENG_VAC && f->eng != ENG_DRUM) voices_set_engine(0, f->eng);
        if (f->eng == ENG_VC3) for (i = V3_PIN0; i < V3_COUNT; i++) voices_param(0, ENG_VC3, i, 0);
        if (f->eng == ENG_DRUM) {
            int v;
            voices_param(track, ENG_DRUM, DR_ID(0, DR_KIT), f->kit);
            for (v = 0; v < 6; v++) drum_gates_set(track, v, f->g[v]);
        } else for (k = 0; f->p[k].id >= 0; k++) {
            if (f->eng == ENG_VAC) PV[f->p[k].id] = f->p[k].v;
            else voices_param(0, f->eng, f->p[k].id, f->p[k].v);
        }
        bl = state_save(body, sizeof body, 0);
        if (!bl) { fprintf(stderr, "preset %s does not fit\n", f->name); return 1; }
        for (i = 0; i < 18; i++) ent[i] = 0;
        for (i = 0; f->name[i] && i < NAME_LEN; i++) ent[2 + i] = (uint8_t)f->name[i];
        ent[15] = (uint8_t)f->eng; k = FAC_K0 + cnt[f->eng]++; ent[16] = (uint8_t)k; ent[17] = (uint8_t)(k >> 8);
        w = 18;
        for (o = 0; o + 4 <= bl;) {                        /* keep: the track list and this engine's own sections */
            uint32_t sl = (uint32_t)(body[o + 2] | (body[o + 3] << 8));
            int keep = 0, j;
            for (j = 0; st_sec(j, &s); j++) {
                if (s.id != body[o] || s.rev != body[o + 1]) continue;
                if (s.kind == SK_TRK) keep = 1;
                else if (f->eng == ENG_VAC) keep = s.kind == SK_PV && s.id == 0x01;
                else if (f->eng == ENG_DRUM) keep = (s.kind == SK_ENG && s.t == track && s.e == ENG_DRUM) || s.kind == SK_DSQ;
                else keep = s.kind == SK_ENG && s.t == 0 && s.e == f->eng;
                break;
            }
            if (keep) { memcpy(ent + w, body + o, 4 + sl); w += 4 + sl; }
            o += 4 + sl;
        }
        ent[0] = (uint8_t)w; ent[1] = (uint8_t)(w >> 8);
        for (o = 0; o < w; o++) printf("%u,%s", ent[o], (o & 31) == 31 ? "\n" : "");
        printf("\n");
        fprintf(stderr, "%-12s eng %d  %u bytes\n", f->name, f->eng, w);
    }
    printf("};\n");
    return 0;
}
