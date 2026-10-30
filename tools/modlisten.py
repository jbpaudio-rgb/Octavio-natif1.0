#!/usr/bin/env python3
"""modlisten.py [group ...] : the modulation "listening" tests of NOTES-K.  For each engine and a set of destinations: renders with and without
modulation on the host bench (the firmware's own audio path), measures the targeted feature (pitch, spectral centroid, rms, ...) and what else
moved, compares with the prototype's own engine code (node), writes wav + png + results.json.
groups: vac moog proph drum fx seq zip   (default: all)"""
import sys, os, json, math, re
sys.path.insert(0, os.path.dirname(__file__))
from modlib import *

OUT = os.environ.get('OUT', '/mnt/user-data/outputs/octavio-native-K')
AUD, FIG = os.path.join(OUT, 'audio'), os.path.join(OUT, 'figures')
os.makedirs(AUD, exist_ok=True); os.makedirs(FIG, exist_ok=True)
RES = []

def note(engine, dest, test, expected, measured, verdict, fig='', extra=''):
    RES.append(dict(engine=engine, dest=dest, test=test, expected=expected, measured=measured, verdict=verdict, fig=fig, extra=extra))
    print('%-8s %-9s %-28s %s | %s | %s' % (engine, dest, test, expected, measured, verdict))

def save_wavs(name, a, m, tail=0.0):
    pk = max(np.abs(a).max(), np.abs(m).max(), 1e-6)
    g = 0.7 / pk
    write_wav(os.path.join(AUD, name + '_dry.wav'), a, g)
    write_wav(os.path.join(AUD, name + '_mod.wav'), m, g)

def only_target(chg, allowed):
    """the parameters the matrix moved that are not the expected ones"""
    return sorted(k for k in chg if k not in allowed)

def lfo_line(dst, shape=0, hz=5.0, amt_x=0.13, src=0, sync=0, t=0):
    return 'lfo %d %d %s %d %d %d %d' % (t, src, dst, shape, rate_for_hz(hz), amt_for(amt_x), sync)

def with_mod(script, modlines):
    return script.replace('\nend', '\n' + modlines + '\nend')

# ----------------------------------------------------------------------------------------------------- prototype parameter maps
def vac_proto(pv):
    """native Vactrol PV dict (thousandths) -> the prototype's parameters"""
    d = dict(fold=.25, sym=.1, decay=.002 * 1000 ** .75, mode=1, wave=0, mwave=0, ratio=2, fm=0, freq=20 * 200 ** .485, ead=1,
             atk=.001 * 2000 ** .21, edec=.005 * 600 ** .64, open=0, fenv=0, lvl=1)
    f = lambda v: v / 1000.0
    for k, v in pv.items():
        if k == 'FOLD': d['fold'] = f(v)
        elif k == 'SYM': d['sym'] = f(v)
        elif k == 'DECAY': d['decay'] = .002 * 1000 ** f(v)
        elif k == 'MODE': d['mode'] = v
        elif k == 'WAVE': d['wave'] = v
        elif k == 'MWAVE': d['mwave'] = v
        elif k == 'RATIO': d['ratio'] = f(v)
        elif k == 'FM': d['fm'] = f(v)
        elif k == 'FENV': d['fenv'] = f(v)
        elif k == 'EAD': d['ead'] = v
        elif k == 'ATK': d['atk'] = .001 * 2000 ** f(v)
        elif k == 'EDEC': d['edec'] = .005 * 600 ** f(v)
        elif k == 'OPEN': d['open'] = f(v)
        elif k == 'FREQ': d['freq'] = 20 * 200 ** f(v)
    return d

PMOD = {0: 'm0', 1: 'm1', 2: 'm2', 3: 'm10', 4: 'm3', 5: 'm11'}
def proto_mod(src, dst, shape, hz, amt_x, smooth=0):
    k = PMOD[src]
    d = {k + 't': dst, k + 'a': amt_x, k + 'r': hz, k + 'y': 0}
    if src >= 4: d[k + 'l'] = smooth
    else: d[k + 's'] = shape
    return d

def parse_pv(script):
    d = {}
    for ln in script.splitlines():
        p = ln.split()
        if len(p) == 3 and p[0] == 'pv': d[p[1]] = int(p[2])
    return d

def parse_notes(script):
    ev = []
    for ln in script.splitlines():
        p = ln.split()
        if len(p) == 6 and p[0] == 'at' and p[2] == 'note':
            ev.append([float(p[1]), 'on' if p[5] == '1' else 'off', int(p[4])])
        if len(p) == 5 and p[0] == 'at' and p[2] == 'hit':
            ev.append([float(p[1]), 'hit', int(p[4])])
    return ev

def total_len(script):
    return float(re.search(r'end ([\d.]+)', script).group(1))

# ----------------------------------------------------------------------------------------------------- Vactrol
SUS = """eng 0 0
pv EAD 0
pv FOLD 0
pv SYM 0
pv MODE 0
pv OPEN 1000
pv FREQ 500
pv WAVE 0
at 0.05 note 0 60 1
end 3.5
"""
# drone for timbre tests: AD envelope (0 s decay) leaves the vactrol closed, so "Low pass" mode plays the oscillator through the cutoff only
DRONE = """eng 0 0
pv EAD 1
pv EDEC 0
pv DECAY 200
pv MODE 0
pv FOLD 600
pv SYM 100
pv WAVE 2
pv OPEN 500
pv FREQ 400
at 0.05 note 0 60 1
end 4.0
"""

def vac_pair(name, script, mod, srcspec, probes=''):
    """native dry / modulated + prototype dry / modulated for a Vactrol script"""
    a = run_native(name + '_n0', script)
    m = run_native(name + '_n1', with_mod(script, mod + '\n' + probes))
    pp = vac_proto(parse_pv(script))
    ev = parse_notes(script); T = total_len(script)
    pa = run_proto(name + '_p0', dict(eng='Vactrol', p=pp, events=ev, dur=T))
    pm = run_proto(name + '_p1', dict(eng='Vactrol', p=dict(pp, **proto_mod(*srcspec)), events=ev, dur=T))
    return a, m, pa, pm

# ----------------------------------------------------------------------------------------------------- generic feature test
def pp5(v):
    v = v[np.isfinite(v)]
    return float(np.percentile(v, 95) - np.percentile(v, 5)) if len(v) else float('nan')

def signature(x, tmin, pitch=True, fmin=60, fmax=2000):
    """what moved: pitch p-p (cents), level p-p (dB), centroid p-p (% of its mean), each after tmin"""
    tr, r = rms_series(x, 882, 441); tc, c = centroid_series(x, 2048, 441)
    m = tr > tmin
    db = 20 * np.log10(r[m] + 1e-9)
    mc = tc > tmin
    sig = dict(level_db=pp5(db), cent_pct=100 * pp5(c[mc]) / max(np.mean(c[mc]), 1e-9), centroid_hz=float(np.mean(c[mc])))
    if pitch:
        tp, f = pitch_series(x, fmin, fmax, 4096, 441)
        mp = tp > tmin
        ff = f[mp]
        sig['pitch_cents'] = pp5(cents(ff[np.isfinite(ff)], np.nanmean(ff))) if np.isfinite(ff).sum() > 8 else float('nan')
    return sig

FEAT = {'peak': lambda x: peak_series(x), 'centroid': lambda x: centroid_series(x, 2048, 441), 'rms': lambda x: rms_series(x, 882, 441),
        'pitch': lambda x: zero_cross_pitch(x, 441, 2205), 'pitch_ac': lambda x: pitch_series(x, 60, 2000, 4096, 441)}

