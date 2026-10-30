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
# Destination list: the master list. Entries 0..68 are the prototype's Vactrol list in the order saved presets already use; the entries of the
# other engines are appended (indices only grow), so a stored index keeps its meaning.  Each engine shows its own filtered, ordered view (DSTL).
import re
dst = []          # (key, short, full)
for o in ctl['m0t']['o']:
    key, name = o.split(':', 1)
    if key[0] == 'f' and key[1].isdigit():
        i, n = re.match(r'f(\d+)(.*)', key).groups(); short = FXAB[int(i)] + ' ' + FL[n]
    elif key.startswith('a') and key != 'atk':
        short = 'Arp ' + name[4:8]
    else:
        short = SHORT.get(name, name)
    dst.append((key, short, name))
NEWD = [("mg1l","Osc 1 level","Mg lvl1"),("mg2l","Osc 2 level","Mg lvl2"),("mg3l","Osc 3 level","Mg lvl3"),("mgsl","Noise level","Mg noise"),("mgso","Noise tone","Mg nTone"),("mgst","Noise crush","Mg nCrsh"),("mg1t","Osc 1 tune","Mg tune1"),("mg2t","Osc 2 tune","Mg tune2"),("mg3t","Osc 3 tune","Mg tune3"),("mg1p","Osc 1 width","Mg PW1"),("mg2p","Osc 2 width","Mg PW2"),("mg3p","Osc 3 width","Mg PW3"),("mgcut","Cutoff","Mg cut"),("mgres","Emphasis","Mg emph"),("mgenv","Contour","Mg cont"),
 ("pla","A level","Pr Alvl"),("plb","B level","Pr Blvl"),("psl","Sub level","Pr sub"),("pnl","Noise level","Pr noise"),("pwave","A shape","Pr Ashp"),("ppw","A width","Pr Apw"),("patune","A tune","Pr Atune"),("pbwave","B shape","Pr Bshp"),("pbpw","B width","Pr Bpw"),("pbdet","B tune","Pr Btune"),("pst","Sub tune","Pr subT"),("psp","Sub width","Pr subW"),("pno","Noise tone","Pr nTone"),("pnc","Noise crush","Pr nCrsh"),("pcut","Cutoff","Pr cut"),("pres","Resonance","Pr reso"),("penv","Env amount","Pr env"),("ppm","Poly-mod","Pr pmod"),
 ("sus","Env sustain","Env S"),("rel","Env release","Env R")]
DPN = [("dtu","Tune","Tune"),("ddc","Decay","Decay"),("dtn","Tone","Tone"),("dlv","Level","Level"),("dat","Attack","Attack"),("dpe","Pitch env","Pitch"),("dhd","Hold","Hold"),("ddr","Drive","Drive"),("dfl","Filter","Filter"),("dsw","Swing","Swing"),("dhu","Humanize","Human"),("dch","Chance","Chance"),("dac","Accent","Accent")]
for n, f, sh in NEWD: dst.append((n, sh, f))
for v in range(6):
    for a in DPN: dst.append((a[0] + str(v), "L%d %s" % (v + 1, a[2]), "L%d %s" % (v + 1, a[1])))
dst.append(("sqg", "Seq gate", "Seq gate length"))      # NOT in the prototype: the synth sequencer's gate length (appended: the indexes of saved presets stay valid)
# how each destination is applied: (group mask V=1 M=2 P=4 D=8, kind, argument)
MGID = dict(mg1l='MG_L1', mg2l='MG_L2', mg3l='MG_L3', mgsl='MG_NLEV', mgso='MG_NTONE', mgst='MG_NCRUSH', mg1t='MG_T1', mg2t='MG_T2', mg3t='MG_T3', mg1p='MG_P1', mg2p='MG_P2', mg3p='MG_P3', mgcut='MG_CUT', mgres='MG_RES', mgenv='MG_ENV')
PRID = dict(pla='PR_LA', plb='PR_LB', psl='PR_SL', pnl='PR_NLEV', pwave='PR_WAVE', ppw='PR_PW', patune='PR_TUNE', pbwave='PR_BWAVE', pbpw='PR_BPW', pbdet='PR_BTUNE', pst='PR_ST', psp='PR_SP', pno='PR_NTONE', pnc='PR_NCRUSH', pcut='PR_CUT', pres='PR_RES', penv='PR_ENV', ppm='PR_PM')
DRID = dict(dtu='DR_TUNE', ddc='DR_DECAY', dtn='DR_TONE', dlv='DR_LEVEL', dat='DR_ATK', dpe='DR_PEAK', dhd='DR_HOLD', ddr='DR_DRIVE', dfl='DR_FILT', dsw='DR_SW', dhu='DR_HUM', dch='DR_CHANCE', dac='DR_ACC')
PVID = dict(fold='P_FOLD', sym='P_SYM', freq='P_FREQ', fenv='P_FENV', ratio='P_RATIO', fm='P_FM', decay='P_DECAY', open='P_OPEN')
ARPID = dict(agate='ARP_GATE', asw='ARP_SW', aacc='ARP_ACC', achance='ARP_CHANCE', ahum='ARP_HUM')
meta = []; fxk = 0
for key, short, full in dst:
    if key == 'none': meta.append((0, 'DK_NONE', '0'))
    elif key in PVID: meta.append((1, 'DK_PV', PVID[key]))
    elif key in ('atk', 'edec'): meta.append((7, 'DK_ENV', '0' if key == 'atk' else '1'))
    elif key in ('sus', 'rel'): meta.append((6, 'DK_ENV', '2' if key == 'sus' else '3'))
    elif key == 'lvl': meta.append((15, 'DK_LVL', '0'))
    elif key == 'sqg': meta.append((7, 'DK_SQG', '0'))
    elif key in ARPID: meta.append((7, 'DK_ARP', ARPID[key]))
    elif key[0] == 'f' and key[1].isdigit(): meta.append((7, 'DK_FX', str(fxk))); fxk += 1
    elif key in MGID: meta.append((2, 'DK_ENG', MGID[key]))
    elif key in PRID: meta.append((4, 'DK_ENG', PRID[key]))
    else: meta.append((8, 'DK_DRUM', '%d * DR_PER + %s' % (int(key[3:]), DRID[key[:3]])))
