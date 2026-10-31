/* SPDX-License-Identifier: MPL-2.0 */
/* The prototype's drum machine in fixed point: six voices, each with a type (kick, snare, closed / open hat,
 * clap / tom / crash, cowbell / ride / crash, hi tom, low tom) and a kit (808, 909, acoustic); per-voice tune, decay,
 * tone, level, attack, pitch envelope, hold, drive and a filter; closed hat chokes the open hat. The voice
 * functions follow the prototype's synthesis formulas (exponential envelopes, pitch sweeps, noise, six-square metal). */
#include "voices.h"

#define A24(tau) ((int32_t)(1.4426950409 * 65536.0 * 256.0 / ((tau) * 44100.0)))
#define Q(x) ((int32_t)((x) * 32768.0))
#define Q16(x) ((int32_t)((x) * 65536.0))
#define M15(a, b) ((int32_t)(((int64_t)(a) * (b)) >> 15))
#define M16(a, b) ((int32_t)(((int64_t)(a) * (b)) >> 16))

typedef struct { int32_t tf, dfi, tn, g, hd, pe, at, dr, drinv, fl_c; int8_t fl_t; } dpar_t;
typedef struct {
    uint8_t on, kill;                                    /* kill: samples of fade-out left (a choke or the governor takes the voice: a 32-sample ramp, then off) */
    int32_t t, vel, e, l, l2, fl, fh;
    uint32_t ph, ph2, m[6];
    uint32_t minc[6]; int32_t mtf, mm;                   /* the six metal increments for the (tf, m16) they were computed with */
    uint16_t xa[6], xb[6];                               /* control-rate envelopes (exc): value at the last 8-sample boundary / the next one, per call site */
} dvoice_t;
typedef struct {
    int16_t p[DR_COUNT];
    uint16_t gate[6];                                    /* the sequencer lanes (DG): bit s = step s */
    uint8_t dirty, kit_all;
    dpar_t P[6];
    dvoice_t V[6];
} drum_t;

#define DRUM_FADE 32                                    /* samples of the ramp that takes a drum voice away (a choke, the governor) */
#define NDRUM 1                                         /* one drum machine: only track 4 (t = 3) can run the drum engine */
#define DRUM_T0 3
static drum_t DRM[NDRUM];
static inline drum_t *dp(int t) { return (unsigned)(t - DRUM_T0) < (unsigned)NDRUM ? &DRM[t - DRUM_T0] : 0; }
static const uint8_t DKV[12] = {0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5};
static const uint16_t MET[6] = {2053, 3044, 3696, 5227, 5400, 8000};          /* Hz * 10 */

static inline int32_t exq(int32_t t, int32_t a24, int32_t dfi)                /* exp(-t * a / df), Q15 */
{
    int64_t x = (((int64_t)t * a24) >> 8) * dfi >> 16;
    if (x > 16 * 65536) return 0;
    return (int32_t)exp2_fx(-(int32_t)x, 15);
}
/* an exponential envelope at control rate: exp(-t a / df) is computed every 8 samples (at the boundary V->t % 8 == 0, one step ahead) and interpolated
 * linearly in between (the slowest-moving decay here is 0.8 ms = 35 samples: the error is < 0.7 %).  A voice has up to 6 call sites, numbered in call order (ci). */
static __attribute__((noinline)) int32_t exc(dvoice_t *V, int ci, int32_t tu, int32_t a24, int32_t dfi)       /* tu: V->t - hold (may be < 0: the envelope waits at 1) */
{
    int ph = V->t & 7;
    if (!ph) {
        int32_t tn = tu + 8;
        V->xa[ci] = (uint16_t)(V->t == 0 ? exq(tu < 0 ? 0 : tu, a24, dfi) : V->xb[ci]);
        V->xb[ci] = (uint16_t)exq(tn < 0 ? 0 : tn, a24, dfi);
    }
    return (int32_t)V->xa[ci] + (((int32_t)V->xb[ci] - (int32_t)V->xa[ci]) * ph >> 3);
}
static inline uint32_t dinc(int32_t hz256) { return (uint32_t)(((uint64_t)(uint32_t)hz256 * 97392u) >> 8); }