def feat_test(engine, dest, test, native, native_mod, proto_eng, proto_p, proto_modd, events, T, feature, hz, tmin, expect, allowed,
              figname, wavname, probes='', ylabel='', ratio_tol=0.4, minratio=4.0, pitch_sig=False, log=False, extra_series=None):
    """native: script text; native_mod: lines added; proto_*: the prototype's engine, params, modulation keys; feature: centroid | rms | pitch"""
    a = run_native(figname + '_n0', native)
    m = run_native(figname + '_n1', with_mod(native, native_mod + '\n' + probes))
    pa = run_proto(figname + '_p0', dict(eng=proto_eng, p=proto_p, events=events, dur=T))
    pm = run_proto(figname + '_p1', dict(eng=proto_eng, p=dict(proto_p, **proto_modd), events=events, dur=T))
    F = FEAT[feature]
    res = {}
    for nm, r in (('native dry', a), ('native mod', m), ('proto dry', pa), ('proto mod', pm)):
        t, v = F(r.x)
        if feature == 'rms': v = 20 * np.log10(v + 1e-9)
        if feature.startswith('pitch') and log: v = cents(v, np.nanmean(v[t > tmin]))
        res[nm] = (t, v)
    st = {}
    for nm in res:
        t, v = res[nm]
        st[nm] = (mod_spectrum(t, v, tmin), pp5(v[t > tmin]))
    (fn, dn, shn), pn = st['native mod'][0], st['native mod'][1]
    (fp, dp, shp), pp_ = st['proto mod'][0], st['proto mod'][1]
    dry_n, dry_p = st['native dry'][1], st['proto dry'][1]
    sign_m = signature(m.x, tmin, pitch_sig); sign_a = signature(a.x, tmin, pitch_sig)
    unm = only_target(m.chg, allowed)
    harm = min(abs(fn - k * hz) for k in (1, 2, 3, 4)) < 0.35           # a nonlinear response to the knob may put the strongest line on a harmonic of the LFO
    ok = harm and abs(fn - fp) < 0.35 and pn > minratio * max(dry_n, 1e-9) and abs(pn - pp_) < ratio_tol * max(pp_, 1e-9) and not unm
    save_wavs(wavname, a.x, m.x)
    series = [(ylabel or feature, [('native mod', *res['native mod']), ('prototype mod', *res['proto mod']), ('native dry', *res['native dry'])])]
    if extra_series: series += extra_series(a, m, pa, pm)
    figure(os.path.join(FIG, wavname + '.png'), '%s / %s : %s' % (engine, dest, test), series, spec_of=('native, modulated', m.x))
    sg = lambda d: 'level %.1f dB, centroid %.0f %%%s' % (d['level_db'], d['cent_pct'], (', pitch %.0f cents' % d['pitch_cents']) if 'pitch_cents' in d else '')
    meas = 'native %.2f Hz, depth %.3g p-p (dry %.3g); prototype %.2f Hz, depth %.3g (dry %.3g); p-p of everything, modulated [%s] vs dry [%s]; other params moved: %s' % (
        fn, pn, dry_n, fp, pp_, dry_p, sg(sign_m), sg(sign_a), unm or 'none')
    note(engine, dest, test, expect, meas, 'OK' if ok else 'CHECK', wavname + '.png')
    return a, m, pa, pm, res

def amt2(x):
    return (amt_for(x) / 1000.0) ** 2

# ----------------------------------------------------------------------------------------------------- Vactrol
SUS = """eng 0 0
pv EAD 0
pv FOLD 0
pv SYM 0
pv MODE 0
pv OPEN 1000
pv FREQ 500
pv WAVE 0
at 0.05 note 0 60 1
end 3.5
"""
# drone for timbre tests: the AD envelope with 0 s decay leaves the vactrol closed, so "Low pass" mode plays the oscillator through the cutoff alone
DRONE = """eng 0 0
pv EAD 1
pv EDEC 0
pv DECAY 200
pv MODE 0
pv FOLD 600
pv SYM 100
pv WAVE 2
pv OPEN 500
pv FREQ 400
at 0.05 note 0 60 1
end 4.0
"""

def vac_args(script, src, dst, shape, hz, ax):
    return dict(proto_eng='Vactrol', proto_p=vac_proto(parse_pv(script)), proto_modd=proto_mod(src, dst, shape, hz, amt2(ax)), events=parse_notes(script), T=total_len(script))

def vac_vibrato():
    hz, ax = 5.0, 0.13
    a, m, pa, pm, res = feat_test('Vactrol', 'freq', 'vibrato: LFO1 sine 5 Hz, 0.13', SUS, lfo_line('freq', 0, hz, ax), feature='pitch', hz=hz, tmin=0.3,
        expect='pitch oscillates at 5.0 Hz; depth +-0.13 x log2(200) = +-1190 cents', allowed={'P_FREQ'}, figname='vac_vibrato', wavname='vactrol_vibrato_freq',
        probes='probe pv FREQ', ylabel='pitch (cents re 283 Hz)', log=True, **vac_args(SUS, 0, 'freq', 0, hz, ax))
    # square: two pitches
    sq = lfo_line('freq', 3, hz, ax)
    a2 = run_native('vac_vib_sq', with_mod(SUS, sq))
    t, f = zero_cross_pitch(a2.x, 441, 1103); f = f[(t > .3) & np.isfinite(f)]
    lo, hi = np.percentile(f, 10), np.percentile(f, 90)
    two = np.mean((np.abs(f - lo) < 0.06 * lo) | (np.abs(f - hi) < 0.06 * hi))
    note('Vactrol', 'freq', 'vibrato, square LFO 5 Hz', 'two pitches 283*2^(+-0.995) Hz (hi/lo = %.2f oct)' % (2 * ax * math.log2(200)), 'native: %.0f / %.0f Hz = %.2f oct apart, %.0f %% of the windows on the two plateaus' % (lo, hi, math.log2(hi / lo), 100 * two),
         'OK' if abs(math.log2(hi / lo) - 2 * ax * math.log2(200)) < 0.1 and two > .8 else 'CHECK')

def vac_wah():
    hz, ax = 3.0, 0.35
    mod = lfo_line('open', 0, hz, ax)
    a, m, pa, pm, res = feat_test('Vactrol', 'open', 'wah (LP cutoff, "Low pass" mode): LFO1 sine 3 Hz, 0.35', DRONE, mod, feature='centroid', hz=hz, tmin=0.5,
        expect='spectral centroid oscillates at 3 Hz (cutoff sweeps), pitch / level otherwise constant', allowed={'P_OPEN'}, figname='vac_wah', wavname='vactrol_wah_open',
        probes='probe pv OPEN', ylabel='centroid (Hz)', pitch_sig=True, **vac_args(DRONE, 0, 'open', 0, hz, ax))
    return a, m

def vac_timbre():
    for dst, ax, hz in (('fold', 0.30, 3.0), ('sym', 0.30, 3.0), ('fm', 0.30, 3.0), ('ratio', 0.30, 3.0)):
        sc = DRONE.replace('pv FM 0', '') + ('pv FM 400\n' if dst in ('ratio', 'fenv') else '')
        sc = sc.replace('\nend', '\npv FM 400\nend') if dst in ('ratio',) else sc
        vac_args_ = vac_args(sc, 0, dst, 0, hz, ax)
        feat_test('Vactrol', dst, 'timbre: LFO1 sine 3 Hz, 0.30', sc, lfo_line(dst, 0, hz, ax), feature='centroid', hz=hz, tmin=0.5,
                  expect='centroid oscillates at 3 Hz (harmonic content follows %s)' % dst, allowed={'P_' + dst.upper()}, figname='vac_' + dst, wavname='vactrol_timbre_' + dst,
                  ylabel='centroid (Hz)', pitch_sig=False, ratio_tol=0.5, **vac_args_)

