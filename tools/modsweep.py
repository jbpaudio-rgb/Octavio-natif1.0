#!/usr/bin/env python3
"""modsweep.py ENGINE... : for every destination of the engine's list, an LFO (sine, 2 Hz, amount 1) on that destination against the same render
without modulation (host bench).  Reports whether the sound changed (log-spectral distance and level difference), which parameters moved
(CHG: the modulated value must be the target and nothing else) and the zipper check.  Writes work/sweep_<engine>.csv."""
import re, sys, os, math, csv
sys.path.insert(0, os.path.dirname(__file__))
from modlib import *
import modlisten_proto as _mp
vac_proto_map = _mp.vac_proto

SRC = open(os.path.join(ROOT, 'src', 'pext_gen.h')).read()
DST = {int(i): n for i, n in re.findall(r'\{[^}]*\},\s*/\* (\d+) (\w+) \*/', SRC)}
DSTL = []
for k in range(4):
    m = re.search(r'DSTL%d\[\d+\] = \{([^}]*)\}' % k, SRC)
    DSTL.append([int(v) for v in m.group(1).split(',')])
ENG = ['Vactrol', 'Minimoog', 'Prophet', 'Drums']
GRP = {}
for i, (g, kind, a) in enumerate(re.findall(r'\{ (\d+), (DK_\w+), ([^}]*?) \},\s*/\* \d+', SRC)):
    GRP[i] = (int(g), kind)

def vac_proto_(lines):
    d = {}
    for ln in lines:
        q = ln.split()
        if len(q) == 3 and q[0] == 'pv': d[q[1]] = int(q[2])
    return vac_proto_map(d)

def notes_pattern(note, period, gate, total, prefix='at'):
    out = []
    k = 0
    while 0.1 + k * period + gate < total:
        t0 = 0.1 + k * period
        out.append('at %.3f note 0 %d 1' % (t0, note))
        out.append('at %.3f note 0 %d 0' % (t0 + gate, note))
        k += 1
    return '\n'.join(out)

OVR = {                                      # conditions under which a destination can be heard at all (the same as in the prototype)
    (0, 'open'): ['pv MODE 0'],                                  # the LP cutoff only acts in the "Low pass" mode
    (1, 'mg1p'): ['ep 0 MG_W1 3'], (1, 'mg2p'): ['ep 0 MG_W2 3'], (1, 'mg3p'): ['ep 0 MG_W3 3'],       # pulse width: pulse waves
    (2, 'pbpw'): ['ep 0 PR_BWAVE 800'], (2, 'pst'): ['ep 0 PR_SL 600'], (2, 'psp'): ['ep 0 PR_SL 600', 'ep 0 PR_SW 2'],
    (2, 'pno'): ['ep 0 PR_NLEV 500'], (2, 'pnc'): ['ep 0 PR_NLEV 500'],
    (2, 'edec'): ['ep 0 PR_SUS 200'], (2, 'sus'): ['ep 0 PR_SUS 400'],
    (1, 'edec'): ['ep 0 MG_SUS 200'],
    (0, 'f13size'): ['at 1.0 fxk 0 P_F13HOLD 1000'], (0, 'f13mix'): ['at 1.0 fxk 0 P_F13HOLD 1000'],      # Freeze: hold it once there is something to freeze
    (1, 'f13size'): ['at 1.0 fxk 0 P_F13HOLD 1000'], (1, 'f13mix'): ['at 1.0 fxk 0 P_F13HOLD 1000'],
    (2, 'f13size'): ['at 1.0 fxk 0 P_F13HOLD 1000'], (2, 'f13mix'): ['at 1.0 fxk 0 P_F13HOLD 1000'],
}
def base_script(e, dst):
    """setup of engine e (track 0) with the destination's prerequisites"""
    L = ['eng 0 %d' % e]
    nm = DST[dst]
    total = 3.2
    if e == 0:
        L += ['pv EAD 1', 'pv MODE 1', 'pv WAVE 0', 'pv FOLD 400', 'pv SYM 150', 'pv RATIO 2000', 'pv FM 350', 'pv FENV 500',
              'pv ATK 150', 'pv EDEC 450', 'pv DECAY 500', 'pv OPEN 300', 'pv FREQ 485']
        L.append(notes_pattern(60, 0.5, 0.15, total))
    elif e in (1, 2):
        L.append(notes_pattern(48 if e == 1 else 55, 1.0, 0.7, total))
    else:
        v = 0
        m = re.match(r'd[a-z]{2}(\d)$', nm)
        if m: v = int(m.group(1))
        L += ['at %.3f hit 0 %d' % (0.1 + 0.5 * k, v) for k in range(6)]
    L += OVR.get((e, nm), [])
    if e == 0 and nm in ('sqg', 'agate'): L.append('pv EAD 0')      # the gate length only matters with the plain gate (AD envelope off)
    kind = GRP[dst][1]
    if kind == 'DK_FX':
        fx = int(re.match(r'f(\d+)', nm).group(1))
        mixname = [n for n in DST.values() if re.match(r'f%dmix$' % fx, n)][0]
        L += ['fx 0 0 %d' % fx, 'fxk 0 P_%s 600' % mixname.upper()]
    if kind == 'DK_ARP':
        L = [l for l in L if not l.startswith('at')]
        L += ['arp 0 ARP_MODE 1', 'arp 0 ARP_OCT 1', 'arp 0 ARP_RATE 3', 'at 0.1 note 0 60 1', 'at 0.1 note 0 64 1', 'at 0.1 note 0 67 1']
    if kind == 'DK_SQG':
        L = [l for l in L if not l.startswith('at')]
        L += ['play 1', 'bpm 120']
    L.append('end %.1f' % total)
    return '\n'.join(L)

