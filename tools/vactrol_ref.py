#!/usr/bin/env python3
"""Float reference of the prototype's Vactrol engine (BuchlaEngine, base parameters, no modulation), used to check the
fixed-point engine in src/vactrol.c. usage: vactrol_ref.py OUT.npy [events...]  events: on:NOTE@SAMPLE off@SAMPLE ..."""
import math, sys
import numpy as np
TAU = 2 * math.pi

def wf(p, t):
    return math.sin(TAU * p) if t == 0 else 1 - 4 * abs(p - .5) if t == 1 else 2 * p - 1 if t == 2 else (1 if p < .5 else -1)

class Vac:
    def __init__(s, sr=44100, **kw):
        s.sr = sr
        s.p = dict(fold=.25, sym=.1, decay=.002 * 1000 ** .75, mode=1, wave=0, mwave=0, ratio=2, fm=0, freq=20 * 200 ** .485,
                   ead=1, atk=.001 * 2000 ** .21, edec=.005 * 600 ** .64, open=0, fenv=0, lvl=1)
        s.p.update(kw)
        s.ph = s.mph = 0.0; s.note = 60; s.gate = 0; s.env = 0.0; s.stage = 0; s.vel = 1.0
        s.fa = s.sl = s.lp = s.dc = s.v = 0.0
    def note_on(s, n): s.note = n; s.gate = 1; s.stage = 1
    def note_off(s): s.gate = 0
    def render(s, n, events=()):
        p = s.p; sr = s.sr; out = np.zeros(n)
        ev = sorted(events)
        k = 0
        for i in range(n):
            while k < len(ev) and ev[k][0] <= i:
                if ev[k][1] == 'on': s.note_on(ev[k][2])
                else: s.note_off()
                k += 1
            if i % 32 == 0:
                d = p['decay']
                s.aAtt = 1 - math.exp(-1 / ((min(.001, d * .4) + .0012 * max(0, 1 - d / .01)) * sr))
                s.aDec = 1 - math.exp(-1 / (d * sr))
                fq = p['freq'] * 2 ** ((s.note - 60) / 12); inc = fq / sr; minc = inc * p['ratio']
                a = .5 + p['fold'] ** 1.5 * 30; sym = p['sym'] ** 2 * 1.2
                ai = 1 / (p['atk'] * sr); dk = math.exp(-5 / (p['edec'] * sr))
            s.ph += inc
            if s.ph >= 1: s.ph -= 1
            s.mph += minc
            if s.mph >= 1: s.mph -= 1
            o2 = wf(s.mph, p['mwave'])
            osc = wf((s.ph + p['fm'] * (1 - p['fenv'] + p['fenv'] * s.v) * o2 + 8) % 1, p['wave'])
            x = math.sin(a * (osc + sym))
            if p['ead']:
                if s.stage == 1:
                    s.env += ai
                    if s.env >= 1: s.env = 1; s.stage = 2
                elif s.stage == 2:
                    s.env *= dk
                    if s.env < .001: s.env = 0; s.stage = 0
                t = (s.env * s.env * (3 - 2 * s.env) if s.stage == 1 else s.env) * s.vel
            else:
                t = s.vel if s.gate else 0
            s.fa += (t - s.fa) * (s.aAtt if t > s.fa else s.aDec * (.4 + 1.6 * s.fa))
            s.sl += (t - s.sl) * (s.aAtt * .5 if t > s.sl else s.aDec * .2)
            v = .65 * s.fa + .35 * s.sl; s.v = v
            y = x
            if p['mode'] < 2:
                c = p['open'] + (1 - p['open']) * v if p['mode'] == 0 else v
                w = TAU * 50 * 2 ** (c * 9) / sr; g = w / (1 + w)
                s.lp += g * (x - s.lp); y = s.lp
            if p['mode'] > 0: y *= v
            s.dc += (y - s.dc) * .001
            out[i] = (y - s.dc) * .5 * p['lvl']
        return out

if __name__ == '__main__':
    v = Vac()
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 44100
    np.save(sys.argv[1], v.render(n, [(0, 'on', 60), (int(.4 * 44100), 'off', 0)]))