def vac_tremolo():
    hz, ax = 5.0, 0.5
    feat_test('Vactrol', 'lvl', 'tremolo: LFO1 sine 5 Hz, 0.5', SUS, lfo_line('lvl', 0, hz, ax), feature='rms', hz=hz, tmin=0.3,
              expect='level oscillates at 5 Hz, gain = 1 + 0.5 sin clipped at 1 -> 6 dB p-p; pitch and timbre constant', allowed={'t0 LEVEL'}, figname='vac_trem', wavname='vactrol_tremolo_lvl',
              ylabel='level (dB)', pitch_sig=True, probes='probe lvl 0', **vac_args(SUS, 0, 'lvl', 0, hz, ax))


# ----------------------------------------------------------------------------------------------------- Minimoog / Prophet
def eng_args(e, script, src, dst, shape, hz, ax, smooth=0):
    lines = script.splitlines()
    return dict(proto_eng=['Vactrol', 'Minimoog', 'Prophet', 'Drums'][e], proto_p=proto_from_native(e, lines), proto_modd=proto_mod(src, dst, shape, hz, amt2(ax), smooth),
                events=parse_notes(script), T=total_len(script))

def moog_script(extra='', notes='at 0.05 note 0 48 1', T=4.0, osc='ep 0 MG_W1 1\nep 0 MG_L1 800\nep 0 MG_L2 0\nep 0 MG_L3 0\nep 0 MG_NLEV 0'):
    return 'eng 0 1\n%s\nep 0 MG_CUT 550\nep 0 MG_RES 300\nep 0 MG_ENV 0\n%s\n%s\nend %.1f\n' % (osc, extra, notes, T)

def proph_script(extra='', notes='at 0.05 note 0 48 1', T=4.0, osc='ep 0 PR_WAVE 0\nep 0 PR_LA 800\nep 0 PR_LB 0\nep 0 PR_SL 0\nep 0 PR_NLEV 0'):
    return 'eng 0 2\n%s\nep 0 PR_CUT 500\nep 0 PR_RES 250\nep 0 PR_ENV 0\n%s\n%s\nend %.1f\n' % (osc, extra, notes, T)

def expected_pitch_cents(probe_n, law):
    st = law(probe_n)
    return (st.max() - st.min()) * 100

def tune_law(n):                                 # semitones of a tune knob (Minimoog 7, Prophet 12): sign(d) d^2 * range
    d = (n - 0.5) * 2
    return np.sign(d) * d * d

def moog():
    # pitch: oscillator 1 tune (triangle), base 0.75, +-0.2
    hz, ax = 4.0, 0.2
    sc = moog_script('ep 0 MG_T1 750', osc='ep 0 MG_W1 0\nep 0 MG_L1 800\nep 0 MG_L2 0\nep 0 MG_L3 0\nep 0 MG_NLEV 0')
    a, m, pa, pm, res = feat_test('Minimoog', 'mg1t', 'pitch (osc 1 tune): LFO1 sine 4 Hz, 0.2 around knob .75', sc, lfo_line('mg1t', 0, hz, ax), feature='pitch_ac', hz=hz, tmin=0.4,
        expect='pitch oscillates at 4 Hz; range = sign(d) d^2 x 7 semitones with d = 2(knob - .5): %.0f cents p-p for knob .55 .. .95' % (7 * 100 * (tune_law(np.array([.95])) - tune_law(np.array([.55])))[0]),
        allowed={'t0 MG[2]'}, figname='moog_pitch', wavname='moog_pitch_mg1t', probes='probe ep 0 MG_T1', ylabel='pitch (cents)', log=True, ratio_tol=0.25,
        **eng_args(1, sc, 0, 'mg1t', 0, hz, ax))
    sc = moog_script()
    feat_test('Minimoog', 'mgcut', 'wah (cutoff): LFO1 sine 3 Hz, 0.25 around knob .55', sc, lfo_line('mgcut', 0, 3.0, 0.25), feature='centroid', hz=3.0, tmin=0.5,
              expect='centroid oscillates at 3 Hz (cutoff = exp(9.2 x knob) scale)', allowed={'t0 MG[19]'}, figname='moog_cut', wavname='moog_wah_mgcut', ylabel='centroid (Hz)',
              probes='probe ep 0 MG_CUT', pitch_sig=True, ratio_tol=0.35, **eng_args(1, sc, 0, 'mgcut', 0, 3.0, 0.25))
    sc = moog_script('ep 0 MG_CUT 450\nep 0 MG_RES 500')
    feat_test('Minimoog', 'mgres', 'emphasis: LFO1 sine 3 Hz, 0.4 around knob .5', sc, lfo_line('mgres', 0, 3.0, 0.4), feature='peak', hz=3.0, tmin=0.5,
              expect='the resonant peak (strongest line re rms, dB) oscillates at 3 Hz (feedback gain 4 x res)', allowed={'t0 MG[20]'}, figname='moog_res', wavname='moog_res_mgres', ylabel='level (dB)',
              pitch_sig=False, ratio_tol=0.4, **eng_args(1, sc, 0, 'mgres', 0, 3.0, 0.4))
    sc = moog_script()
    feat_test('Minimoog', 'lvl', 'tremolo (Level): LFO1 sine 5 Hz, 0.5', sc, lfo_line('lvl', 0, 5.0, 0.5), feature='rms', hz=5.0, tmin=0.5,
              expect='level oscillates at 5 Hz, 6 dB p-p (gain 1 + 0.5 sin, clipped at 1)', allowed={'t0 LEVEL'}, figname='moog_trem', wavname='moog_tremolo_lvl', ylabel='level (dB)',
              pitch_sig=True, probes='probe lvl 0', **eng_args(1, sc, 0, 'lvl', 0, 5.0, 0.5))
    sc = moog_script()
    feat_test('Minimoog', 'mg1l', 'osc 1 level: LFO1 sine 3 Hz, 0.35 around .8', sc, lfo_line('mg1l', 0, 3.0, 0.35), feature='rms', hz=3.0, tmin=0.5,
              expect='level follows the oscillator level (linear in the knob), at 3 Hz', allowed={'t0 MG[4]'}, figname='moog_l1', wavname='moog_osclevel_mg1l', ylabel='level (dB)',
              pitch_sig=True, **eng_args(1, sc, 0, 'mg1l', 0, 3.0, 0.35))
    sc = moog_script(osc='ep 0 MG_W1 3\nep 0 MG_L1 800\nep 0 MG_L2 0\nep 0 MG_L3 0\nep 0 MG_NLEV 0') .replace('MG_CUT 550', 'MG_CUT 750')
    feat_test('Minimoog', 'mg1p', 'pulse width (PWM): LFO1 sine 3 Hz, 0.45 around .5', sc, lfo_line('mg1p', 0, 3.0, 0.45), feature='centroid', hz=3.0, tmin=0.5,
              expect='centroid oscillates at 3 Hz (pulse width sweeps)', allowed={'t0 MG[3]'}, figname='moog_pw', wavname='moog_pwm_mg1p', ylabel='centroid (Hz)',
              pitch_sig=True, **eng_args(1, sc, 0, 'mg1p', 0, 3.0, 0.45))
    sc = moog_script(osc='ep 0 MG_L1 0\nep 0 MG_L2 0\nep 0 MG_L3 0\nep 0 MG_NLEV 600\nep 0 MG_NTYPE 0\nep 0 MG_NTONE 500').replace('MG_CUT 550', 'MG_CUT 900')
    feat_test('Minimoog', 'mgso', 'noise tone: LFO1 sine 3 Hz, 0.45 around .5', sc, lfo_line('mgso', 0, 3.0, 0.45), feature='centroid', hz=3.0, tmin=0.5,
              expect='noise centroid oscillates at 3 Hz (tone knob: LP below .5, HP above)', allowed={'t0 MG[16]'}, figname='moog_ntone', wavname='moog_noisetone_mgso', ylabel='centroid (Hz)',
              pitch_sig=False, ratio_tol=0.5, minratio=2.0, **eng_args(1, sc, 0, 'mgso', 0, 3.0, 0.45))

