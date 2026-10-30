#!/usr/bin/env python3
"""modlisten2.py [group ...] : the second half of the modulation tests (NOTES-K): sh (S&H plateaus) fx (effect destinations) seq (Seq gate, arp gate)
trk (per-track matrices) zip (zipper noise).  Same conventions as modlisten.py."""
import sys, os, json, math
sys.path.insert(0, os.path.dirname(__file__))
import modlisten as ML
from modlisten import *

def probe_changes(tp, v, tmin=0.3):
    m = tp > tmin
    d = np.abs(np.diff(v[m])) > 1e-9
    return int(d.sum()), tp[m][1:][d]

# ---------------------------------------------------------------------------------------------------------------- S&H
def sh_tests():
    # Minimoog cutoff <- S&H 1 (4 Hz, not smoothed): the cutoff holds a random value for 250 ms, the spectral centroid follows
    for (src, sm, name) in ((4, 0, 'sh1'), (5, 0, 'sh2'), (4, 700, 'sh1_smooth')):
        hz, ax = 4.0, 0.35
        sc = moog_script('ep 0 MG_W1 2\nep 0 MG_L1 800', osc='ep 0 MG_W1 2\nep 0 MG_L1 800\nep 0 MG_L2 0\nep 0 MG_L3 0\nep 0 MG_NLEV 0', T=5.0)
        line = 'lfo 0 %d mgcut %d %d %d 0' % (src, sm, rate_for_hz(hz), amt_for(ax))
        a = run_native('sh_%s_a' % name, sc)
        m = run_native('sh_%s_m' % name, with_mod(sc, line + '\nprobe ep 0 MG_CUT'))
        tp, pr = m.probe['t'], m.probe['p0']
        n, tch = probe_changes(tp, pr)
        span = np.ptp(pr[tp > 0.3])
        tc, cm = centroid_series(m.x, 2048, 441); tc2, ca = centroid_series(a.x, 2048, 441)
        prc = np.interp(tc, tp, pr)
        sel = tc > 0.4
        r = np.corrcoef(np.log(cm[sel]), prc[sel])[0, 1]
        # plateaus: holds when not smoothed
        if sm == 0:
            cl = tch[np.concatenate([[True], np.diff(tch) > 0.05])]            # a new held value starts a short glide of a few blocks: one cluster per S&H sample
            plateau = np.diff(cl); rate = 1.0 / np.median(plateau) if len(plateau) else float('nan')
            still = np.mean(np.abs(np.diff(pr[tp > .3])) < 1e-9)
            meas = 'new value every %.0f ms (expected 1/%.1f Hz = %.0f ms; %d samples in %.1f s), knob still in %.0f %% of the blocks, range %d..%d (thousandths); correlation of log(centroid) with the knob %.2f; centroid p-p dry %.0f %% vs modulated %.0f %%' % (
                1000 * np.median(plateau), hz, 1000 / hz, len(cl), 5.0 - 0.3, 100 * still, pr[tp > .3].min(), pr[tp > .3].max(), r, 100 * np.ptp(ca[tc2 > .4]) / ca.mean(), 100 * np.ptp(cm[sel]) / cm[sel].mean())
            ok = abs(rate - hz) < 0.4 and r > 0.7 and span > 100 and still > 0.7
        else:
            step = np.abs(np.diff(pr[tp > .3])).max()
            meas = 'smoothed S&H: the knob glides (largest block-to-block step %d thousandths vs %d range), %d changes; centroid correlation with the knob %.2f' % (step, span, n, r)
            ok = step < 0.35 * span and r > 0.8
        figure(os.path.join(FIG, 'moog_%s_cutoff.png' % name), 'Minimoog / mgcut <- S&H %d, 4 Hz, smoothing %d' % (src - 3, sm),
               [('cutoff knob', [('probe', tp, pr)]), ('centroid (Hz)', [('modulated', tc, cm), ('dry', tc2, ca)])], spec_of=('native, modulated', m.x))
        save_wavs('moog_%s_cutoff' % name, a.x, m.x)
        un = only_target(m.chg, {'t0 MG[0,MG_CUT]'} | {k for k in m.chg if 'MG[' in k and k.endswith('%d]' % MG_NAMES.index('MG_CUT'))})
        note('Minimoog', 'mgcut', 'S&H %d, smoothing %d' % (src - 3, sm), 'sample & hold: ' + ('plateaus at the S&H rate' if sm == 0 else 'glides between the held values') + ', filter follows', meas + '; other params moved: %s' % (un or 'none'), 'OK' if ok and not un else 'CHECK', 'moog_%s_cutoff.png' % name)
    # Vactrol pitch <- S&H 2 : staircase of pitches
    hz, ax = 3.0, 0.1
    line = 'lfo 0 5 freq 0 %d %d 0' % (rate_for_hz(hz), amt_for(ax))
    a = run_native('sh_vp_a', SUS); m = run_native('sh_vp_m', with_mod(SUS, line + '\nprobe pv FREQ'))
    t, f = zero_cross_pitch(m.x, 441, 1103)
    tp, pr = m.probe['t'], m.probe['p0']
    n, tch = probe_changes(tp, pr)
    exp_f = 20 * 200 ** (np.interp(t, tp, pr) / 1000.0)
    sel = (t > 0.3) & np.isfinite(f)
    cc = np.corrcoef(np.log2(f[sel]), np.log2(exp_f[sel]))[0, 1]
    figure(os.path.join(FIG, 'vactrol_sh2_freq.png'), 'Vactrol / freq <- S&H 2, 3 Hz, amount 0.1', [('pitch (Hz)', [('measured', t[sel], f[sel]), ('expected from the knob (note 60 -> oscillator Freq)', t[sel], exp_f[sel])])], spec_of=('native, modulated', m.x))
    save_wavs('vactrol_sh2_freq', a.x, m.x)
    note('Vactrol', 'freq', 'S&H 2 on pitch', 'pitch staircase, one new value every 1/3 s', '%d held values in %.1f s; measured log-pitch vs knob-implied pitch correlation %.3f; other params moved: %s' % (n, 3.5 - 0.3, cc, only_target(m.chg, {'P_FREQ'}) or 'none'),
         'OK' if cc > 0.95 and not only_target(m.chg, {'P_FREQ'}) else 'CHECK', 'vactrol_sh2_freq.png')

