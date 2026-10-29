/* SPDX-License-Identifier: MPL-2.0 */
/* fixed-point helpers: sine, 2^x, integer square root. No float anywhere in the firmware. */
#include "octavio.h"
extern const uint32_t EXP2_TAB[257];

int32_t sin_q15(uint32_t ph)
{
    uint32_t i = ph >> 22;
    int32_t f = (int32_t)((ph >> 6) & 0xFFFFu), a = SIN_TAB[i], b = SIN_TAB[i + 1];
    return a + (((b - a) * f) >> 16);
}

uint32_t exp2_fx(int32_t x, int outq)           /* 2^(x/65536) * 2^outq */
{
    int32_t n = x >> 16;
    uint32_t fr = (uint32_t)x & 0xFFFFu, idx = fr >> 8, rem = fr & 255u;
    uint32_t a = EXP2_TAB[idx], b = EXP2_TAB[idx + 1];
    uint32_t m = a + (uint32_t)(((int64_t)(b - a) * (int32_t)rem) >> 8);   /* Q30, [1,2) */
    int32_t sh = n + outq - 30;
    if (sh >= 0)
        return sh > 1 ? 0xFFFFFFFFu : (m > (0xFFFFFFFFu >> sh) ? 0xFFFFFFFFu : m << sh);
    return -sh >= 32 ? 0u : m >> -sh;
}

uint32_t isqrt32(uint32_t v)
{
    uint32_t r = 0, b = 1u << 30;
    while (b > v)
        b >>= 2;
    while (b) {
        if (v >= r + b) {
            v -= r + b;
            r = (r >> 1) + b;
        } else
            r >>= 1;
        b >>= 2;
    }
    return r;
}
