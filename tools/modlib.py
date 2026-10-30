#!/usr/bin/env python3
"""modlib.py - helpers of the modulation "listening test" (NOTES-K): run the host bench (host/modtest.c) and the prototype's own
engines (tools/proto_render.js), and measure what the modulation did to the signal (pitch, spectral centroid, rms, spectrogram)."""
import math, os, re, subprocess, wave
import numpy as np

FS = 44100
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
WORK = os.path.join(ROOT, 'work')
BIN = os.path.join(WORK, 'modtest')
os.makedirs(WORK, exist_ok=True)

# ---------------------------------------------------------------- running
def rate_for_hz(hz):                    # LFO rate knob (thousandths) for a frequency: .05 * 800^r Hz
    return int(round(1000 * math.log(hz / 0.05) / math.log(800)))
def hz_of_rate(r):
    return 0.05 * 800 ** (r / 1000.0)
def amt_for(x):                         # amount knob (thousandths) whose squared value is x (the knob-domain depth)
    return int(round(1000 * math.sqrt(x)))

class Res:
    pass

def run_native(name, script, probes=True):
    """script: text for host/modtest; returns Res(x=float32 mono, probe=dict name->array, chg={param: (base, min, max)})"""
    sp = os.path.join(WORK, name + '.txt'); fp = os.path.join(WORK, name + '.f32')
    cp = os.path.join(WORK, name + '.csv'); gp = os.path.join(WORK, name + '.chg')
    open(sp, 'w').write(script)
    env = dict(os.environ, CHG=gp)
    r = subprocess.run([BIN, sp, fp, cp], env=env, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError('modtest failed: ' + r.stderr + '\n' + script)
    res = Res()
    res.x = np.fromfile(fp, 'f4')
    res.name = name
    res.probe = {}
    if probes and os.path.getsize(cp) > 4:
        d = np.genfromtxt(cp, delimiter=',', names=True)
        d = np.atleast_1d(d)
        for k in d.dtype.names:
            res.probe[k] = d[k]
    res.chg = {}
    for ln in open(gp).read().splitlines():
        nm, b, mn, mx = ln.rsplit(',', 3)
        res.chg[nm] = (int(b), int(mn), int(mx))
    return res

PROTO_HTML = None
def run_proto(name, scen):
    sp = os.path.join(WORK, name + '.json'); fp = os.path.join(WORK, name + '.f32')
    import json
    open(sp, 'w').write(json.dumps(scen))
    subprocess.run(['node', os.path.join(ROOT, 'tools', 'proto_render.js'), sp, fp], check=True, capture_output=True)
    res = Res(); res.x = np.tanh(np.fromfile(fp, 'f4') * 0.84); res.name = name; res.probe = {}      # the firmware's track volume .8 x master 1.05 and tanh, so the two sides are on the same scale
    pj = fp + '.probe.json'
    if os.path.exists(pj):
        a = np.array(json.load(open(pj)), float)
        res.probe['t'] = a[:, 0]
        for i, q in enumerate(scen.get('probe', [])):
            res.probe[q] = a[:, 1 + i]
    return res

def write_wav(path, x, gain=1.0):
    y = np.clip(np.asarray(x, float) * gain, -1, 1)
    w = wave.open(path, 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(FS)
    w.writeframes((y * 32767).astype('<i2').tobytes()); w.close()

# ---------------------------------------------------------------- analysis
def stft(x, win=2048, hop=441, nfft=None):
    nfft = nfft or win
    w = np.hanning(win)
    n = 1 + max(0, (len(x) - win) // hop)
    idx = np.arange(win)[None, :] + hop * np.arange(n)[:, None]
    S = np.fft.rfft(x[idx] * w, nfft, axis=1)
    t = (hop * np.arange(n) + win / 2) / FS
    f = np.fft.rfftfreq(nfft, 1.0 / FS)
    return t, f, S

def rms_series(x, win=441, hop=221):
    n = 1 + max(0, (len(x) - win) // hop)
    idx = np.arange(win)[None, :] + hop * np.arange(n)[:, None]
    return (hop * np.arange(n) + win / 2) / FS, np.sqrt((x[idx] ** 2).mean(1))

def centroid_series(x, win=2048, hop=441, fmax=15000):
    t, f, S = stft(x, win, hop)
    m = np.abs(S)
    m[:, f > fmax] = 0
    return t, (m * f).sum(1) / np.maximum(m.sum(1), 1e-12)

def peak_series(x, win=4096, hop=441, lo=80, hi=8000):
    """strongest spectral line (dB) in lo..hi re the frame's rms: follows a resonant peak"""
    t, f, S = stft(x, win, hop)
    m = (f > lo) & (f < hi)
    mag = np.abs(S[:, m]) / (win / 4.0)
    rms = np.sqrt((np.abs(S) ** 2).sum(1) * 2) / (win / 2.0) / 1.5
    return t, 20 * np.log10(mag.max(1) / (rms + 1e-9) + 1e-9)

def pitch_series(x, fmin=60, fmax=2000, win=4096, hop=441):
    """normalised autocorrelation pitch tracker with parabolic interpolation; Hz (nan when unvoiced)"""
    n = 1 + max(0, (len(x) - win) // hop)
    out = np.full(n, np.nan); t = (hop * np.arange(n) + win / 2) / FS
    lo, hi = int(FS / fmax), int(FS / fmin)
    w = np.hanning(win)
    for i in range(n):
        s = x[i * hop:i * hop + win] * w
        if np.sqrt((s ** 2).mean()) < 1e-4:
            continue
        F = np.fft.rfft(s, 2 * win)
        ac = np.fft.irfft(F * np.conj(F))[:hi + 2]
        ac = ac / max(ac[0], 1e-12)
        seg = ac[lo:hi + 1]
        # first strong local maximum (avoids octave errors)
        pk = [k for k in range(1, len(seg) - 1) if seg[k] > seg[k - 1] and seg[k] >= seg[k + 1] and seg[k] > 0.5]
        if not pk:
            continue
        mx = max(seg[k] for k in pk)
        k = next(k for k in pk if seg[k] > 0.9 * mx) + lo
        a, b, c = ac[k - 1], ac[k], ac[k + 1]
        d = a - 2 * b + c
        k = k + (0.5 * (a - c) / d if d else 0)
        out[i] = FS / k
    return t, out

def zero_cross_pitch(x, hop=441, win=2205):
    """Hz from zero crossings per window (for clean tones)"""
    n = 1 + max(0, (len(x) - win) // hop)
    out = np.full(n, np.nan); t = (hop * np.arange(n) + win / 2) / FS
    for i in range(n):
        s = x[i * hop:i * hop + win]
        if np.sqrt((s ** 2).mean()) < 1e-4:
            continue
        z = np.where((s[:-1] < 0) & (s[1:] >= 0))[0]
        if len(z) >= 2:
            fr = z + (-s[z]) / (s[z + 1] - s[z])
            out[i] = FS * (len(z) - 1) / (fr[-1] - fr[0])
    return t, out

def mod_spectrum(t, v, tmin=0.0, fmax=30.0):
    """frequency (Hz) and depth (peak-to-peak, 5-95 percentile) of an oscillation of the feature v(t) sampled at t"""
    m = (t >= tmin) & np.isfinite(v)
    tt, vv = t[m], v[m]
    if len(vv) < 16:
        return float('nan'), float('nan'), 0.0
    dt = np.median(np.diff(tt))
    vv = vv - vv.mean()
    n = 1 << int(math.ceil(math.log2(len(vv) * 16)))
    F = np.abs(np.fft.rfft(vv * np.hanning(len(vv)), n))
    f = np.fft.rfftfreq(n, dt)
    sel = (f > 0.2) & (f < fmax)
    k = np.argmax(F[sel]); f0 = f[sel][k]
    # how much of the variance is at f0 (+-8 %) and its first harmonics
    p = F ** 2
    tot = p[sel].sum()
    band = sel & (np.abs(f - f0) < 0.12 * f0 + 0.15)
    return f0, np.percentile(vv, 95) - np.percentile(vv, 5), float(p[band].sum() / max(tot, 1e-30))

def cents(f, ref):
    return 1200 * np.log2(f / ref)

def lsd_db(a, b, win=2048, hop=1024, floor_db=-70):
    """log-spectral distance between two renders (dB, over the bins where either is above floor re the loudest), and the rms-level difference in dB"""
    n = min(len(a), len(b)); a, b = a[:n], b[:n]
    _, f, A = stft(a, win, hop); _, _, B = stft(b, win, hop)
    A = np.abs(A) + 1e-9; B = np.abs(B) + 1e-9
    ref = max(A.max(), B.max())
    msk = ((20 * np.log10(A / ref) > floor_db) | (20 * np.log10(B / ref) > floor_db)) & (f[None, :] > 30) & (f[None, :] < 16000)
    d = 20 * np.log10(A / B)
    lsd = float(np.sqrt((d[msk] ** 2).mean())) if msk.any() else 0.0
    ra, rb = np.sqrt((a ** 2).mean()) + 1e-9, np.sqrt((b ** 2).mean()) + 1e-9
    return lsd, 20 * np.log10(ra / rb)

def click_metric(x, fs=FS):
    """zipper / click indicator: energy of the second difference above 8 kHz relative to a smooth version; returns (peak |d2| / rms|d2|) and the
    ratio (dB) of the 1-8 ms blockwise high-frequency residue - used on pure tones where the ideal result has no such residue"""
    d2 = np.diff(x, 2)
    return float(np.max(np.abs(d2)) / (np.sqrt((d2 ** 2).mean()) + 1e-12))

def sideband_db(x, f0, spacing=FS / 128.0, kmax=3, win=16384, t0=0.5):
    """level (dB re the carrier) of the strongest spectral line at f0 +- k * 344.5 Hz (the 128-sample block rate): zipper-noise lines"""
    s = x[int(t0 * FS):int(t0 * FS) + win]
    s = s * np.hanning(len(s))
    F = np.abs(np.fft.rfft(s, 4 * len(s)))
    f = np.fft.rfftfreq(4 * len(s), 1.0 / FS)
    car = F[(np.abs(f - f0) < 20)].max()
    best = -200
    for k in range(1, kmax + 1):
        for sg in (-1, 1):
            fk = f0 + sg * k * spacing
            if 20 < fk < FS / 2 - 20:
                band = F[np.abs(f - fk) < 12]
                if len(band):
                    best = max(best, 20 * np.log10(band.max() / car))
    return best

# ---------------------------------------------------------------- figures
def figure(path, title, series, spec_of=None, xlim=None):
    """series: list of panels; each panel = (ylabel, [(label, t, v)...] [, extra dict]); spec_of = (label, x) draws a spectrogram on top"""
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    n = len(series) + (1 if spec_of else 0)
    fig, ax = plt.subplots(n, 1, figsize=(9, 2.1 * n + 0.6), sharex=True)
    ax = np.atleast_1d(ax); i = 0
    if spec_of:
        lab, x = spec_of
        t, f, S = stft(x, 2048, 441)
        db = 20 * np.log10(np.abs(S).T + 1e-7)
        ax[0].pcolormesh(t, f / 1000, np.clip(db, db.max() - 80, None), shading='auto', cmap='magma')
        ax[0].set_ylim(0, 8); ax[0].set_ylabel('kHz'); ax[0].set_title(lab, fontsize=8, loc='left'); i = 1
    for ylabel, curves in series:
        for lab, t, v in curves:
            ax[i].plot(t, v, lw=1.0, label=lab)
        ax[i].set_ylabel(ylabel, fontsize=8); ax[i].grid(alpha=.3)
        if len(curves) > 1 or curves[0][0]:
            ax[i].legend(fontsize=7, loc='upper right', ncol=3)
        i += 1
    ax[-1].set_xlabel('s')
    if xlim:
        ax[-1].set_xlim(*xlim)
    fig.suptitle(title, fontsize=10)
    fig.tight_layout()
    fig.savefig(path, dpi=100)
    plt.close(fig)


# ---------------------------------------------------------------- native script -> the prototype's parameters (for the side-by-side runs)
MG_KEYS = ['mg1w', 'mg1r', 'mg1t', 'mg1p', 'mg1l', 'mg2w', 'mg2r', 'mg2t', 'mg2p', 'mg2l', 'mg3w', 'mg3r', 'mg3t', 'mg3p', 'mg3l', 'mgsw', 'mgso', 'mgst', 'mgsl',
           'mgcut', 'mgres', 'mgenv', 'mggl', 'mgrel', 'atk', 'edec', 'sus', 'fatk', 'fdec', 'fsus', 'lvl']
PR_KEYS = ['pwave', 'parng', 'patune', 'ppw', 'pbwave', 'pbrng', 'pbdet', 'pbpw', 'pla', 'plb', 'psw', 'pso', 'pst', 'psp', 'psl', 'pnt', 'pno', 'pnc', 'pnl',
           'pcut', 'pres', 'penv', 'ppm', 'atk', 'edec', 'sus', 'rel', 'fatk', 'fdec', 'fsus', 'frel', 'lvl']
MG_NAMES = ['MG_W1', 'MG_R1', 'MG_T1', 'MG_P1', 'MG_L1', 'MG_W2', 'MG_R2', 'MG_T2', 'MG_P2', 'MG_L2', 'MG_W3', 'MG_R3', 'MG_T3', 'MG_P3', 'MG_L3', 'MG_NTYPE', 'MG_NTONE',
            'MG_NCRUSH', 'MG_NLEV', 'MG_CUT', 'MG_RES', 'MG_ENV', 'MG_GLIDE', 'MG_REL', 'MG_ATK', 'MG_DEC', 'MG_SUS', 'MG_FATK', 'MG_FDEC', 'MG_FSUS', 'MG_LVL']
PR_NAMES = ['PR_WAVE', 'PR_RNG', 'PR_TUNE', 'PR_PW', 'PR_BWAVE', 'PR_BRNG', 'PR_BTUNE', 'PR_BPW', 'PR_LA', 'PR_LB', 'PR_SW', 'PR_SO', 'PR_ST', 'PR_SP', 'PR_SL', 'PR_NTYPE',
            'PR_NTONE', 'PR_NCRUSH', 'PR_NLEV', 'PR_CUT', 'PR_RES', 'PR_ENV', 'PR_PM', 'PR_ATK', 'PR_DEC', 'PR_SUS', 'PR_REL', 'PR_FATK', 'PR_FDEC', 'PR_FSUS', 'PR_FREL', 'PR_LVL']
DR_NAMES = {'DR_KIT': 'dkit', 'DR_TYPE': 'dty', 'DR_KV': 'dkv', 'DR_TUNE': 'dtu', 'DR_DECAY': 'ddc', 'DR_TONE': 'dtn', 'DR_LEVEL': 'dlv', 'DR_ATK': 'dat', 'DR_PEAK': 'dpe',
            'DR_HOLD': 'dhd', 'DR_DRIVE': 'ddr', 'DR_FILT': 'dfl', 'DR_SW': 'dsw', 'DR_HUM': 'dhu', 'DR_CHANCE': 'dch', 'DR_ACC': 'dac'}
SEL_KEYS = {'mg1w', 'mg1r', 'mg2w', 'mg2r', 'mg3w', 'mg3r', 'mgsw', 'mgrel', 'parng', 'pbrng', 'psw', 'pso', 'pnt', 'pwave_', 'dty', 'dkv', 'dkit'}
def proto_time(key, v):
    f = v / 1000.0
    if key in ('atk', 'fatk'): return .001 * 2000 ** f
    if key in ('edec', 'rel', 'fdec', 'frel'): return .005 * 600 ** f
    return None
def proto_from_native(engine, lines):
    """the prototype's parameter dict for the 'ep T NAME VAL' lines of a native script (engine 1 Moog, 2 Prophet, 3 Drums)"""
    d = {}
    for ln in lines:
        p = ln.split()
        if len(p) != 4 or p[0] != 'ep': continue
        nm, v = p[2], int(p[3])
        if engine == 1 and nm in MG_NAMES: key = MG_KEYS[MG_NAMES.index(nm)]
        elif engine == 2 and nm in PR_NAMES: key = PR_KEYS[PR_NAMES.index(nm)]
        elif engine == 3:
            b, _, vi = nm.partition('.')
            if b not in DR_NAMES: continue
            key = DR_NAMES[b] + (vi if b != 'DR_KIT' else '')
        else: continue
        k0 = re.sub(r'\d$', '', key) if engine == 3 else key
        tm = proto_time(key, v)
        if tm is not None: d[key] = tm
        elif k0 in SEL_KEYS or key in SEL_KEYS: d[key] = v
        else: d[key] = v / 1000.0
    return d

def similarity(x1, x2):
    """correlation of the level (dB) and centroid (log Hz) trajectories of two renders: how alike they move (1 = identical movements)"""
    n = min(len(x1), len(x2)); x1, x2 = x1[:n], x2[:n]
    _, r1 = rms_series(x1, 1102, 551); _, r2 = rms_series(x2, 1102, 551)
    _, c1 = centroid_series(x1, 2048, 551); _, c2 = centroid_series(x2, 2048, 551)
    out = []
    for a, b in ((20 * np.log10(r1 + 1e-6), 20 * np.log10(r2 + 1e-6)), (np.log(c1 + 1), np.log(c2 + 1))):
        k = min(len(a), len(b)); a, b = a[:k], b[:k]
        if a.std() < 1e-6 or b.std() < 1e-6: out.append(1.0 if abs(a.mean() - b.mean()) < 0.5 else 0.0)
        else: out.append(float(np.corrcoef(a, b)[0, 1]))
    return min(out), out