# ---------------------------------------------------------------------------------------------------------------- FX
def fx_tests():
    base = DRONE.replace('end 4.0', '')
    # f0drive: overdrive amount -> harmonic content
    sc = base + 'fx 0 0 0\nfxk 0 P_F0MIX 1000\nfxk 0 P_F0DRIVE 300\nend 4.0\n'
    hz, ax = 3.0, 0.4
    a = run_native('fx0_a', sc); m = run_native('fx0_m', with_mod(sc, lfo_line('f0drive', 0, hz, ax) + '\nprobe fx 0 P_F0DRIVE'))
    tc, cm = centroid_series(m.x, 2048, 441); tc0, ca = centroid_series(a.x, 2048, 441)
    f, d, sh = mod_spectrum(tc, cm, 0.5); f0_, d0, _ = mod_spectrum(tc0, ca, 0.5)
    un = only_target(m.chg, {k for k in m.chg if 'FX[' in k})
    save_wavs('fx_f0drive', a.x, m.x)
    figure(os.path.join(FIG, 'fx_f0drive.png'), 'FX slot 0 (drive) / f0drive <- LFO1 sine 3 Hz, amount 0.4', [('centroid (Hz)', [('modulated', tc, cm), ('dry', tc0, ca)]), ('drive knob', [('probe', m.probe['t'], m.probe['p0'])])], spec_of=('native, modulated', m.x))
    note('FX', 'f0drive', 'overdrive amount', 'harmonic content (centroid) oscillates at 3 Hz', 'strongest line %.2f Hz, p-p %.3g (dry %.3g); moved besides the FX knob: %s' % (f, d, d0, un or 'none'), 'OK' if abs(f - hz) < 0.4 and d > 4 * d0 and not un else 'CHECK', 'fx_f0drive.png')
    # f6mix: reverb mix -> level of the tail after a snare hit
    sc = 'eng 0 0\npv EAD 0\npv MODE 1\npv DECAY 100\npv OPEN 1000\npv FREQ 700\npv WAVE 2\nat 0.1 note 0 60 1\nat 0.13 note 0 60 0\nfx 0 0 6\nfxk 0 P_F6MIX 500\nfxk 0 P_F6SIZE 700\nend 6.0\n'
    a = run_native('fx6_a', sc); m = run_native('fx6_m', with_mod(sc, lfo_line('f6mix', 0, 0.5, 0.45) + '\nprobe fx 0 P_F6MIX'))
    tr, rm = rms_series(m.x, 2205, 1102); tr0, ra = rms_series(a.x, 2205, 1102)
    dbm, dba = 20 * np.log10(rm + 1e-9), 20 * np.log10(ra + 1e-9)
    tp, pr = m.probe['t'], m.probe['p0'] / 1000.0
    mix = np.interp(tr, tp, pr)
    sel = (tr > 0.6) & (tr < 5.5) & (mix > 0.2) & (dba > -62) & (tr > 0.2)
    diff = (dbm - dba)[sel]; expd = 20 * np.log10(mix[sel] / 0.5)
    cc = np.corrcoef(diff, expd)[0, 1]; sl = np.polyfit(expd, diff, 1)[0]
    un = only_target(m.chg, {k for k in m.chg if 'FX[' in k})
    save_wavs('fx_f6mix_reverb_tail', a.x, m.x)
    figure(os.path.join(FIG, 'fx_f6mix_reverb_tail.png'), 'FX slot 0 (reverb) / f6mix <- LFO1 sine 0.5 Hz, amount 0.45 (one 30 ms note at 0.1 s)', [('tail level (dB)', [('modulated', tr, dbm), ('static mix .5', tr0, dba)]), ('modulated - static (dB) vs 20 log(mix/.5)', [('measured', tr[sel], diff), ('expected', tr[sel], expd)]), ('mix knob', [('probe', tp, pr)])], spec_of=('native, modulated', m.x))
    note('FX', 'f6mix', 'reverb mix on the tail', 'the reverb tail level follows the mix knob: modulated - static = 20 log(mix / .5)', 'level difference vs expected: correlation %.2f, slope %.2f, range %.1f dB; moved besides the FX knob: %s' % (cc, sl, np.ptp(diff), un or 'none'), 'OK' if cc > 0.9 and 0.7 < sl < 1.3 and not un else 'CHECK', 'fx_f6mix_reverb_tail.png')
    # f5time: delay time -> echo spacing: .03 * 40^knob s.  LFO 0.05 Hz (period 20 s): +1 at 5 s, -1 at 15 s
    hz = 0.05
    sc = 'eng 0 0\npv EAD 0\npv MODE 1\npv DECAY 100\npv OPEN 1000\npv FREQ 700\npv WAVE 2\nat 5.0 note 0 60 1\nat 5.03 note 0 60 0\nat 15.0 note 0 60 1\nat 15.03 note 0 60 0\nfx 0 0 5\nfxk 0 P_F5MIX 700\nfxk 0 P_F5FB 0\nfxk 0 P_F5TIME 500\nend 17.0\n'
    a = run_native('fx5_a', sc); m = run_native('fx5_m', with_mod(sc, lfo_line('f5time', 0, hz, 0.35) + '\nprobe fx 0 P_F5TIME'))
    def echo_ms(x, t0):
        s = x[int(t0 * FS):int((t0 + 0.9) * FS)]
        s = s - s.mean()
        n = len(s); F = np.fft.rfft(s, 2 * n); R = np.fft.irfft(np.abs(F) ** 2)[:n]
        lo, hi = int(0.06 * FS), int(0.7 * FS)
        R = R / (R[0] + 1e-12)
        return 1000 * (np.argmax(R[lo:hi]) + lo) / FS
    tp, pr = m.probe['t'], m.probe['p0'] / 1000.0
    rows = []
    for th in (5.0, 15.0):
        k = pr[np.searchsorted(tp, th) - 1]
        rows.append((th, k, 30 * 40 ** k, echo_ms(a.x, th), echo_ms(m.x, th)))
    un = only_target(m.chg, {k for k in m.chg if 'FX[' in k})
    ok = all(abs(r[4] - r[2]) < 0.2 * r[2] for r in rows) and abs(rows[0][4] - rows[1][4]) > 60 and not un
    note('FX', 'f5time', 'delay time', 'echo spacing = .03 x 40^knob s', '; '.join('hit at %.1f s: knob %.2f -> expected echo %.0f ms, measured %.0f ms (static knob .5: %.0f ms)' % (r[0], r[1], r[2], r[4], r[3]) for r in rows) + '; moved besides the FX knob: %s' % (un or 'none'), 'OK' if ok else 'CHECK')
    save_wavs('fx_f5time_delay', a.x, m.x)