static int32_t met(dvoice_t *V, int32_t m16, int32_t tf16)           /* six squares, the metallic hat source, Q15 */
{
    int i, a = 0;
    if (V->mtf != tf16 || V->mm != m16 || !V->minc[0]) {
        for (i = 0; i < 6; i++) V->minc[i] = dinc((int32_t)(((int64_t)MET[i] * 256 / 10 * m16 >> 16) * tf16 >> 16));
        V->mtf = tf16; V->mm = m16;
    }
    for (i = 0; i < 6; i++) {
        V->m[i] += V->minc[i];
        a += V->m[i] < 0x80000000u ? 1 : -1;
    }
    return a * 5461;
}

#define HPF(x, c) ({ int32_t _x = (x); V->l += M16(_x - V->l, (c)); _x - V->l; })
#define LPF(x, c) ({ int32_t _x = (x); V->l2 += M16(_x - V->l2, (c)); V->l2; })

static int32_t dvoice(int ty, dvoice_t *V, int k, const dpar_t *pr)
{
    int32_t tu = V->t - pr->hd, t = tu < 0 ? 0 : tu, tf, tn = pr->tn, dfi = pr->dfi, nz = c_rand15(), s, e, x;
    int ci = 0;
    tf = pr->pe ? M16(pr->tf, 65536 + (int32_t)(((int64_t)pr->pe * exc(V, ci++, tu, A24(.04), 65536)) >> 15)) : pr->tf;
#define EX(tau) exc(V, ci++, tu, A24(tau), 65536)
#define EXD(tau) exc(V, ci++, tu, A24(tau), dfi)
    if (ty == 0) {                                                    /* kick */
        int32_t f, cl;
        if (k == 0) {
            f = M16((46 * 256) + 90 * M15(256, EX(.045)) , tf);
            V->ph += dinc(f);
            e = EXD(.5);
            s = M15(sin_q15(V->ph), e);
            cl = M15(M15(EX(.004), tn / 2), nz);
        } else if (k == 1) {
            f = M16((50 * 256) + 170 * M15(256, EX(.02)), tf);
            V->ph += dinc(f);
            e = EXD(.24);
            s = M15(M15(c_tanh(M16(sin_q15(V->ph), Q16(1.2) + (int32_t)(((int64_t)tn * Q16(4)) >> 15))), Q(.85)), e);
            cl = M15(M15(EX(.003), M15(tn, Q(.45))), nz);
        } else {
            f = M16((62 * 256) + 60 * M15(256, EX(.03)), tf);
            V->ph += dinc(f);
            V->ph2 += dinc(M16(f, Q16(1.9)));
            e = EXD(.2);
            s = M15(sin_q15(V->ph) + M15(M15(sin_q15(V->ph2), Q(.25)), EX(.07)), e);
            cl = M15(M15(LPF(nz, Q16(.25)), EX(.006)), Q(1.2) * 0 + M15(Q(.3) + M15(tn, Q(.9)), Q(1.2)));
        }
        return M15(s + cl, Q(.95));
    }
    if (ty == 1) {                                                    /* snare */
        int32_t b, nn;
        if (k == 0) {
            V->ph += dinc(M16(180 * 256, tf)); V->ph2 += dinc(M16(330 * 256, tf));
            b = M15(M15(sin_q15(V->ph), Q(.5)) + M15(sin_q15(V->ph2), Q(.3)), EXD(.09));
            nn = M15(M15(HPF(nz, Q16(.12)), EXD(.17)), M15(Q(.25) + M15(tn, Q(.9)), Q(.7)));
        } else if (k == 1) {
            int32_t fe = Q16(1) + M16(Q16(.25), EX(.012) * 2);
            V->ph += dinc(M16(M16(185 * 256, tf), fe)); V->ph2 += dinc(M16(M16(330 * 256, tf), fe));
            b = M15(M15(sin_q15(V->ph) + M15(sin_q15(V->ph << 1), Q(.3)) + M15(sin_q15(V->ph2), Q(.5)), Q(.42)), EXD(.07));
            nn = M15(M15(HPF(nz, Q16(.1)), EXD(.22)), M15(Q(.2) + M15(tn, Q(1.1)), Q(.75)));
        } else {
            int32_t n2;
            V->ph += dinc(M16(M16(190 * 256, tf), Q16(1) + M16(Q16(.15), EX(.02) * 2)));
            b = M15(M15(sin_q15(V->ph), Q(.5)), EXD(.1));
            { int32_t h = HPF(nz, Q16(.25)); n2 = LPF(h, Q16(.5)); }
            nn = M15(M15(n2, EXD(.2)), M15(Q(.25) + M15(tn, Q(1.1)), Q(1.2))) + M15(M15(nz, EX(.004)), Q(.3));
        }
        return M15(b + nn, Q(.9));
    }
    if (ty == 2 || ty == 3) {                                         /* hats */
        int op = ty == 3;
        if (k == 0) {
            x = met(V, Q16(1), tf);
            x = M15(HPF(x, Q16(.55) + M16(tn * 2, Q16(.3))), Q(1.6) / 2) * 2;
            return M15(M15(x, op ? EXD(.4) : EXD(.05)), Q(.55));
        }
        if (k == 1) {
            x = M15(met(V, Q16(1.15), tf), Q(.5)) + M15(nz, Q(.35) + M15(tn, Q(.4)));
            x = M15(HPF(x, Q16(.5) + M16(tn * 2, Q16(.3))), Q(1.5) / 2) * 2;
            return M15(M15(x, op ? EXD(.5) : EXD(.06)), Q(.6));
        }
        x = M15(nz, Q(.85)) + M15(met(V, Q16(1.4), tf), Q(.2));
        x = M15(HPF(x, Q16(.45) + M16(tn * 2, Q16(.35))), Q(1.5) / 2) * 2;
        return M15(M15(x, op ? EXD(.5) : EXD(.045)), Q(.6));
    }
    if (ty == 4) {                                                    /* clap / tom */
        if (k == 2) {
            int32_t fe = Q16(1) + M16(Q16(.7), EX(.05) * 2);
            V->ph += dinc(M16(M16(110 * 256, tf), fe));
            e = EXD(.3);
            return M15(M15(sin_q15(V->ph), Q(.9)) + M15(M15(HPF(nz, Q16(.2)), EX(.01)), M15(Q(.2) + M15(tn, Q(.9)), Q(.6))), M15(e, Q(.9)));
        } else {
            int32_t c = Q16(.12) + M16(tn * 2, Q16(.25)), pl, ts = (int32_t)(((int64_t)t * 1000) / 44100);  /* ms */
            x = LPF(HPF(nz, Q16(.1)), c) * 3;
            if (ts < 34) { int32_t tm = (int32_t)(((int64_t)(t % 507) * 1) ); pl = exq(tm, A24(.0035), 65536); }   /* 11.5 ms bursts */
            else pl = exq(t - 1499, A24(.16), dfi);
            return M15(x, M15(pl, Q(.8)));
        }
    }
    if (ty > 5) {                                                     /* hi / low tom */
        int hi = ty == 6;
        int32_t f0 = hi ? (k == 1 ? 200 : k == 0 ? 250 : 190) : (k == 1 ? 110 : k == 0 ? 120 : 95);
        int32_t swv = Q16(1) + M16((k == 0 ? Q16(.6) : k == 1 ? Q16(.8) : Q16(.35)), EX(.04) * 2);
        int32_t dcs = hi ? (k == 0 ? 280 : k == 1 ? 224 : 320) : (k == 0 ? 403 : k == 1 ? 322 : 460);   /* decay, ms */
        int32_t cl;
        V->ph += dinc(M16(M16(f0 * 256, tf), swv));
        e = exc(V, ci++, tu, A24(.001) / dcs, dfi);
        s = M15(sin_q15(V->ph), e);
        if (k == 2) {
            V->ph2 += dinc(M16(M16(f0 * 256, tf), Q16(1.6)));
            s += M15(M15(sin_q15(V->ph2), Q(.3)), exc(V, ci++, tu, A24(.001) / dcs * 2, dfi));
        }
        cl = M15(M15(k == 2 ? LPF(nz, Q16(.3)) : nz, EX(.005)), Q(.08) + M15(tn, Q(.5)));
        return M15(s + cl, Q(.9));
    }
    /* type 5: cowbell / ride / crash */
    if (k == 0) {
        V->ph += dinc(M16(540 * 256, tf)); V->ph2 += dinc(M16(800 * 256, tf));
        x = (V->ph < 0x80000000u ? 32768 : -32768) + (V->ph2 < 0x80000000u ? 32768 : -32768);
        x = LPF(x, Q16(.18) + M16(tn * 2, Q16(.4)));
        e = M15(EXD(.12), 32768 - EX(.0008));
        return M15(M15(x, e), Q(.5));
    }
    if (k == 1) {
        x = M15(met(V, Q16(1.6), tf), Q(.6)) + M15(nz, Q(.3) + M15(tn, Q(.5)));
        x = M15(HPF(x, Q16(.4) + M16(tn * 2, Q16(.3))), Q(1.5) / 2) * 2;
        return M15(M15(x, EXD(1.0)), Q(.4));
    }
    x = M15(nz, Q(.8)) + M15(met(V, Q16(1.7), tf), Q(.3));
    x = M15(HPF(x, Q16(.3) + M16(tn * 2, Q16(.3))), Q(1.6) / 2) * 2;
    return M15(M15(x, M15(EXD(1.4), 32768 - EX(.004))), Q(.45));
}