def sweep(e, tag=''):
    rows = []
    for dst in DSTL[e]:
        if dst == 0: continue
        if e == 3 and re.match(r'd(sw|hu|ch|ac)\d', DST[dst]): continue     # lane settings: tested with the step clock (modlisten.py)
        nm = DST[dst]
        b = base_script(e, dst)
        a = run_native('sw_%d_%s_a' % (e, nm), b)
        a2 = run_native('sw_%d_%s_a2' % (e, nm), b)
        ls0, _ = lsd_db(a.x, a2.x)
        m = run_native('sw_%d_%s_m' % (e, nm), b.replace('\nend', '\nlfo 0 0 %s 0 %d 1000 0\nend' % (nm, rate_for_hz(1.7))))
        ls, dl = lsd_db(a.x, m.x)
        moved = sorted(m.chg.keys())
        sim_m = sim_d = float('nan')
        if GRP[dst][1] in ('DK_PV', 'DK_ENV', 'DK_ENG', 'DK_DRUM', 'DK_LVL'):          # side by side with the prototype's own engine code
            lines = b.splitlines()
            pp = vac_proto_(lines) if e == 0 else proto_from_native(e, lines)
            ev = []
            for ln in lines:
                q = ln.split()
                if len(q) == 6 and q[0] == 'at' and q[2] == 'note': ev.append([float(q[1]), 'on' if q[5] == '1' else 'off', int(q[4])])
                if len(q) == 5 and q[0] == 'at' and q[2] == 'hit': ev.append([float(q[1]), 'hit', int(q[4])])
            hz = hz_of_rate(rate_for_hz(1.7))
            pa = run_proto('sw_%d_%s_p0' % (e, nm), dict(eng=ENG[e], p=pp, events=ev, dur=3.2))
            pm = run_proto('sw_%d_%s_p1' % (e, nm), dict(eng=ENG[e], p=dict(pp, m0t=nm, m0a=1.0, m0s=0, m0r=hz, m0y=0), events=ev, dur=3.2))
            sim_m = similarity(m.x, pm.x)[0]; sim_d = similarity(a.x, pa.x)[0]
        rows.append(dict(engine=ENG[e], dest=nm, idx=dst, lsd_db=round(ls, 2), level_db=round(dl, 2), repro=round(ls0, 3), moved=';'.join('%s[%d..%d]' % (k, v[1], v[2]) for k, v in sorted(m.chg.items())),
                         sim_mod=round(sim_m, 3), sim_dry=round(sim_d, 3), rms_a=round(float(np.sqrt((a.x ** 2).mean())), 4), rms_m=round(float(np.sqrt((m.x ** 2).mean())), 4)))
    return rows

if __name__ == '__main__':
    allrows = []
    for e in [int(a) for a in sys.argv[1:]]:
        rows = sweep(e)
        allrows += rows
        with open(os.path.join(WORK, 'sweep_%s.csv' % ENG[e]), 'w', newline='') as f:
            w = csv.DictWriter(f, rows[0].keys()); w.writeheader(); w.writerows(rows)
        for r in rows:
            print('%-8s %-9s lsd %6.2f dB  lvl %6.2f dB  sim %.2f (dry %.2f)  moved %s' % (r['engine'], r['dest'], r['lsd_db'], r['level_db'], r['sim_mod'], r['sim_dry'], r['moved']))