# ---------------------------------------------------------------------------------------------------------------- Seq gate / arp gate
def gate_durations(x, thr_db=-30, minlen=0.01):
    """onsets and durations (s) of the notes of a gated signal: 2 ms rms windows above thr_db re the peak"""
    hop = 88
    n = len(x) // hop
    r = np.sqrt((x[:n * hop].reshape(n, hop) ** 2).mean(1))
    on = r > r.max() * 10 ** (thr_db / 20)
    edges = np.diff(on.astype(int))
    st = np.where(edges == 1)[0] + 1; en = np.where(edges == -1)[0] + 1
    if len(en) and len(st) and en[0] < st[0]: en = en[1:]
    st = st[:len(en)]
    return st * hop / FS, (en[:len(st)] - st) * hop / FS

def seq_tests():
    sc = 'eng 0 0\npv EAD 0\npv MODE 1\npv OPEN 1000\npv FREQ 500\npv WAVE 0\npv ATK 0\npv DECAY 0\nbpm 120\nsqg 0 0xFFFF\nplay 1\nend 10.0\n'
    sc = sc.replace('sqg 0 0xFFFF', ''.join('sqg %d 0\n' % t for t in (1, 2, 3)) + 'sqg 0 0xFFFF')
    a = run_native('sqg_a', sc)
    m = run_native('sqg_m', with_mod(sc, lfo_line('sqg', 0, 0.2, 1.0) + '\nprobe sqg 0'))
    def on_ms(x, k):
        w = x[int(k * 0.125 * FS):int((k + 1) * 0.125 * FS)]
        r = np.sqrt((w[:len(w) // 44 * 44].reshape(-1, 44) ** 2).mean(1))
        return 1000 * 44 / FS * np.sum(r > 0.03 * np.abs(x).max())
    NS = 76
    da = np.array([on_ms(a.x, k) for k in range(2, NS)]); dm = np.array([on_ms(m.x, k) for k in range(2, NS)])
    tp, pr = m.probe['t'], m.probe['p0'] / 32768.0
    exp = np.array([pr[np.searchsorted(tp, k * 0.125 + 0.002) - 1] * 125.0 for k in range(2, NS)])
    tm = np.arange(2, NS) * 0.125
    cc = np.corrcoef(dm, exp)[0, 1]; sl = np.polyfit(exp, dm, 1)[0]; off = np.mean(dm - exp)
    un = only_target(m.chg, {'t0 SEQGATE'})
    save_wavs('vactrol_seqgate', a.x, m.x)
    figure(os.path.join(FIG, 'vactrol_seqgate.png'), 'Vactrol / sqg (Seq gate) <- LFO1 sine 0.2 Hz, amount 1 : 16 steps at 120 bpm', [('note length (ms)', [('sounding time per step', tm, dm), ('gate knob x 125 ms', tm, exp)])], spec_of=('native, modulated', m.x))
    note('Vactrol', 'sqg', 'Seq gate (synth sequencer)', 'the sequencer note length follows the Seq gate knob (0 .. 1 x 125 ms; 0.4 default)', 'dry (knob .4 = 50 ms): %.0f ms sounding per 125 ms step; modulated %.0f..%.0f ms; vs knob x 125 ms: correlation %.3f, slope %.2f, offset %+.0f ms (vactrol release); moved: %s' % (np.median(da), dm.min(), dm.max(), cc, sl, off, un or 'none'), 'OK' if cc > 0.95 and 0.8 < sl < 1.2 and not un else 'CHECK', 'vactrol_seqgate.png')
    # arp gate
    sc = 'eng 0 0\npv EAD 0\npv MODE 1\npv OPEN 1000\npv FREQ 500\npv WAVE 0\npv ATK 0\npv DECAY 0\nbpm 120\narp 0 ARP_MODE 1\narp 0 ARP_OCT 1\narp 0 ARP_RATE 3\nat 0.1 note 0 60 1\nend 10.0\n'
    a = run_native('agate_a', sc); m = run_native('agate_m', with_mod(sc, lfo_line('agate', 0, 0.2, 1.0) + '\nprobe arp 0 ARP_GATE'))
    ta, da = gate_durations(a.x); tm, dm = gate_durations(m.x)
    tp, pr = m.probe['t'], m.probe['p0'] / 1000.0
    n = len(dm)
    note('Vactrol', 'agate', 'arpeggiator gate length', 'arp note length follows Gate (range .05 .. 1 of the arp step)', 'dry notes %.0f ms; modulated: %.0f..%.0f ms over %d notes (knob %.2f..%.2f); moved: %s' % (1000 * np.median(da) if len(da) else float('nan'), 1000 * dm.min() if n else float('nan'), 1000 * dm.max() if n else float('nan'), n, pr.min(), pr.max(), only_target(m.chg, {k for k in m.chg if 'ARP' in k}) or 'none'),
         'OK' if n > 10 and dm.max() > 2 * dm.min() and not only_target(m.chg, {k for k in m.chg if 'ARP' in k}) else 'CHECK')

# ---------------------------------------------------------------------------------------------------------------- per-track matrices
def trk_tests():
    # four tracks, each with its own engine and its own matrix; every track is heard alone (the others muted with trkvol 0) and compared with a render where only that track is configured
    cfg = {
        0: ['eng 0 0', 'pv EAD 0', 'pv MODE 0', 'pv OPEN 1000', 'pv FREQ 500', 'at 0.1 note 0 60 1', lfo_line('freq', 0, 3.0, 0.1, t=0)],
        1: ['eng 1 1', 'ep 1 MG_W1 2', 'ep 1 MG_CUT 550', 'at 0.1 note 1 48 1', lfo_line('mgcut', 0, 2.0, 0.3, t=1)],
        2: ['eng 2 2', 'ep 2 PR_CUT 500', 'at 0.1 note 2 55 1', lfo_line('pcut', 0, 4.0, 0.3, t=2)],
        3: ['eng 3 3'] + ['at %.2f hit 3 %d' % (0.1 + 0.5 * k, k % 3) for k in range(6)] + [lfo_line('dlv0', 0, 1.0, 0.5, t=3)],
    }
    def script(solo, mods):
        L = []
        for t in range(4):
            L += [l for l in cfg[t] if mods or not l.startswith('lfo')]
        L += ['trkvol %d %d' % (t, 1000 if t == solo else 0) for t in range(4)]
        L.append('end 3.5')
        return '\n'.join(L) + '\n'
    rows = []
    for t in range(4):
        x_mod = run_native('trk%d_mod' % t, script(t, True)).x
        x_dry = run_native('trk%d_dry' % t, script(t, False)).x
        # only this track's own matrix: other tracks' lines removed
        own = '\n'.join([l for u in range(4) for l in cfg[u] if (u == t or not l.startswith('lfo'))] + ['trkvol %d %d' % (u, 1000 if u == t else 0) for u in range(4)] + ['end 3.5']) + '\n'
        x_own = run_native('trk%d_own' % t, own).x
        l_leak, _ = lsd_db(x_mod, x_own)
        l_eff, _ = lsd_db(x_mod, x_dry)
        rows.append((t, l_leak, l_eff))
        save_wavs('tracks_t%d_solo' % t, x_dry, x_mod)
        note(['Vactrol', 'Minimoog', 'Prophet', 'Drums'][t], ['freq', 'mgcut', 'pcut', 'dlv0'][t], 'per-track matrix, track %d solo (all 4 matrices active)' % t, 'its own matrix acts, the others do not leak into it',
             'modulated vs unmodulated: %.1f dB log-spectral distance; vs the same track with ONLY its own matrix configured: %.2f dB (0 = no cross-talk)' % (l_eff, l_leak), 'OK' if l_eff > 1.0 and l_leak < 0.3 else 'CHECK')

# ---------------------------------------------------------------------------------------------------------------- zipper
def zip_tests():
    # Level: a 1 kHz-ish pure tone through the gain, LFO 6 Hz amount 1
    sc = SUS.replace('end 3.5', 'end 3.0').replace('pv FREQ 500', 'pv FREQ 800')
    a = run_native('zip_lvl_a', sc)
    tp_, f_ = zero_cross_pitch(a.x, 441, 2205); f0 = float(np.nanmedian(f_[tp_ > .3]))
    for nm, dst, hz, ax, extra, base in (('lvl 6 Hz', 'lvl', 6.0, 1.0, 'probe lvl 0', sc), ('lvl 25 Hz', 'lvl', 25.0, 1.0, 'probe lvl 0', sc), ('lvl 60 Hz', 'lvl', 60.0, 1.0, 'probe lvl 0', sc)):
        m = run_native('zip_%s' % nm.replace(' ', '_'), with_mod(base, lfo_line(dst, 0, hz, ax) + '\n' + extra))
        sb = sideband_db(m.x, f0, kmax=1, t0=0.4, win=32768)
        sb0 = sideband_db(a.x, f0, kmax=1, t0=0.4, win=32768)
        note('Vactrol', dst, 'zipper: %s, carrier %.0f Hz' % (nm, f0), 'no spectral lines at carrier +- 344.5 Hz (the block rate) beyond those of the LFO itself', 'strongest line at f0 +- k x 344.5 Hz: %.1f dB re carrier (unmodulated: %.1f dB)' % (sb, sb0), 'OK' if sb < (-50 if 'cut' in dst else -60) else 'CHECK')
    # pitch and cutoff steps (control-rate values written once per block)
    tri = 'ep 0 MG_W1 0\nep 0 MG_L1 800\nep 0 MG_L2 0\nep 0 MG_L3 0\nep 0 MG_NLEV 0'
    for nm, dst, hz, ax, notes, osc in (('Minimoog pitch (osc 1 tune) 8 Hz, 0.02', 'mg1t', 8.0, 0.02, 'at 0.05 note 0 72 1', tri),
                                       ('Minimoog cutoff 6 Hz, 0.3', 'mgcut', 6.0, 0.3, 'at 0.05 note 0 72 1', 'ep 0 MG_W1 1\nep 0 MG_L1 800\nep 0 MG_L2 0\nep 0 MG_L3 0\nep 0 MG_NLEV 0')):
        sc = moog_script('ep 0 MG_T1 750' if dst == 'mg1t' else '', notes=notes, T=3.0, osc=osc)
        a = run_native('zip_%s_a' % dst, sc); m = run_native('zip_%s_m' % dst, with_mod(sc, lfo_line(dst, 0, hz, ax)))
        tp_, f_ = pitch_series(a.x, 200, 1500, 4096, 441); f0m = float(np.nanmedian(f_[tp_ > .5]))
        sb = sideband_db(m.x, f0m, kmax=1, t0=0.5, win=32768); sb0 = sideband_db(a.x, f0m, kmax=1, t0=0.5, win=32768)
        note('Minimoog', dst, 'zipper: %s, carrier %.0f Hz' % (nm, f0m), 'no spectral lines at carrier +- 344.5 Hz', 'strongest line at f0 +- k x 344.5 Hz: %.1f dB re carrier (unmodulated: %.1f dB)' % (sb, sb0), 'OK' if sb < (-50 if 'cut' in dst else -60) else 'CHECK')
    for nm, dst, hz, ax in (('Prophet pitch (osc A tune) 8 Hz, 0.02', 'patune', 8.0, 0.02), ('Prophet cutoff 6 Hz, 0.3', 'pcut', 6.0, 0.3)):
        sc = proph_script('ep 0 PR_TUNE 750' if dst == 'patune' else '', notes='at 0.05 note 0 72 1', T=3.0, osc='ep 0 PR_WAVE %d\nep 0 PR_LA 800\nep 0 PR_LB 0\nep 0 PR_SL 0\nep 0 PR_NLEV 0' % (0 if dst == 'patune' else 1))
        a = run_native('zip_%s_a' % dst, sc); m = run_native('zip_%s_m' % dst, with_mod(sc, lfo_line(dst, 0, hz, ax)))
        tp_, f_ = pitch_series(a.x, 200, 1500, 4096, 441); f0m = float(np.nanmedian(f_[tp_ > .5]))
        sb = sideband_db(m.x, f0m, kmax=1, t0=0.5, win=32768); sb0 = sideband_db(a.x, f0m, kmax=1, t0=0.5, win=32768)
        note('Prophet', dst, 'zipper: %s, carrier %.0f Hz' % (nm, f0m), 'no spectral lines at carrier +- 344.5 Hz', 'strongest line at f0 +- k x 344.5 Hz: %.1f dB re carrier (unmodulated: %.1f dB)' % (sb, sb0), 'OK' if sb < (-50 if 'cut' in dst else -60) else 'CHECK')
    for nm, dst, hz, ax in (('Vactrol pitch (freq) 8 Hz, 0.02', 'freq', 8.0, 0.02),):
        sc = (SUS if dst == 'freq' else DRONE).replace('pv FREQ 500', 'pv FREQ 800').replace('pv FREQ 400', 'pv FREQ 700').replace('end 3.5', 'end 3.0').replace('end 4.0', 'end 3.0')
        a = run_native('zip_v%s_a' % dst, sc); m = run_native('zip_v%s_m' % dst, with_mod(sc, lfo_line(dst, 0, hz, ax)))
        tp_, f_ = pitch_series(a.x, 200, 3000, 4096, 441); f0m = float(np.nanmedian(f_[tp_ > .5]))
        sb = sideband_db(m.x, f0m, kmax=1, t0=0.5, win=32768); sb0 = sideband_db(a.x, f0m, kmax=1, t0=0.5, win=32768)
        note('Vactrol', dst, 'zipper: %s, carrier %.0f Hz' % (nm, f0m), 'no spectral lines at carrier +- 344.5 Hz', 'strongest line at f0 +- k x 344.5 Hz: %.1f dB re carrier (unmodulated: %.1f dB)' % (sb, sb0), 'OK' if sb < (-50 if 'cut' in dst else -60) else 'CHECK')

if __name__ == '__main__':
    groups = sys.argv[1:] or ['sh', 'fx', 'seq', 'trk', 'zip']
    if 'sh' in groups: sh_tests()
    if 'fx' in groups: fx_tests()
    if 'seq' in groups: seq_tests()
    if 'trk' in groups: trk_tests()
    if 'zip' in groups: zip_tests()
    json.dump(ML.RES, open(os.path.join(WORK, 'results2_%s.json' % '_'.join(groups)), 'w'), indent=1)