static int32_t sq15(int32_t v1000) { return v1000 * 32768 / 1000; }

static void drum_derive(drum_t *d)
{
    int v;
    for (v = 0; v < 6; v++) {
        const int16_t *p = &d->p[v * DR_PER];
        dpar_t *r = &d->P[v];
        int32_t fl = p[DR_FILT];
        r->tf = (int32_t)exp2_fx((p[DR_TUNE] - 500) * 131 + 0, 16);                     /* 2^((tu - .5) * 2) */
        r->dfi = (int32_t)exp2_fx(-(p[DR_DECAY] - 500) * 262, 16);                      /* 1 / 4^((dc - .5) * 2) */
        r->tn = sq15(p[DR_TONE]);
        r->g = p[DR_LEVEL] * 41;                                                        /* * 1.25 * 32768 / 1000 */
        r->hd = p[DR_HOLD] * 11025 / 1000;                                              /* * .25 s */
        r->pe = (p[DR_PEAK] - 500) * 118;                                               /* * 1.8 */
        r->at = p[DR_ATK] * 1764 / 1000;                                                /* * .04 s */
        r->dr = p[DR_DRIVE];
        {
            int32_t m = Q16(1) + p[DR_DRIVE] * 328;                                     /* 1 + dr * 5 */
            int32_t th = c_tanh(M16(m, Q16(.6)) / 2);
            r->drinv = th > 0 ? (int32_t)((1u << 30) / (uint32_t)th) : 0;               /* Q15 / Q15 -> Q? : 2^30 / th */
        }
        r->fl_t = 0;
        if (fl < 480) { int32_t u = fl * 65536 / 480; r->fl_t = -1; r->fl_c = Q16(.02) + M16(M16(u, u), Q16(.98)); }
        else if (fl > 520) { int32_t u = (fl - 520) * 65536 / 480; r->fl_t = 1; r->fl_c = Q16(.002) + M16(M16(u, u), Q16(.6)); }
    }
    d->dirty = 0;
}