assert fxk == 52, fxk
ORD = {0: ["fold","sym","freq","fenv","ratio","fm","atk","edec","decay","open","sqg","lvl"],
       1: ["mg1l","mg2l","mg3l","mgsl","mg1t","mg1p","mg2t","mg2p","mg3t","mg3p","mgso","mgst","mgcut","mgres","mgenv","atk","edec","sus","rel","sqg","lvl"],
       2: ["pla","plb","psl","pnl","pwave","ppw","patune","pbwave","pbpw","pbdet","pst","psp","pno","pnc","pcut","pres","penv","ppm","atk","edec","sus","rel","sqg","lvl"],
       3: [str(a) for a in []]}
ORD[3] = [a[0] + str(v) for v in range(6) for a in DPN] + ["lvl"]
# the prototype's own option order (before its sort): none, VACT, fx, arps, mg, p, sus/rel, drums  -> used for the ties of the sort
protoorder = [d[0] for d in dst if d[0] != 'none']
def rank(key, g):
    if key in ORD[g]: return ORD[g].index(key)
    if key in ARPID: return 500 + protoorder.index(key)
    return 1000 + protoorder.index(key)
DSTL = []
for g in range(4):
    lst = [i for i, (key, short, full) in enumerate(dst) if i == 0 or (meta[i][0] >> g) & 1]
    lst = [0] + sorted(lst[1:], key=lambda i: rank(dst[i][0], g))
    DSTL.append(lst)
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
# the sequencer's six lines (SEQ page): destination (full names, shown as the title), key, amount, glide; and the selected step's value
SQN = ['None', 'Pitch (semitones)', 'Pitch (major)', 'Pitch (minor)'] + [d[2] for d in dst[1:]]
SQO = opts('SQDST', SQN); KO = opts('KEYN', [o.split(':', 1)[1] for o in ctl['m4k']['o']])
for j in range(6):
    m = 4 + j
    ids.append('P_M%dT' % m); descs.append('[P_M%dT] = { "Line %d destination", PK_SELECT, %d, FMT_TEXT, 0, %d, 1, %d, %s },' % (m, j + 1, len(SQN), len(SQN) - 1, 2 if j == 0 else 0, SQO))
    ids.append('P_M%dK' % m); descs.append('[P_M%dK] = { "Line %d key", PK_SELECT, 12, FMT_TEXT, 0, 11, 1, 0, %s },' % (m, j + 1, KO))
    ids.append('P_M%dA' % m); descs.append('[P_M%dA] = { "Line %d amount", PK_RANGE, 0, FMT_PCT, 0, 1000, 10, 700, 0 },' % (m, j + 1))
    ids.append('P_M%dG' % m); descs.append('[P_M%dG] = { "Line %d glide", PK_RANGE, 0, FMT_PCT, 0, 1000, 10, 0, 0 },' % (m, j + 1))
ids.append('P_STEPV'); descs.append('[P_STEPV] = { "Step value", PK_RANGE, 0, FMT_STEP, -1000, 1000, 50, 0, 0 },')
(ROOT / 'src/pext_ids.h').write_text('/* generated by tools/gen_pext.py */\n' + ''.join(i + ', ' + ('\n' if n % 6 == 5 else '') for n, i in enumerate(ids)) + '\n')
hdr = '/* generated by tools/gen_pext.py; included by params.c inside PD[] */\n'
(ROOT / 'src/pext_opts.h').write_text(hdr + 'static const char *const OPT_ENGINE[4] = { "VACT", "MINI", "PRO70", "808" };\n' + '\n'.join(opts_out) + '\n')
(ROOT / 'src/pext_desc.h').write_text(hdr + '\n'.join(descs) + '\n')
# destination ids for the page code
g = ['/* generated by tools/gen_pext.py: the destination master list (see dst.c) */', '#define N_DST %d' % len(dst), '#define N_SQDST %d' % (len(dst) + 3),
     'static const struct { uint8_t grp, kind; uint16_t a; } DSTM[N_DST] = {']
g += ['    { %d, %s, %s },  /* %d %s */' % (m[0], m[1], m[2], i, dst[i][0]) for i, m in enumerate(meta)]
g += ['};']
for k in range(4):
    g.append('static const uint8_t DSTL%d[%d] = { %s };' % (k, len(DSTL[k]), ', '.join(str(i) for i in DSTL[k])))
g.append('static const uint8_t DSTLN[4] = { %s };' % ', '.join(str(len(l)) for l in DSTL))
g.append('static const uint8_t *const DSTL[4] = { DSTL0, DSTL1, DSTL2, DSTL3 };')
(ROOT / 'src/pext_gen.h').write_text('\n'.join(g) + '\n')
print(len(ids), 'params,', len(dst), 'destinations')
