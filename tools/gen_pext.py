#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""tools/proto_ctl.json (every <input>/<select> of the prototype, dumped with Playwright) -> src/pext_ids.h + src/pext_desc.c :
the page parameters (LFO, FX, ARP, TEMPO, MIDI menus, HOME) as extra entries of params.h's P_* table."""
import json, math
from pathlib import Path
ROOT = Path(__file__).resolve().parent.parent
ctl = {e['k']: e for e in json.load(open(ROOT / 'tools/proto_ctl.json')) if e.get('k')}
ids, descs, opts_out = [], [], []
def cid(k): return 'P_' + k.upper()
def opts(name, lst):
    opts_out.append('static const char *const OPT_%s[%d] = { %s };' % (name, len(lst), ', '.join('"%s"' % s for s in lst)))
    return 'OPT_' + name
def th(x): return int(round(float(x) * 1000))
def rng(k, label, fmt='FMT_PCT'):
    e = ctl[k]; mn, mx, st, v = th(e['min']), th(e['max']), max(1, th(e['step'])), th(e['v'])
    ids.append(cid(k)); descs.append('[%s] = { "%s", PK_RANGE, 0, %s, %d, %d, %d, %d, 0 },' % (cid(k), label, fmt, mn, mx, st, v))
def sel(k, label, texts, dflt=None, fmt='FMT_TEXT'):
    e = ctl[k]; o = DSTOPT if k[0] == 'm' and k.endswith('t') else opts(k.upper(), texts); d = int(e['v']) if dflt is None else dflt
    ids.append(cid(k)); descs.append('[%s] = { "%s", PK_SELECT, %d, %s, 0, %d, 1, %d, %s },' % (cid(k), label, len(texts), fmt, len(texts) - 1, d, o))
def chk(k, label, v=0):
    ids.append(cid(k)); descs.append('[%s] = { "%s", PK_CHECK, 2, FMT_TEXT, 0, 1, 1, %d, OPT_OFF },' % (cid(k), label, v))

SHORT = {"Envelope attack": "Env A", "Envelope decay": "Env D", "VACT decay": "Vac D", "FM amount": "FM amt", "FM ratio": "FM rat",
         "FM envelope": "FM env", "LP cutoff": "Cutoff", "Symmetry": "Sym", "Frequency": "Freq"}
FXAB = ["Drv", "Phs", "Fln", "Cho", "Mic", "Dly", "Rev", "Grn", "Fbk", "Bit", "Rng", "Stu", "Rvs", "Frz", "Tap", "Shm"]
FL = {"drive": "Drive", "tone": "Tone", "rate": "Rate", "depth": "Depth", "fb": "Feedback", "mix": "Mix", "cents": "Detune", "time": "Time", "size": "Size",
      "damp": "Damping", "crush": "Crush", "density": "Density", "pitch": "Pitch", "freq": "Freq", "shape": "Shape", "len": "Length", "chance": "Chance",
      "hold": "Hold", "wow": "Wow", "age": "Age", "shift": "Shift", "decay": "Decay"}
# Destination list as the prototype shows it for the Vactrol engine (the LFO's m0t select)
dst = []
for o in ctl['m0t']['o']:
    key, name = o.split(':', 1)
    if key[0] == 'f' and key[1].isdigit():
        import re
        i, n = re.match(r'f(\d+)(.*)', key).groups(); short = FXAB[int(i)] + ' ' + FL[n]
    elif key.startswith('a') and key != 'atk':
        short = 'Arp ' + name[4:8]
    else:
        short = SHORT.get(name, name)
    dst.append((key, short))
DSTOPT = opts('DST', [d[1] for d in dst])
LFOS = [0, 1, 2, 10, 3, 11]
SHP = ["Sine", "Triangle", "Sawtooth", "Square"]
for s, m in enumerate(LFOS):
    sh = m in (3, 11)
    sel('m%dt' % m, 'LFO %d destination' % (s + 1), [d[1] for d in dst], 0)
    if sh: rng('m%dl' % m, 'Smoothing')
    else: sel('m%ds' % m, 'Shape', SHP)
    rng('m%dr' % m, 'Rate', 'FMT_RATE')
    rng('m%da' % m, 'Amount')
    chk('m%dy' % m, 'Tempo sync')
