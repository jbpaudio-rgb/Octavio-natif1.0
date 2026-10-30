/* SPDX-License-Identifier: MPL-2.0 */
/* Host test of the per-track FX chains (gcc): fxtest IN.f32 OUT_PREFIX spec0 [spec1 spec2 spec3] [bpm]
 * spec = "fx:p0,p1,..[:sync]" : the track's first slot is effect fx with those knobs (thousandths, mix last), the other slots
 * the first other effects with mix 0.  Every track gets the same input (Q15 floats in IN, blocks of 128); track t's output goes
 * to OUT_PREFIX.t (float32).  "-" skips a track.  build: tools/fxhost.sh */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "octavio.h"
#include "params.h"
#include "tables_gen.c"
#include "mathfx.c"
#include "cdsp.c"
#include "params.c"
static int bpm_v = 120;
#include "fxtab.h"
static inline uint32_t fm1_ticks(void) { return 0; }
#include "fxstate.c"
#include "fxdsp.c"
void ui_params_changed(void) {}
int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    long n, i;
    float *in, *out[4] = { 0, 0, 0, 0 };
    int t, ns = 0, j;
    fseek(f, 0, SEEK_END); n = ftell(f) / 4; fseek(f, 0, SEEK_SET);
    in = malloc(n * 4);
    if (fread(in, 4, n, f) != (size_t)n) return 1;
    fclose(f);
    for (i = 0; i < P_COUNT; i++) PV[i] = PD[i].def;
    fx_defaults();
    for (t = 0; t < 4; t++) {
        const char *sp = 3 + t < argc ? argv[3 + t] : "-";
        int fx, k = 0, e, jj = 1;
        int16_t *P = FXT[t].p;
        char *p;
        if (sp[0] == '-' || strlen(sp) < 3) continue;
        fx = atoi(sp);
        p = strchr(sp, ':');
        FXT[t].ord[0] = (uint8_t)fx;
        for (e = 0; e < 16 && jj < FX_NSLOT; e++) if (e != fx) FXT[t].ord[jj++] = (uint8_t)e;
        if (p) {
            p++;
            while (*p && *p != ':') { P[FX_FIRST[fx] + k++ - P_F0DRIVE] = (int16_t)atoi(p); while (*p && *p != ',' && *p != ':') p++; if (*p == ',') p++; }
            if (*p == ':') P[P_F5SYNC - P_F0DRIVE] = (int16_t)atoi(p + 1);
        }
        out[t] = malloc(n * 4); ns++;
    }
    if (argc > 7) bpm_v = atoi(argv[7]);
    fx_load_view();
    fx_check();
    for (i = 0; i + 128 <= n; i += 128) {
        fx_begin();
        fx_admit();
        for (t = 0; t < 4; t++) {
            int32_t b[128];
            if (!out[t]) continue;
            for (j = 0; j < 128; j++) b[j] = (int32_t)(in[i + j] * 32768.0f);
            fx_track(t, b, 128, 1);
            for (j = 0; j < 128; j++) out[t][i + j] = b[j] / 32768.0f;
        }
    }
    for (t = 0; t < 4; t++) {
        char nm[256];
        if (!out[t]) continue;
        snprintf(nm, sizeof nm, "%s.%d", argv[2], t);
        f = fopen(nm, "wb"); fwrite(out[t], 4, n, f); fclose(f);
    }
    (void)ns;
    return 0;
}