def prophet():
    sc = proph_script('ep 0 PR_TUNE 750', osc='ep 0 PR_WAVE 200\nep 0 PR_LA 800\nep 0 PR_LB 0\nep 0 PR_SL 0\nep 0 PR_NLEV 0')
    hz, ax = 4.0, 0.2
    feat_test('Prophet', 'patune', 'pitch (osc A tune): LFO1 sine 4 Hz, 0.2 around .75', sc, lfo_line('patune', 0, hz, ax), feature='pitch_ac', hz=hz, tmin=0.4,
              expect='pitch oscillates at 4 Hz; sign(d) d^2 x 12 semitones: %.0f cents p-p for knob .55 .. .95' % (12 * 100 * (tune_law(np.array([.95])) - tune_law(np.array([.55])))[0]),
              allowed={'t0 PR[2]'}, figname='proph_pitch', wavname='prophet_pitch_patune', probes='probe ep 0 PR_TUNE', ylabel='pitch (cents)', log=True, ratio_tol=0.25,
              **eng_args(2, sc, 0, 'patune', 0, hz, ax))
    sc = proph_script()
    feat_test('Prophet', 'pcut', 'wah (cutoff): LFO1 sine 3 Hz, 0.25 around knob .5', sc, lfo_line('pcut', 0, 3.0, 0.25), feature='centroid', hz=3.0, tmin=0.5,
              expect='centroid oscillates at 3 Hz', allowed={'t0 PR[19]'}, figname='proph_cut', wavname='prophet_wah_pcut', ylabel='centroid (Hz)', probes='probe ep 0 PR_CUT',
              pitch_sig=True, ratio_tol=0.35, **eng_args(2, sc, 0, 'pcut', 0, 3.0, 0.25))
    sc = proph_script('ep 0 PR_CUT 450\nep 0 PR_RES 500')
    feat_test('Prophet', 'pres', 'resonance: LFO1 sine 3 Hz, 0.4 around .5', sc, lfo_line('pres', 0, 3.0, 0.4), feature='peak', hz=3.0, tmin=0.5,
              expect='the resonant peak (strongest line re rms, dB) oscillates at 3 Hz', allowed={'t0 PR[20]'}, figname='proph_res', wavname='prophet_res_pres', ylabel='level (dB)', ratio_tol=0.4,
              **eng_args(2, sc, 0, 'pres', 0, 3.0, 0.4))
    sc = proph_script()
    feat_test('Prophet', 'lvl', 'tremolo (Level): LFO1 sine 5 Hz, 0.5', sc, lfo_line('lvl', 0, 5.0, 0.5), feature='rms', hz=5.0, tmin=0.5,
              expect='level oscillates at 5 Hz, 6 dB p-p', allowed={'t0 LEVEL'}, figname='proph_trem', wavname='prophet_tremolo_lvl', ylabel='level (dB)', pitch_sig=True,
              probes='probe lvl 0', **eng_args(2, sc, 0, 'lvl', 0, 5.0, 0.5))
    sc = proph_script()
    feat_test('Prophet', 'pla', 'osc A level: LFO1 sine 3 Hz, 0.35 around .8', sc, lfo_line('pla', 0, 3.0, 0.35), feature='rms', hz=3.0, tmin=0.5,
              expect='level follows osc A level at 3 Hz', allowed={'t0 PR[8]'}, figname='proph_la', wavname='prophet_osclevel_pla', ylabel='level (dB)', pitch_sig=True,
              **eng_args(2, sc, 0, 'pla', 0, 3.0, 0.35))
    sc = proph_script(osc='ep 0 PR_WAVE 0\nep 0 PR_LA 800\nep 0 PR_LB 0\nep 0 PR_SL 0\nep 0 PR_NLEV 0').replace('PR_CUT 500', 'PR_CUT 700')
    feat_test('Prophet', 'pwave', 'osc A shape (saw -> sync): LFO1 sine 3 Hz, 0.45 around .5', sc, lfo_line('pwave', 0, 3.0, 0.45).replace(' 0 %d' % rate_for_hz(3.0), ' 0 %d' % rate_for_hz(3.0)), feature='centroid', hz=3.0, tmin=0.5,
              expect='centroid oscillates at 3 Hz (shape knob blends saw / triangle / pulse)', allowed={'t0 PR[0]'}, figname='proph_shape', wavname='prophet_oscshape_pwave', ylabel='centroid (Hz)',
              ratio_tol=0.5, **eng_args(2, sc, 0, 'pwave', 0, 3.0, 0.45))
    sc = proph_script('ep 0 PR_LB 900\nep 0 PR_BWAVE 0\nep 0 PR_BTUNE 540', osc='ep 0 PR_WAVE 0\nep 0 PR_LA 0\nep 0 PR_SL 0\nep 0 PR_NLEV 0').replace('PR_ENV 0', 'PR_ENV 0\nep 0 PR_PM 500')
    feat_test('Prophet', 'ppm', 'poly-mod (osc B -> cutoff): LFO1 sine 3 Hz, 0.4 around .5', sc, lfo_line('ppm', 0, 3.0, 0.4), feature='centroid', hz=3.0, tmin=0.5,
              expect='centroid oscillates at 3 Hz (osc B depth into the cutoff)', allowed={'t0 PR[22]'}, figname='proph_pm', wavname='prophet_polymod_ppm', ylabel='centroid (Hz)',
              ratio_tol=0.5, **eng_args(2, sc, 0, 'ppm', 0, 3.0, 0.4))