void drum_set(int t, int id, int v)
{
    drum_t *d = dp(t);
    int vv;
    if (!d || id < 0 || id >= DR_COUNT) return;
    if (id % DR_PER == DR_KIT) {                                  /* the kit for all six voices */
        v = v < 0 ? 0 : v > 2 ? 2 : v;
        for (vv = 0; vv < 6; vv++) d->p[vv * DR_PER + DR_KV] = (int16_t)v;
    }
    d->p[id] = (int16_t)(v < 0 ? 0 : v);
    d->dirty = 1;
}
int drum_get(int t, int id) { drum_t *d = dp(t); return d && id >= 0 && id < DR_COUNT ? d->p[id] : 0; }
uint16_t drum_gates(int t, int v) { drum_t *d = dp(t); return d ? d->gate[v % 6] : 0; }
void drum_gates_set(int t, int v, uint16_t bits) { drum_t *d = dp(t); if (d) d->gate[v % 6] = bits; }

void drum_init(int t)
{
    drum_t *d = dp(t);
    int i, v;
    if (!d) return;
    for (i = 0; i < (int)sizeof(*d); i++) ((uint8_t *)d)[i] = 0;
    for (v = 0; v < 6; v++) {
        int16_t *p = &d->p[v * DR_PER];
        p[DR_TYPE] = (int16_t)v; p[DR_TUNE] = 500; p[DR_DECAY] = 500; p[DR_TONE] = 500; p[DR_LEVEL] = 800;
        p[DR_PEAK] = 500; p[DR_FILT] = 500; p[DR_CHANCE] = 1000;
    }
    drum_derive(d);
}

