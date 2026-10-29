/* SPDX-License-Identifier: MPL-2.0 */
/* Host harness: renders the firmware's screen with the same sources (gcc, not the target) and writes an RGB PPM.
 *   render OUT.ppm [page] [phase_turns_q32] [encoder steps knob...]   */
#include <stdio.h>
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
#include "../src/vactrol.c"
#include "../src/ui.c"
void ui_params_changed(void) {}
int main(int argc, char **argv)
{
    static uint16_t band[SCR_W * 8];
    static uint8_t rgb[SCR_W * SCR_H * 3];
    int y, i;
    ui_init();
    vac_init();
    for (i = 3; i + 1 < argc; i += 2) {        /* "kN steps" pairs: knob N, steps */
        if (argv[i][0] == 'k') ui_encoder(argv[i][1] - '1', atoi(argv[i + 1]));
        if (argv[i][0] == 'b') ui_button(atoi(argv[i + 1]));
    }
    vac_update();
    if (argc > 2) ed_phase = (uint32_t)strtoul(argv[2], 0, 0);
    ui_prepare();
    for (y = 0; y < SCR_H; y += 8) {
        gfx_band(band, y, y + 8);
        ui_draw(y, y + 8);
        for (i = 0; i < SCR_W * 8; i++) {
            uint16_t p = band[i];
            uint32_t r = (p >> 8) & 0xF8, g = (p >> 3) & 0xFC, b = (p << 3) & 0xF8;
            uint8_t *o = rgb + ((y * SCR_W) + i) * 3;
            o[0] = (uint8_t)(r | (r >> 5)); o[1] = (uint8_t)(g | (g >> 6)); o[2] = (uint8_t)(b | (b >> 5));
        }
    }
    {
        FILE *f = fopen(argv[1], "wb");
        fprintf(f, "P6\n240 240\n255\n");
        fwrite(rgb, 1, sizeof rgb, f);
        fclose(f);
    }
    return 0;
}