# ----------------------------------------------------------------------------------------------------- envelope parameters, note by note
def env_db(x, t0, t1, hop=44, win=441):
    """rms envelope in dB: 10 ms window (a few periods of the notes used), 1 ms hop"""
    i0, i1 = int(t0 * FS), int(t1 * FS)
    seg = x[i0:i1]
    n = max(0, (len(seg) - win) // hop)
    idx = np.arange(win)[None, :] + hop * np.arange(n)[:, None]
    r = np.sqrt((seg[idx] ** 2).mean(1)) + 1e-9
    return t0 + (np.arange(n) * hop + win / 2) / FS, 20 * np.log10(r)

def m_attack(x, on, off, win):             # ms from the onset to 90 % of the peak amplitude
    t, d = env_db(x, on, on + win); pk = d.max(); lin = 10 ** (d / 20)
    i = np.argmax(lin >= 0.9 * lin.max()); return 1000 * (t[i] - on)
def m_decay(x, on, off, win, db=-20, ew=441):      # ms from the peak to db below it
    t, d = env_db(x, on, on + win, 44, ew); k = np.argmax(d)
    j = np.where(d[k:] <= d[k] + db)[0]
    return 1000 * (t[k + j[0]] - t[k]) if len(j) else float('nan')
def m_sustain(x, on, off, win):            # dB re 1 at 80 % of the gate
    t, d = env_db(x, on + 0.8 * (off - on), on + 0.8 * (off - on) + 0.04); return float(d.mean())
def m_release(x, on, off, win, db=-12):    # ms from the note-off to db below the level at the note-off
    t, d = env_db(x, off - 0.03, off + win); k = int(np.argmin(np.abs(t - off))); j = np.where(d[k:] <= d[k] + db)[0]
    return 1000 * (t[k + j[0]] - t[k]) if len(j) else float('nan')
def m_cent(x, on, off, win):               # centroid 80 ms after the onset
    seg = x[int((on + 0.08) * FS):int((on + 0.08) * FS) + 2048]
    S = np.abs(np.fft.rfft(seg * np.hanning(len(seg)))); f = np.fft.rfftfreq(len(seg), 1.0 / FS)
    return float((S * f).sum() / (S.sum() + 1e-9))

def pernote(engine, e, dest, test, script_fn, native_dst, probe_native, probe_proto, metric, mname, slope_expect, figname, wavname, hz=0.26, ax=1.0, nn=11, gate=0.5, period=0.8,
            log_metric=True, allowed=None, expect='', ln_scale=None, tol=0.35, conv=None, hit=None):
    """one note every `period`; the destination follows a slow LFO; metric(note) is compared with the destination value at the onset: slope of log(metric) vs knob, native against the prototype"""
    ons = [0.1 + period * k for k in range(nn)]
    T = ons[-1] + period
    notes = '\n'.join('at %.3f note 0 %d 1\nat %.3f note 0 %d 0' % (o, NOTE[e], o + gate, NOTE[e]) for o in ons) if hit is None else '\n'.join('at %.3f hit 0 %d' % (o, hit) for o in ons)
    sc = script_fn(notes, T)
    a = run_native(figname + '_n0', sc)
    m = run_native(figname + '_n1', with_mod(sc, lfo_line(native_dst, 0, hz, ax) + '\n' + probe_native))
    eargs = vac_args(sc, 0, native_dst, 0, hz, ax) if e == 0 else eng_args(e, sc, 0, native_dst, 0, hz, ax)
    pm = run_proto(figname + '_p1', dict(eng=eargs['proto_eng'], p=dict(eargs['proto_p'], **eargs['proto_modd']), events=eargs['events'], dur=T, probe=[probe_proto]))
    pa = run_proto(figname + '_p0', dict(eng=eargs['proto_eng'], p=eargs['proto_p'], events=eargs['events'], dur=T))
    kn = m.probe['p0'] / 1000.0; tn = m.probe['t']
    kp = pm.probe[probe_proto]; tp = pm.probe['t']
    if conv: kp = conv(kp)
    vn, vp, v0, kk, kkp = [], [], [], [], []
    for o in ons:
        vn.append(metric(m.x, o, o + gate, period * 0.95)); vp.append(metric(pm.x, o, o + gate, period * 0.95)); v0.append(metric(a.x, o, o + gate, period * 0.95))
        kk.append(kn[np.searchsorted(tn, o + 0.005) - 1]); kkp.append(kp[np.searchsorted(tp, o + 0.005) - 1])
    kk, kkp = np.array(kk), np.array(kkp)
    if ln_scale == 'lin' or not log_metric:
        yn, yp = np.array(vn), np.array(vp)
    else:
        yn, yp = np.log(np.array(vn)), np.log(np.array(vp))
    ok_n = np.isfinite(yn) & np.isfinite(yp)
    sl_n = np.polyfit(kk[ok_n], yn[ok_n], 1)[0] if ok_n.sum() > 4 else float('nan')
    sl_p = np.polyfit(kkp[ok_n], yp[ok_n], 1)[0] if ok_n.sum() > 4 else float('nan')
    cc = np.corrcoef(yn[ok_n], yp[ok_n])[0, 1] if ok_n.sum() > 4 else float('nan')           # native metric vs the prototype's, note by note
    from scipy.stats import spearmanr
    rho = spearmanr(kk[ok_n], yn[ok_n])[0] if ok_n.sum() > 4 else float('nan')
    dev = float(np.median(np.abs(yn[ok_n] - yp[ok_n]))) if ok_n.sum() else float('nan')       # median |ln ratio| (log metrics) or abs difference (linear)
    save_wavs(wavname, a.x, m.x)
    import matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
    fig, ax_ = plt.subplots(3, 1, figsize=(9, 7.2))
    t, f, S = stft(m.x, 2048, 441); ax_[0].plot(np.arange(len(m.x)) / FS, m.x, lw=.4); ax_[0].set_title('%s / %s : %s' % (engine, dest, test), fontsize=9); ax_[0].set_ylabel('native, modulated')
    ax_[1].plot(tn, kn, label='native knob (probe)'); ax_[1].plot(tp, kp, ls='--', label='prototype'); ax_[1].plot(ons, kk, 'o'); ax_[1].set_ylabel(dest); ax_[1].legend(fontsize=7)
    ax_[2].plot(ons, vn, 'o-', label='native'); ax_[2].plot(ons, vp, 's--', label='prototype'); ax_[2].plot(ons, v0, 'k:', label='native, no modulation'); ax_[2].set_ylabel(mname); ax_[2].legend(fontsize=7)
    if log_metric and ln_scale != 'lin': ax_[2].set_yscale('log')
    for q in ax_: q.grid(alpha=.3)
    fig.tight_layout(); fig.savefig(os.path.join(FIG, wavname + '.png'), dpi=100); plt.close(fig)
    unm = only_target(m.chg, allowed or set())
    lim = 0.25 if (log_metric and ln_scale != 'lin') else 2.0
    ok = np.isfinite(cc) and cc > 0.95 and dev < lim and abs(rho) > 0.7 and abs(sl_n - sl_p) < tol * max(abs(sl_p), 1e-6) and not unm
    note(engine, dest, test, expect, 'per-note %s over %d notes: native vs prototype correlation %.3f, median deviation %s %.2f, native vs destination value rank correlation %+.2f, slope native %.2f / prototype %.2f; spans %.3g..%.3g (without modulation %.3g..%.3g); other params moved: %s' % (
         mname, int(ok_n.sum()), cc, 'ln-ratio' if (log_metric and ln_scale != 'lin') else 'abs', dev, rho, sl_n, sl_p, np.nanmin(vn), np.nanmax(vn), np.nanmin(v0), np.nanmax(v0), unm or 'none'), 'OK' if ok else 'CHECK', wavname + '.png')

NOTE = {0: 60, 1: 48, 2: 48}
def vac_env_script(extra):
    def f(notes, T):
        return 'eng 0 0\npv EAD 1\npv MODE 2\npv FOLD 0\npv SYM 0\npv WAVE 0\npv FREQ 500\npv DECAY 300\npv ATK 100\npv EDEC 450\n%s\n%s\nend %.2f\n' % (extra, notes, T)
    return f

def envelopes():
    pernote('Vactrol', 0, 'atk', 'envelope attack, slow LFO (0.26 Hz, amount 0.5 around .45)', vac_env_script('pv ATK 450'), 'atk', 'probe pv ATK', 'atk', m_attack, 'attack 10-90 % (ms)', 'ln(2000) = 7.6',
            'vac_atk', 'vactrol_env_atk', conv=lambda v: np.log(v / .001) / np.log(2000), ax=0.5, allowed={'P_ATK'}, expect='attack time follows .001 x 2000^knob s (log scale)')
    pernote('Vactrol', 0, 'edec', 'envelope decay', vac_env_script('pv EDEC 450\npv DECAY 100'), 'edec', 'probe pv EDEC', 'edec', lambda x, o, f, w: m_decay(x, o, f, w), 'decay to -20 dB (ms)',
            'ln(600) = 6.4', 'vac_edec', 'vactrol_env_edec', conv=lambda v: np.log(v / .005) / np.log(600), ax=0.4, allowed={'P_EDEC'}, expect='decay time follows .005 x 600^knob s')
    pernote('Vactrol', 0, 'decay', 'vactrol decay (292), gate mode', vac_env_script('pv EAD 0\npv DECAY 450'), 'decay', 'probe pv DECAY', 'decay', lambda x, o, f, w: m_release(x, o, f, w), 'release to -20 dB (ms)',
            'ln(1000) = 6.9', 'vac_decay', 'vactrol_env_decay', conv=lambda v: np.log(v / .002) / np.log(1000), ax=0.35, allowed={'P_DECAY'}, expect='release of the vactrol follows .002 x 1000^knob s')
    def msc(extra):
        return lambda notes, T: moog_script(extra, notes, T)
    pernote('Minimoog', 1, 'atk', 'amplitude attack', msc('ep 0 MG_ATK 400\nep 0 MG_DEC 450\nep 0 MG_SUS 600'), 'atk', 'probe ep 0 MG_ATK', 'atk', m_attack, 'attack 10-90 % (ms)', 'ln(2000) = 7.6',
            'moog_atk', 'moog_env_atk', conv=lambda v: np.log(v / .001) / np.log(2000), ax=0.45, allowed={'t0 MG[24]'}, expect='attack time follows .001 x 2000^knob s')
    pernote('Minimoog', 1, 'edec', 'amplitude decay (to sustain 0)', msc('ep 0 MG_ATK 100\nep 0 MG_DEC 450\nep 0 MG_SUS 0'), 'edec', 'probe ep 0 MG_DEC', 'edec', lambda x, o, f, w: m_decay(x, o, f, w), 'decay to -20 dB (ms)', 'ln(600) = 6.4',
            'moog_dec', 'moog_env_edec', conv=lambda v: np.log(v / .005) / np.log(600), ax=0.12, allowed={'t0 MG[25]'}, expect='decay time follows .005 x 600^knob s')
    pernote('Minimoog', 1, 'sus', 'sustain level', msc('ep 0 MG_ATK 100\nep 0 MG_DEC 200\nep 0 MG_SUS 500'), 'sus', 'probe ep 0 MG_SUS', 'sus', m_sustain, 'level at 80 % of the gate (dB)', '20/ln(10) = 8.7 dB per ln(sus)',
            'moog_sus', 'moog_env_sus', ax=0.45, ln_scale='lin', log_metric=False, allowed={'t0 MG[26]'}, expect='sustain level (dB) follows 20 log10(sus) (linear amplitude)')
    pernote('Minimoog', 1, 'mgenv', 'filter contour amount (centroid 80 ms after the onset)', msc('ep 0 MG_ATK 100\nep 0 MG_SUS 800\nep 0 MG_FATK 50\nep 0 MG_FDEC 600\nep 0 MG_FSUS 100\nep 0 MG_ENV 400\nep 0 MG_CUT 550'), 'mgenv',
            'probe ep 0 MG_ENV', 'mgenv', m_cent, 'centroid at 80 ms (Hz)', 'positive (exp(5 x env x contour))', 'moog_env', 'moog_contour_mgenv', ax=0.35, allowed={'t0 MG[21]'},
            expect='the contour amount opens the filter more with the envelope: centroid rises with the knob')
    def psc(extra):
        return lambda notes, T: proph_script(extra, notes, T)
    pernote('Prophet', 2, 'atk', 'amplitude attack', psc('ep 0 PR_ATK 400\nep 0 PR_DEC 450\nep 0 PR_SUS 600'), 'atk', 'probe ep 0 PR_ATK', 'atk', m_attack, 'attack 10-90 % (ms)', 'ln(2000) = 7.6',
            'proph_atk', 'prophet_env_atk', conv=lambda v: np.log(v / .001) / np.log(2000), ax=0.45, allowed={'t0 PR[23]'}, expect='attack time follows .001 x 2000^knob s')
    pernote('Prophet', 2, 'edec', 'amplitude decay (to sustain 0)', psc('ep 0 PR_ATK 100\nep 0 PR_DEC 450\nep 0 PR_SUS 0'), 'edec', 'probe ep 0 PR_DEC', 'edec', lambda x, o, f, w: m_decay(x, o, f, w), 'decay to -20 dB (ms)',
            'ln(600) = 6.4', 'proph_dec', 'prophet_env_edec', conv=lambda v: np.log(v / .005) / np.log(600), ax=0.12, allowed={'t0 PR[24]'}, expect='decay time follows .005 x 600^knob s')
    pernote('Prophet', 2, 'rel', 'release', psc('ep 0 PR_ATK 100\nep 0 PR_DEC 200\nep 0 PR_SUS 800\nep 0 PR_REL 450'), 'rel', 'probe ep 0 PR_REL', 'rel', lambda x, o, f, w: m_release(x, o, f, w), 'release to -20 dB (ms)',
            'ln(600) = 6.4', 'proph_rel', 'prophet_env_rel', conv=lambda v: np.log(v / .005) / np.log(600), ax=0.2, allowed={'t0 PR[26]'}, expect='release time follows .005 x 600^knob s')
    pernote('Prophet', 2, 'penv', 'filter env amount (centroid 80 ms after the onset)', psc('ep 0 PR_ATK 100\nep 0 PR_SUS 800\nep 0 PR_FATK 50\nep 0 PR_FDEC 600\nep 0 PR_FSUS 100\nep 0 PR_ENV 400\nep 0 PR_CUT 500'), 'penv',
            'probe ep 0 PR_ENV', 'penv', m_cent, 'centroid at 80 ms (Hz)', 'positive', 'proph_env', 'prophet_envamt_penv', ax=0.35, allowed={'t0 PR[21]'}, expect='centroid rises with the env amount')


# ----------------------------------------------------------------------------------------------------- drums
def m_f0(t0, t1):
    def f(x, on, off, win):
        seg = x[int((on + t0) * FS):int((on + t1) * FS)]
        z = np.where((seg[:-1] < 0) & (seg[1:] >= 0))[0]
        if len(z) < 3: return float('nan')
        fr = z + (-seg[z]) / (seg[z + 1] - seg[z])
        return FS * (len(z) - 1) / (fr[-1] - fr[0])
    return f
def m_peak(x, on, off, win):
    return float(np.abs(x[int(on * FS):int((on + 0.05) * FS)]).max())
def m_peak_t(x, on, off, win):             # ms to the first sample above 90 % of the hit's peak
    seg = np.abs(x[int(on * FS):int((on + 0.2) * FS)]); return 1000 * float(np.argmax(seg >= 0.9 * seg.max())) / FS
def m_thd(x, on, off, win):                # distortion of the kick body: 1 - (energy within +-12 % of the strongest line) / (energy 20..2000 Hz), 60-200 ms
    seg = x[int((on + 0.06) * FS):int((on + 0.20) * FS)]
    S = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), 1 << 15)) ** 2; f = np.fft.rfftfreq(1 << 15, 1.0 / FS)
    b = (f > 20) & (f < 2000); k = np.argmax(np.where(b, S, 0)); fp = f[k]
    return float(1 - S[(np.abs(f - fp) < 0.12 * fp)].sum() / S[b].sum())