void drum_hit(int t, int v, int vel15)
{
    drum_t *d = dp(t);
    dvoice_t *V;
    int j;
    if (!d || v < 0 || v > 5) return;
    V = &d->V[v];
    V->on = 1; V->kill = 0; V->t = 0; V->vel = vel15; V->ph = V->ph2 = 0; V->l = V->l2 = V->fl = V->fh = 0; V->e = 32768 << 8;
    for (j = 0; j < 6; j++) V->m[j] = 0;
    V->minc[0] = 0;
    if (d->p[v * DR_PER + DR_TYPE] == 2)
        for (j = 0; j < 6; j++)
            if (d->p[j * DR_PER + DR_TYPE] == 3 && d->V[j].on && !d->V[j].kill) d->V[j].kill = DRUM_FADE;             /* the closed hat chokes the open one: a short fade, not a cut */
}
void drum_on(int t, int note, int vel15) { drum_hit(t, DKV[((note % 12) + 12) % 12], vel15); }
int drum_active(int t)
{
    int v;
    drum_t *d = dp(t);
    if (!d) return 0;
    for (v = 0; v < 6; v++) if (d->V[v].on) return 1;
    return 0;
}

void drum_render(int t, int32_t *out, int n)
{
    drum_t *d = dp(t);
    int i, v;
    if (!d) { for (i = 0; i < n; i++) out[i] = 0; return; }
    if (d->dirty) drum_derive(d);
    for (i = 0; i < n; i++) {
        int32_t acc = 0;
        for (v = 0; v < 6; v++) {
            dvoice_t *V = &d->V[v];
            const dpar_t *P = &d->P[v];
            int32_t x, ax, ty;
            if (!V->on) continue;
            ty = d->p[v * DR_PER + DR_TYPE];
            x = dvoice(ty, V, d->p[v * DR_PER + DR_KV], P);
            if (P->at > 0 && V->t < P->at) x = (int32_t)(((int64_t)x * V->t) / P->at);
            if (P->dr > 0) {
                int32_t m = Q16(1) + P->dr * 328;
                x = (int32_t)(((int64_t)c_tanh((int32_t)(((int64_t)x * m) >> 16)) * P->drinv) >> 15);
            }
            if (P->fl_t < 0) { V->fl += M16(x - V->fl, P->fl_c); x = V->fl; }
            else if (P->fl_t > 0) { V->fh += M16(x - V->fh, P->fl_c); x = x - V->fh; }
            V->t++;
            ax = x < 0 ? -x : x;
            V->e = (ax << 8) > M16(V->e, 65470) ? (ax << 8) : M16(V->e, 65470);       /* the output follower in Q23: in Q15 its decay stalled at 1 LSB / sample and killed quiet low-pitched voices at the zero crossings */
            x = M15(M15(x, P->g), V->vel);
            if (V->kill) { x = (x * (int32_t)V->kill) >> 5; if (!--V->kill) V->on = 0; }          /* fading out: 32 .. 1 / 32 */
            if (ty == 2 || ty == 3) x = M15(x, Q(1.9) / 2) * 2;
            acc += x;
            if ((V->t > 2205 && V->e < (13 << 8)) || V->t > 176400) V->on = 0;
        }
        out[i] = M15(c_tanh(M15(acc, Q(.9))), Q(.8));
    }
}
