/* SPDX-License-Identifier: MPL-2.0 */
/* ARP / ARP2 / ARP3 graph (the prototype's darp): 16 steps of the arpeggio as bars on a 5-line grid, the current step in red.
 * With no key held the prototype shows the C major triad 60 64 67 through the arpeggiator settings.  Included from ui.c. */

static uint8_t arp_held[16], arp_nheld;                  /* hook for the key handling: held notes (empty -> the demo triad) */
static void arp_set_held(const uint8_t *n, int cnt)
{
    int i;
    arp_nheld = (uint8_t)(cnt > 16 ? 16 : cnt);
    for (i = 0; i < arp_nheld; i++) arp_held[i] = n[i];
}
static int arp_build(uint8_t *out)                      /* arpBuild(): the step list for the current mode, octaves and repeat */
{
    uint8_t b[16], t[64], u[64];
    int n = 0, i, j, l = 0, mode = PV[P_ARPMODE], oct = PV[P_ARPOCT] + 1, rep = PV[P_ARPREP] + 1, m = 0;
    if (arp_nheld) { n = arp_nheld; for (i = 0; i < n; i++) b[i] = arp_held[i]; }
    else { b[0] = 60; b[1] = 64; b[2] = 67; n = 3; }
    if (mode != 6)
        for (i = 1; i < n; i++) { uint8_t v = b[i]; for (j = i - 1; j >= 0 && b[j] > v; j--) b[j + 1] = b[j]; b[j + 1] = v; }
    for (j = 0; j < oct; j++) for (i = 0; i < n && l < 64; i++) t[l++] = (uint8_t)(b[i] + 12 * j);
    if (mode == 2) { for (i = 0; i < l; i++) u[i] = t[l - 1 - i]; m = l; }                                  /* Down */
    else if (mode == 3 || mode == 4) {                                                                     /* Up-Dn / Dn-Up: pp() */
        if (mode == 4) { for (i = 0; i < l; i++) u[i] = t[l - 1 - i]; for (i = 0; i < l; i++) t[i] = u[i]; }
        for (i = 0; i < l; i++) u[i] = t[i];
        m = l;
        for (i = l - 2; i >= 1 && m < 64; i--) u[m++] = t[i];
    } else if (mode == 7 || mode == 8) {                                                                   /* Conv / Div: cv() */
        int a = 0, z = l - 1;
        while (a <= z) { u[m++] = t[a++]; if (a <= z) u[m++] = t[z--]; }
        if (mode == 8) { for (i = 0; i < m; i++) t[i] = u[m - 1 - i]; for (i = 0; i < m; i++) u[i] = t[i]; }
    } else { for (i = 0; i < l; i++) u[i] = t[i]; m = l; }
    l = 0;
    for (i = 0; i < m; i++) for (j = 0; j < rep && l < 64; j++) out[l++] = u[i];
    return l;
}
static void arp_draw(void)
{
    static const uint8_t PAT[6][8] = { { 1 }, { 1, 0, 0, 1, 0, 0, 1, 0 }, { 0, 1 }, { 1, 0, 1, 1 }, { 1, 0, 0, 1, 0, 1, 0, 0 }, { 1, 0, 0, 0 } };
    static const uint8_t PATN[6] = { 1, 8, 2, 4, 8, 4 }, SPB[6] = { 1, 2, 3, 4, 6, 8 };
    uint8_t S[64];
    int L = arp_build(S), lo = 60, hi = 64, k, i, on = PV[P_ARPMODE] > 0, cur;
    int32_t sw = PV[P_ARPSW], gate = PV[P_ARPGATE], acc = PV[P_ARPACC], chance = PV[P_ARPCHANCE];
    uint32_t sec_steps;
    if (L < 1) L = 1;
    for (i = 0; i < L; i++) { if (S[i] < lo) lo = S[i]; if (S[i] > hi) hi = S[i]; }
    for (i = 0; i <= 4; i++)
        g_hline(7, 232, GYC16((14 * 256) + i * 5504), 0x333333u);                  /* y = 14 + i * 21.5 */
    {                                                                                  /* the current step follows the clock: bpm / 60 * steps per beat */
        uint32_t tm = arp_now % 3600000u;
        sec_steps = (uint32_t)(((uint64_t)tm * (uint32_t)(PV[P_BPM] * SPB[PV[P_ARPRATE]]) * 18325194u) >> 40);
        cur = (int)(sec_steps & 15u);
    }
    for (k = 0; k < 16; k++) {
        int nt, rest, a;
        int32_t xq8, wq8, hq8, yq8, aa, gv;
        uint32_t col;
        if (PV[P_ARPMODE] == 5) { uint32_t h = (uint32_t)(k * 2654435761u) ^ ((arp_now / 667u) * 40503u); h ^= h >> 13; h *= 2246822519u; nt = S[(h >> 16) % (uint32_t)L]; }
        else nt = S[k % L];
        yq8 = (106 * 256) - ((nt - lo) * 86 * 256) / (hi - lo > 1 ? hi - lo : 1);
        xq8 = k * 5280 + ((k & 1) ? sw * 2376 / 1000 : 0);
        gv = gate >= 980 ? 1000 : gate;
        wq8 = (485760 * gv) / 100000;
        if (wq8 < 768) wq8 = 768;
        hq8 = 1792 + ((acc > 0 && k % 4 == 0) ? acc * 1792 / 1000 : 0);
        rest = !PAT[PV[P_ARPPAT]][k % PATN[PV[P_ARPPAT]]];
        a = on ? 256 : 90;
        aa = (a * (350000 + 650 * chance)) / 1000000;
        if (rest) aa = aa * 46 >> 8;
        col = k == cur ? COL_RED : (k % 4 == 0 ? COL_TEXT : 0xb9b5a8u);
        gfx_frect16(GX16(0) + ((xq8 * 11202) >> 18) - 0, GYC16(yq8 - hq8 / 2), GX16(0) + (((xq8 + wq8) * 11202) >> 18), GYC16(yq8 + hq8 / 2), col, (uint32_t)aa);
        if (k == cur)
            gfx_frect16(GX16(0) + ((k * 5280 * 11202) >> 18), GYC16(115 * 256), GX16(0) + (((k * 5280 + 5280 - 512) * 11202) >> 18), GYC16(118 * 256),
                        COL_RED, on ? 230 : 90);
    }
}