def m_hit_cent(x, on, off, win):
    seg = x[int(on * FS):int(on * FS) + 4096]
    S = np.abs(np.fft.rfft(seg * np.hanning(len(seg)))); f = np.fft.rfftfreq(len(seg), 1.0 / FS)
    return float((S * f).sum() / (S.sum() + 1e-9))
def m_hit_decay(x, on, off, win):          # ms from the peak to -3 dB (10 ms rms)
    return m_decay(x, on, on + 0.2, 0.45, -3, 1764)
def m_hold(x, on, off, win):               # ms until the level has fallen 3 dB below its peak
    t, d = env_db(x, on, on + 0.36, 44, 1764); k = np.argmax(d); j = np.where(d[k:] <= d[k] - 3)[0]
    return 1000 * (t[k + j[0]] - on) if len(j) else float('nan')

def drums():
    def dsc(v, extra=''):
        return lambda notes, T: 'eng 0 3\n%s\n%s\nend %.2f\n' % (extra, notes, T)
    kw = dict(hz=0.37, nn=28, gate=0.2, period=0.25)
    pernote('Drums', 3, 'dtu0', 'kick tune (808 kick, f0 of the body 70-170 ms)', dsc(0), 'dtu0', 'probe ep 0 DR_TUNE.0', 'dtu0', m_f0(0.07, 0.17), 'body frequency (Hz)', '2 octaves per knob unit = ln(4) = 1.39',
            'drum_tune', 'drums_kick_tune_dtu0', hit=0, ax=0.5, allowed={'t0 DR[v0,3]'}, expect='pitch ratio 2^((tune - .5) x 2): body frequency doubles per +0.5 of the knob', **kw)
    pernote('Drums', 3, 'ddc0', 'kick decay (time to -3 dB)', dsc(0, 'ep 0 DR_DECAY.0 600'), 'ddc0', 'probe ep 0 DR_DECAY.0', 'ddc0', m_hit_decay, 'decay to -3 dB (ms)', '+2 ln(4) = 2.77 (the knob lengthens)',
            'drum_decay', 'drums_kick_decay_ddc0', hit=0, ax=0.3, allowed={'t0 DR[v0,4]'}, expect='decay time scales as 4^((decay - .5) x 2) (a larger knob = a longer decay)', **dict(kw, period=0.5, nn=16, hz=0.25))
    pernote('Drums', 3, 'dlv1', 'snare level (peak of each hit)', dsc(1), 'dlv1', 'probe ep 0 DR_LEVEL.1', 'dlv1', m_peak, 'peak (re 1)', 'linear in the knob (x 1.25)',
            'drum_level', 'drums_snare_level_dlv1', hit=1, ax=0.5, ln_scale='lin', log_metric=False, allowed={'t0 DR[v1,6]'}, expect='hit level proportional to the knob', **kw)
    pernote('Drums', 3, 'dtn1', 'snare tone (centroid)', dsc(1), 'dtn1', 'probe ep 0 DR_TONE.1', 'dtn1', m_hit_cent, 'centroid of the hit (Hz)', 'positive (more noise)',
            'drum_tone', 'drums_snare_tone_dtn1', hit=1, ax=0.5, allowed={'t0 DR[v1,5]'}, expect='more tone knob = brighter snare (noise level)', **kw)
    pernote('Drums', 3, 'dfl2', 'closed hat filter (centroid)', dsc(2), 'dfl2', 'probe ep 0 DR_FILT.2', 'dfl2', m_hit_cent, 'centroid of the hit (Hz)', 'rises above .5, falls below',
            'drum_filter', 'drums_hat_filter_dfl2', hit=2, ax=0.5, allowed={'t0 DR[v2,11]'}, expect='LP below the middle (darker), HP above (thinner)', **kw)
    pernote('Drums', 3, 'ddr0', 'kick drive (harmonic distortion of the body)', dsc(0), 'ddr0', 'probe ep 0 DR_DRIVE.0', 'ddr0', m_thd, 'distortion (1 - fundamental share)', 'positive',
            'drum_drive', 'drums_kick_drive_ddr0', hit=0, ax=0.5, ln_scale='lin', log_metric=False, allowed={'t0 DR[v0,10]'}, expect='tanh drive adds harmonics to the body', **kw)
    pernote('Drums', 3, 'dat0', 'kick attack (time to 90 % of the peak)', dsc(0, 'ep 0 DR_ATK.0 300'), 'dat0', 'probe ep 0 DR_ATK.0', 'dat0', m_peak_t, 'time to 90 % (ms)', 'linear (x 40 ms)',
            'drum_attack', 'drums_kick_attack_dat0', hit=0, ax=0.3, ln_scale='lin', log_metric=False, allowed={'t0 DR[v0,7]'}, expect='attack ramp = knob x 40 ms', **kw)
    pernote('Drums', 3, 'dhd0', 'kick hold (time until -3 dB)', dsc(0, 'ep 0 DR_HOLD.0 300\nep 0 DR_DECAY.0 400'), 'dhd0', 'probe ep 0 DR_HOLD.0', 'dhd0', m_hold, 'time to -3 dB (ms)', 'linear (x 250 ms)',
            'drum_hold', 'drums_kick_hold_dhd0', hit=0, ax=0.3, ln_scale='lin', log_metric=False, allowed={'t0 DR[v0,9]'}, expect='envelope held for knob x 250 ms', **dict(kw, period=0.5, nn=16, hz=0.25))
    pernote('Drums', 3, 'dpe0', 'kick pitch envelope (f0 of the first 30 ms)', dsc(0), 'dpe0', 'probe ep 0 DR_PEAK.0', 'dpe0', m_f0(0.012, 0.042), 'initial frequency (Hz)', 'rises above .5',
            'drum_pitchenv', 'drums_kick_pitchenv_dpe0', hit=0, ax=0.5, ln_scale='lin', log_metric=False, allowed={'t0 DR[v0,8]'}, expect='initial sweep depth (knob - .5) x 1.8', **kw)