FXD = [["Overdrive",["drive","tone","mix"]],["Phaser",["rate","depth","fb","mix"]],["Flanger",["rate","depth","fb","mix"]],["Chorus",["rate","depth","mix"]],["Micro pitch",["cents","mix"]],["Delay",["time","fb","mix"]],["Reverb",["size","damp","mix"]],["Granular",["size","density","pitch","mix"]],["Feedback",["freq","fb","tone","mix"]],["Bitcrush",["crush","rate","mix"]],["Ring mod",["freq","shape","mix"]],["Stutter",["len","chance","mix"]],["Reverse",["time","fb","mix"]],["Freeze",["size","hold","mix"]],["Tape",["wow","drive","age","mix"]],["Shimmer",["decay","shift","mix"]]]
for i, (nm, ps) in enumerate(FXD):
    for n in ps: rng('f%d%s' % (i, n), nm + ' ' + FL[n])
chk('f5sync', 'Delay tempo sync')
sel('arpmode', 'Mode', [o.split(':', 1)[1] for o in ctl['arpmode']['o']])
sel('arpoct', 'Octaves', [o.split(':', 1)[1] for o in ctl['arpoct']['o']])
sel('arprate', 'Rate', [o.split(':', 1)[1] for o in ctl['arprate']['o']])
rng('arpgate', 'Gate'); rng('arpsw', 'Swing')
sel('arplatch', 'Latch', ['Off', 'On']); rng('arpacc', 'Accent'); rng('arpchance', 'Chance')
sel('arptr', 'Transpose', [o.split(':', 1)[1] for o in ctl['arptr']['o']])
sel('arprep', 'Repeat', [o.split(':', 1)[1] for o in ctl['arprep']['o']])
rng('arphum', 'Humanize')
sel('arppat', 'Pattern', [o.split(':', 1)[1] for o in ctl['arppat']['o']])
ids.append('P_BPM'); descs.append('[P_BPM] = { "Tempo", PK_RANGE, 0, FMT_INT, 30, 300, 1, 120, 0 },')
for i in range(4): sel('chs%d' % i, 'Channel T%d' % (i + 1), ['CH %d' % (j + 1) for j in range(16)])
for k, l in (('ionote', 'Notes'), ('ioclk', 'Clock'), ('iotrn', 'Start'), ('iosnd', 'Send')):
    sel(k, l, [o.split(':', 1)[1].upper() for o in ctl[k]['o']], [o.split(':')[0] for o in ctl[k]['o']].index(ctl[k]['v']))
for i in range(4):
    ids.append('P_VOL%d' % i); descs.append('[P_VOL%d] = { "Track %d volume", PK_RANGE, 0, FMT_PCT, 0, 1000, 10, 800, 0 },' % (i, i + 1))
for i in range(4):
    ids.append('P_TYP%d' % i); descs.append('[P_TYP%d] = { "Track %d engine", PK_SELECT, 4, FMT_TEXT, 0, 3, 1, %d, OPT_ENGINE },' % (i, i + 1, i))
(ROOT / 'src/pext_ids.h').write_text('/* generated by tools/gen_pext.py */\n' + ''.join(i + ', ' + ('\n' if n % 6 == 5 else '') for n, i in enumerate(ids)) + '\n')
hdr = '/* generated by tools/gen_pext.py; included by params.c inside PD[] */\n'
(ROOT / 'src/pext_opts.h').write_text(hdr + 'static const char *const OPT_ENGINE[4] = { "VACT", "MINI", "PRO70", "808" };\n' + '\n'.join(opts_out) + '\n')
(ROOT / 'src/pext_desc.h').write_text(hdr + '\n'.join(descs) + '\n')
# destination ids for the page code
(ROOT / 'src/pext_gen.h').write_text('/* generated */\n#define N_DST %d\n' % len(dst))
print(len(ids), 'params,', len(dst), 'destinations')