def onsets(x, thr_db=-50, hold=0.05):
    """times (s) of the hits: the level (2 ms windows) jumps by more than 9 dB within 3 windows and is above thr_db; at least `hold` s apart"""
    hop, win = 44, 88
    n = (len(x) - win) // hop
    idx = np.arange(win)[None, :] + hop * np.arange(n)[:, None]
    d = 20 * np.log10(np.sqrt((x[idx] ** 2).mean(1)) + 1e-9)
    t = (np.arange(n) * hop + win / 2) / FS
    out = []
    for i in range(3, n):
        if d[i] > thr_db and d[i] - d[i - 3] > 9 and (not out or t[i] - out[-1] > hold):
            out.append(t[i])
    return np.array(out) - 0.0015, t, d

def drum_lanes():
    T = 12.0
    base = 'eng 0 3\nbpm 120\n' + ''.join('sqg %d 0\n' % t for t in range(1, 4)) + ''.join('dg 0 %d 0\n' % v for v in range(6)) + 'ep 0 DR_DECAY.2 80\ndg 0 2 0xFFFF\nplay 1\n'      # the default beat is cleared: only the hat lane plays
    step = 0.125
    # swing
    sc = base + 'end %.1f\n' % T
    a = run_native('lane_n0', sc)
    m = run_native('lane_sw', with_mod(sc, lfo_line('dsw2', 0, 0.2, 1.0) + '\nprobe ep 0 DR_SWING' .replace('DR_SWING', 'DR_SW.2')))
    on, _, _ = onsets(m.x); k = np.floor(on / step + 0.1).astype(int)
    tp, pr = m.probe['t'], m.probe['p0'] / 1000.0
    odd = k % 2 == 1
    delay_odd = np.array([on[i] - step * k[i] for i in range(len(on))])
    sw = np.array([pr[np.searchsorted(tp, on[i]) - 1] for i in range(len(on))])
    exp = sw * 0.5 * step
    ok_ = len(on) > 60
    d_odd = delay_odd[odd]; e_odd = exp[odd]
    c1 = np.corrcoef(d_odd, e_odd)[0, 1]; slope = np.polyfit(e_odd, d_odd, 1)[0]
    d_even = delay_odd[~odd]
    save_wavs('drums_lane_swing_dsw2', a.x, m.x)
    figure(os.path.join(FIG, 'drums_lane_swing_dsw2.png'), 'Drums / dsw2 (lane swing) <- LFO1 sine 0.2 Hz, amount 1 : hat on all 16 steps', [('delay of the hit re the step grid (ms)', [('odd steps', on[odd], 1000 * d_odd), ('expected sw x 62.5 ms', on[odd], 1000 * e_odd), ('even steps', on[~odd], 1000 * d_even)])], spec_of=('native, modulated', m.x))
    note('Drums', 'dsw2', 'lane swing', 'odd steps late by swing x .5 step (0..62.5 ms), even steps on the grid', 'odd-step delay vs expected: correlation %.3f, slope %.2f, rms error %.1f ms; even steps mean %.1f ms (block quantisation 2.9 ms); %d hits; moved (besides the target): %s' % (c1, slope, 1000 * np.sqrt(np.mean((d_odd - e_odd) ** 2)), 1000 * d_even.mean(), len(on), only_target(m.chg, {'t0 DR[v2,12]'}) or 'none'),
         'OK' if c1 > 0.98 and abs(slope - 1) < 0.1 and abs(d_even.mean()) < 0.006 else 'CHECK', 'drums_lane_swing_dsw2.png')
    # chance
    m = run_native('lane_ch', with_mod(sc, lfo_line('dch2', 0, 0.1, 1.0) + '\nprobe ep 0 DR_CHANCE.2'))
    on, _, _ = onsets(m.x); k = np.floor(on / step + 0.1).astype(int)
    tp, pr = m.probe['t'], m.probe['p0'] / 1000.0
    W = 8; nst = int(T / step) - 2
    frac = np.array([np.sum((k >= w * W) & (k < (w + 1) * W)) / W for w in range(nst // W)])
    ch = np.array([pr[np.searchsorted(tp, (w * W + W / 2) * step) - 1] for w in range(nst // W)])
    cc = np.corrcoef(frac, ch)[0, 1]; sl = np.polyfit(ch, frac, 1)[0]
    save_wavs('drums_lane_chance_dch2', a.x, m.x)
    figure(os.path.join(FIG, 'drums_lane_chance_dch2.png'), 'Drums / dch2 (lane chance) <- LFO1 sine 0.1 Hz, amount 1', [('share of the 16 steps that sound (8-step windows)', [('measured', (np.arange(len(frac)) + .5) * W * step, frac), ('chance knob', (np.arange(len(frac)) + .5) * W * step, ch)])], spec_of=('native, modulated', m.x))
    note('Drums', 'dch2', 'lane chance', 'a step sounds with probability = the knob', 'share of sounding steps vs knob over 8-step windows: correlation %.2f, slope %.2f (binomial noise ~0.17 per window)' % (cc, sl), 'OK' if cc > 0.8 and 0.6 < sl < 1.4 else 'CHECK', 'drums_lane_chance_dch2.png')
    # accent
    m = run_native('lane_ac', with_mod(sc, lfo_line('dac2', 0, 0.2, 1.0) + '\nprobe ep 0 DR_ACC.2'))
    on, _, _ = onsets(m.x); k = np.floor(on / step + 0.1).astype(int)
    tp, pr = m.probe['t'], m.probe['p0'] / 1000.0
    pk = np.array([np.abs(m.x[int(o * FS):int((o + 0.02) * FS)]).max() for o in on])
    ac = np.array([pr[np.searchsorted(tp, o) - 1] for o in on])
    onb = (k % 4 == 0)
    ref = np.median(pk[onb])
    ratio = pk[~onb] / ref
    cc = np.corrcoef(ratio, 1 - ac[~onb] * 0.6)[0, 1]; sl = np.polyfit(1 - ac[~onb] * 0.6, ratio, 1)[0]
    save_wavs('drums_lane_accent_dac2', a.x, m.x)
    figure(os.path.join(FIG, 'drums_lane_accent_dac2.png'), 'Drums / dac2 (lane accent) <- LFO1 sine 0.2 Hz, amount 1', [('off-beat peak / on-beat peak', [('measured', on[~onb], ratio), ('1 - 0.6 x accent', on[~onb], 1 - ac[~onb] * 0.6)])], spec_of=('native, modulated', m.x))
    note('Drums', 'dac2', 'lane accent', 'off-beat steps at (1 - 0.6 x accent) of the on-beat level', 'off-beat peak ratio vs 1 - .6 x knob: correlation %.3f, slope %.2f' % (cc, sl), 'OK' if cc > 0.9 and 0.6 < sl < 1.4 else 'CHECK', 'drums_lane_accent_dac2.png')
    # humanize
    m = run_native('lane_hu', with_mod(sc, lfo_line('dhu2', 0, 0.2, 1.0) + '\nprobe ep 0 DR_HUM.2'))
    on, _, _ = onsets(m.x); k = np.floor(on / step + 0.1).astype(int)
    tp, pr = m.probe['t'], m.probe['p0'] / 1000.0
    dly = np.array([on[i] - step * k[i] for i in range(len(on))]) * 1000
    hu = np.array([pr[np.searchsorted(tp, on[i]) - 1] for i in range(len(on))])
    lo = hu < 0.15; hi = hu > 0.85
    save_wavs('drums_lane_humanize_dhu2', a.x, m.x)
    figure(os.path.join(FIG, 'drums_lane_humanize_dhu2.png'), 'Drums / dhu2 (lane humanize) <- LFO1 sine 0.2 Hz, amount 1', [('hit time re the grid (ms)', [('hits', on, dly), ('humanize x 18.75 ms (max)', on, hu * 18.75)])], spec_of=('native, modulated', m.x))
    note('Drums', 'dhu2', 'lane humanize', 'late by up to humanize x .15 step = 18.75 ms (random), velocity down by up to 40 %', 'hits with humanize < .15: spread %.1f ms (grid quantisation); hits with humanize > .85: delay %.1f..%.1f ms (max expected 18.75 + 2.9)' % (np.ptp(dly[lo]) if lo.any() else float('nan'), dly[hi].min() if hi.any() else float('nan'), dly[hi].max() if hi.any() else float('nan')),
         'OK' if lo.any() and hi.any() and np.ptp(dly[lo]) < 7 and dly[hi].max() > 8 and dly[hi].max() < 24 else 'CHECK', 'drums_lane_humanize_dhu2.png')

if __name__ == '__main__':
    groups = sys.argv[1:] or ['vac']
    if 'vac' in groups:
        vac_vibrato(); vac_wah(); vac_timbre(); vac_tremolo()
    if 'moog' in groups: moog()
    if 'proph' in groups: prophet()
    if 'env' in groups: envelopes()
    if 'drum' in groups: drums()
    if 'lane' in groups: drum_lanes()
    json.dump(RES, open(os.path.join(WORK, 'results_%s.json' % '_'.join(groups)), 'w'), indent=1)
