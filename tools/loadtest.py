#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""octavio-J worst-case load test driver (NOTES-J).

    python3 tools/loadtest.py list                 the variants
    python3 tools/loadtest.py run NAME [NAME..]    build + run in the emulator + report (up to 3 emulators at once)
    python3 tools/loadtest.py report NAME [..]     only (re)print the report of finished runs
    python3 tools/loadtest.py table                the summary table of every finished run

Environment: JIELI_TOOLCHAIN, AC79_SDK (as build.sh), FM1_EMU (default /home/claude/fw/fm1-emu/target/release/fm1-emu),
LT_OUT (default <repo>/build/loadtest).  Each variant is a firmware built with -DLOADTEST (src/loadtest.c) plus XCFLAGS,
run for MS ms of emulated time with a --keys script; the firmware's `perf` struct (src/fxstate.c) is read back with --dump.
Ticks are the emulator's TIMER4 ticks (24 MHz, instruction-count based): NOT silicon ticks (see NOTES-J)."""
import os, re, subprocess, sys, json, struct, time, wave

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.environ.get("LT_OUT", os.path.join(HERE, "build", "loadtest"))
EMU = os.environ.get("FM1_EMU", "/home/claude/fw/fm1-emu/target/release/fm1-emu")
P = 69632                                  # one audio block in ticks (128 frames x 544 in the emulator)
ENG = "ENG_VAC,ENG_MOOG,ENG_PROPH,ENG_DRUM"

# --keys scripts (ms from boot; the audio runs from ~0.45 s and the statistics start 200 blocks, ~0.6 s, later).
# Every page is visited, every knob turned, two note keys held, HOME+SELECT changes the selected track, the master pot moves.
UI = ",".join([
    "1300:FX", "1400:K1:+8", "1450:K2:-6", "1500:K3:+10", "1550:K4:+3",
    "1700:LFO", "1800:K1:+5", "1850:K2:+5", "1900:K3:+7", "1950:K4:-3",
    "2100:ENV", "2200:K1:+5", "2250:K2:+5", "2300:K3:+5", "2350:K4:+5",
    "2500:ARP", "2600:K1:+2", "2650:K2:+1", "2700:K3:+9", "2750:K4:+9",
    "2900:SEQ", "3000-3300:n7", "3100-3400:n12", "3150:K1:+4", "3200:K2:+6",
    "3600:EDIT", "3700:K1:+6", "3750:K2:-6", "3800:K3:+6", "3850:K4:-6",
    "4000:GLO", "4100:K1:+4", "4150:K2:+4", "4200:K3:+4",
    "4400-4900:HOME", "4600:enc0:+1", "4750:enc0:+1",
    "5000-5100:FX", "5150:K1:+8", "5200:K2:-6", "5250:K3:+10", "5300:K4:+3",
    "5500-6100:FX", "5530-5630:n2", "5900-6000:n9",              # FX held + a white key: effect 1 (phaser) into the slot 30 ms after FX went down, effect 5 (delay) 400 ms after
    "6200:pot:300", "6300-6600:n5", "6400-6700:n9", "6500-6800:n14", "6600:pot:700",
    "6900:LFO", "6950:K1:-5", "7000:K2:-5", "7050:K3:-7", "7100:K4:+3",
    "7250:ENV", "7300:K1:-5", "7350:K2:-5",
    "7500:HOME", "7600:SCL",
])
UI_MS = 8000
# what the script injects (compared with what the main loop took): encoder clicks, button presses (note keys and the master pot are not buttons)
def injected(keys):
    enc = btn = notes = 0
    for it in keys.split(","):
        p = it.split(":")
        w = p[1].upper()
        if w == "POT": continue
        if w.startswith("K") and len(p) > 2 or w.startswith("ENC") and len(p) > 2: enc += abs(int(p[2])); continue
        if w.startswith("N") and w[1:].isdigit(): notes += 1; continue
        btn += 1
    return dict(enc=enc, btn=btn, notes=notes)

def V(xc="", ms=3800, keys="", snap="", eng=None, fx=None):
    f = ["-DLOADTEST"]
    if eng: f.append("-DLT_ENG={%s}" % eng)
    if fx: f.append("-DLT_FX=%s" % fx)
    return dict(xcflags=" ".join(f) + " " + xc, ms=ms, keys=keys, snap=snap)

def E(*e): return ",".join("ENG_" + x for x in e)

NOGOV = "-DNO_GOV "
FX4 = "{{0,1,2,3},{0,1,2,3},{0,1,2,3},{0,1,2,3}}"           # overdrive, phaser, flanger, chorus on every track (Mix > 0): the factory chain once switched on
def G(eng, act, fx=False, xc="", ms=3800, keys=""):
    xc = "-DLT_ACTIVE=%d " % act + ("" if "NSRC" in xc else "-DLT_NOMOD ") + xc + " " + ("" if fx else "-DLT_FXMASK=0 ")
    return V(xc, ms=ms, keys=keys, eng=E(*eng), fx=FX4 if fx else None)
S3 = ("VAC", "MOOG", "PROPH", "MOOG"); S21 = ("MOOG", "PROPH", "MOOG", "DRUM"); S12 = ("PROPH", "MOOG", "MOOG", "DRUM"); # the Minimoog (track 2, selected at boot) is played by hand: single notes repeated, held notes, chords; Vactrol and drums run on their own (arp / step clock, a little modulation)
PLAYM = ",".join(["1500-1800:n7", "1900-2200:n9", "2300-2600:n12", "2700-2800:n7", "2900-3000:n7", "3100-3200:n7", "3300-3400:n7", "3500-3600:n7",
                  "3700-4700:n7", "3750-4750:n12", "3800-4800:n14", "3850-4800:n16", "4900-4950:n5", "5000-5050:n8", "5100-5150:n10", "5200-5250:n13", "5300-5350:n5", "5400-5450:n8"])
def M(xc="", fx=False, ms=6000, eng=None):
    return G(eng or VMD, 9, fx, "-DLT_NSRC=2 -DLT_SEL=1 " + xc, ms=ms, keys=PLAYM)
VMD = ("VAC", "MOOG", "PROPH", "DRUM"); S3B = ("MOOG", "PROPH", "MOOG", "PROPH")
SAVE = "3000:SAVE,3700:SAVE"
VARIANTS = {
    # idle reference: the normal firmware (no LOADTEST), same key script -> UI cost without audio load
    "idle_ui":     dict(xcflags="", ms=UI_MS, keys=UI, snap="1500,3000,4500"),
    "fxgest_idle": dict(xcflags="", ms=6300, keys=UI, snap="5450,5750,6150"),                              # does FX held + a white key reassign a slot (the fx_alloc spike)? idle firmware, fast
    "gov_fxgest":  V(ms=6300, keys=UI, snap="5450,5750,6150"),                                            # the same gesture under load: does it take?
    "lat_idle":    dict(xcflags="", ms=2800, keys="900-1400:HOME,1000:enc0:+3,1700-1900:n7,2200-2400:n12", snap=""),      # HOME + SELECT picks track 4 (drums): a key hits a drum voice at once
    # -- raw (governor off, -DNO_GOV = the firmware as it was): what the machine costs --
    "eng_only":    V(NOGOV + "-DLT_FXMASK=0 -DLT_NOMOD"),                 # engines only (no chain, no mod): VAC MOOG PROPH DRUM, arp + step clock
    "eng_mod":     V(NOGOV + "-DLT_FXMASK=0"),                            # + the 24 mod routes
    "fx_budget":   V(NOGOV),                                              # + the chains, admitted by the static FX_BUDGET of NOTES-H
    "fx_nobudget": V(NOGOV + "-DFX_BUDGET=1000000"),                      # all 16 slots running
    "drum4":       V(NOGOV, eng=E("MOOG", "MOOG", "DRUM", "DRUM")),
    "moog4":       V(NOGOV, eng=E("MOOG", "MOOG", "MOOG", "MOOG")),
    "vc34":       V(NOGOV + "-DLT_FXMASK=0 -DLT_NOMOD", eng=E("VC3", "VC3", "VC3", "VC3")),
    "jun4fx":     V(NOGOV, eng=E("JUN", "JUN", "JUN", "JUN")),                                                  # 4 juno + the whole effects chain + mod
    "jun4":       V(NOGOV + "-DLT_FXMASK=0 -DLT_NOMOD", eng=E("JUN", "JUN", "JUN", "JUN")),
    "df4":       V(NOGOV + "-DLT_FXMASK=0 -DLT_NOMOD", eng=E("DFM", "DFM", "DFM", "DFM")),
    "proph4":      V(NOGOV, eng=E("PROPH", "PROPH", "PROPH", "PROPH")),
    "full_ui":     V(NOGOV, ms=UI_MS, keys=UI, snap="1500,3000,4500"),    # the worst case measured, front panel driven
    # -- with the governor (the shipped firmware) --
    "gov_fx_budget":   V(),
    "gov_fx_nobudget": V("-DFX_BUDGET=1000000"),
    "gov_drum4":       V(eng=E("MOOG", "MOOG", "DRUM", "DRUM")),
    "gov_moog4":       V(eng=E("MOOG", "MOOG", "MOOG", "MOOG")),
    "gov_full_ui":     V(ms=UI_MS, keys=UI, snap="1500,3000,4500"),
    "gov_drum4_ui":    V(ms=UI_MS, keys=UI, eng=E("MOOG", "MOOG", "DRUM", "DRUM")),
    "gov_nobudget_ui": V("-DFX_BUDGET=1000000", ms=UI_MS, keys=UI),
    # -- glitch hunt (NOTES-M): three tracks play, the fourth is silent; chain: none / OD+PH+FL+CH; the shipped firmware (gov) and the raw one --
    "g3syn":      G(S3, 7), "g3syn_fx": G(S3, 7, True), "g3synB_fx": G(S3B, 7, True),
    "g21":        G(S21, 7), "g21_fx": G(S21, 7, True),
    "g12":        G(S12, 7), "g12_fx": G(S12, 7, True),
    "r3syn_fx":   G(S3, 7, True, NOGOV), "r21_fx": G(S21, 7, True, NOGOV), "r12_fx": G(S12, 7, True, NOGOV),
    "g12_noarp":  G(S12, 7, True, "-DLT_NOARP"), "g3syn_noseq": G(S3, 7, True, "-DLT_NOSEQ"),
    # the user's case: Vactrol + Minimoog + drums (one track each), the fourth silent
    "u_vmd":      G(VMD, 11), "u_vmd_fx": G(VMD, 11, True), "u_vmd_noseq": G(VMD, 11, True, "-DLT_NOSEQ"), "u_vmd_noarp": G(VMD, 11, True, "-DLT_NOARP"),
    "u_vmd_mod":  G(VMD, 11, False, "-DLT_NSRC=2", keys=""), "u_vmd_fx_mod": G(VMD, 11, True, "-DLT_NSRC=2"),
    "ur_vmd_mod": G(VMD, 11, False, NOGOV + "-DLT_NSRC=2"),
    "dr1": G(("MOOG", "MOOG", "MOOG", "DRUM"), 8, False, NOGOV, ms=3000), "ph1c": G(("MOOG", "PROPH", "MOOG", "MOOG"), 2, False, NOGOV + "-DLT_CHORD=5", ms=3000), "ph3c": G(("MOOG", "PROPH", "MOOG", "MOOG"), 2, False, NOGOV + "-DLT_CHORD=3", ms=3000), "ph1cm": G(("MOOG", "PROPH", "MOOG", "MOOG"), 2, False, NOGOV + "-DLT_CHORD=1", ms=3000), "ph1": G(("MOOG", "PROPH", "MOOG", "MOOG"), 2, False, NOGOV, ms=3000),
    "mg1": G(("MOOG", "MOOG", "MOOG", "MOOG"), 2, False, NOGOV, ms=3000),
    # M: the new worst cases: two drum tracks (six voices each), Prophet tracks holding a 5-note chord (all their voices sound), heavy chains, 24 mod routes
    "wc": V("-DLT_CHORD=5", eng=E("PROPH", "PROPH", "PROPH", "DRUM")), "wc_raw": V(NOGOV + "-DLT_CHORD=5", eng=E("PROPH", "PROPH", "PROPH", "DRUM")),
    "wc_ui": V("-DLT_CHORD=5", ms=UI_MS, keys=UI, snap="1500,3000,4500", eng=E("PROPH", "PROPH", "PROPH", "DRUM")),
    "wm": V("-DLT_CHORD=5", eng=E("VAC", "MOOG", "MOOG", "DRUM")), "wm_raw": V(NOGOV + "-DLT_CHORD=5", eng=E("VAC", "MOOG", "MOOG", "DRUM")),
    "wp4_raw": V(NOGOV + "-DLT_CHORD=5", eng=E("PROPH", "PROPH", "PROPH", "PROPH")), "wp4": V("-DLT_CHORD=5", eng=E("PROPH", "PROPH", "PROPH", "PROPH")),
    "wc_nofx": V(NOGOV + "-DLT_CHORD=5 -DLT_FXMASK=0 -DLT_NOMOD", eng=E("PROPH", "PROPH", "PROPH", "DRUM")),
    **{f"dv{v}": G(("MOOG", "MOOG", "MOOG", "DRUM"), 8, False, NOGOV + f"-DLT_DLANES={1 << v}", ms=2500) for v in range(6)},       # one drum voice at a time: its cost
    **{f"umb{b}": M(f"-DLT_BURN={b * 696}", True) for b in (0, 20, 40, 60)},      # the user's case (Vactrol + hand-played Minimoog + drums, default chains) on a machine slower by b % of a block
    **{f"umbn{b}": M(f"-DLT_BURN={b * 696} " + NOGOV, True) for b in (0, 20, 40, 60)},
    **{f"wcb{b}": V(f"-DLT_CHORD=5 -DLT_BURN={b * 696}", eng=E("PROPH", "PROPH", "PROPH", "DRUM")) for b in (0, 20)},
    # M: the polyphonic Prophet alone (track 2, notes played from the keys): 5-note chord with staggered releases, then a sixth key (steal)
    "pchord": G(("MOOG", "PROPH", "MOOG", "MOOG"), 2, False, "-DLT_NSRC=0 -DLT_SEL=1 -DLT_NOARP -DLT_NOSEQ -DLT_NOMOD", ms=6500,
                keys="1000-2500:n0,1000-2800:n2,1000-3100:n4,1000-3400:n7,1000-3700:n9,4300-5900:n0,4300-5900:n2,4300-5900:n4,4300-5900:n7,4300-5900:n9,5000-5500:n11"),
    # M: variants of the drum engine (NOTES-M): lanes 0-3 only (B), and the full worst case Minimoog + 5-note Prophet + two drum tracks + chains + 24 routes (wf, governed)
    "drB": G(("MOOG", "MOOG", "MOOG", "DRUM"), 8, False, NOGOV + "-DLT_DLANES=15", ms=3000),
    "wf": V("-DLT_CHORD=5", eng=E("VAC", "MOOG", "PROPH", "DRUM")), "wfB": V("-DLT_CHORD=5 -DLT_DLANES=15", eng=E("VAC", "MOOG", "PROPH", "DRUM")),
    "wf_raw": V(NOGOV + "-DLT_CHORD=5", eng=E("VAC", "MOOG", "PROPH", "DRUM")), "wfB_raw": V(NOGOV + "-DLT_CHORD=5 -DLT_DLANES=15", eng=E("VAC", "MOOG", "PROPH", "DRUM")),
    # M: HOME's track volume (fader K<track> on HOME, held note / drum pattern, wav rms per window must follow the volume)
    "pvol_m": G(("MOOG", "MOOG", "MOOG", "MOOG"), 2, False, "-DLT_NSRC=0 -DLT_SEL=1 -DLT_NOARP -DLT_NOSEQ -DLT_NOMOD", ms=7500,
                keys="1000-7500:n7,2000:HOME,2400:K2:-20,3600:K2:-20,4800:K2:-20,6000:K2:-20"),
    "pvol_p": G(("MOOG", "PROPH", "MOOG", "MOOG"), 2, False, "-DLT_NSRC=0 -DLT_SEL=1 -DLT_NOARP -DLT_NOSEQ -DLT_NOMOD", ms=7500,
                keys="1000-7500:n7,1000-7500:n9,2000:HOME,2400:K2:-20,3600:K2:-20,4800:K2:-20,6000:K2:-20"),
    "pvol_d": G(("MOOG", "MOOG", "MOOG", "DRUM"), 8, False, "-DLT_NSRC=0 -DLT_SEL=3 -DLT_NOMOD", ms=7500,
                keys="2000:HOME,2400:K4:-20,3600:K4:-20,4800:K4:-20,6000:K4:-20"),
    "pvol_f": G(("MOOG", "MOOG", "MOOG", "MOOG"), 2, True, "-DLT_NSRC=0 -DLT_SEL=1 -DLT_NOARP -DLT_NOSEQ -DLT_NOMOD", ms=7500,
                keys="1000-7500:n7,2000:HOME,2400:K2:-20,3600:K2:-20,4800:K2:-20,6000:K2:-20"),
    # M: the user's Prophet test: Vactrol, Minimoog and drums play on their own (arp / step clock, default chains, 24 routes), a 5-key chord is held on the Prophet (track 2) from the keys
    "pk5": G(("VAC", "PROPH", "MOOG", "DRUM"), 13, True, "-DLT_NSRC=2 -DLT_SEL=1", ms=7000,
             keys="2500-6000:n0,2500-6000:n2,2500-6000:n4,2500-6000:n7,2500-6000:n9"),
    # M3: the user's three tracks: Vactrol + Prophet (5-note chord held) + drums in sequence, default chains, a little modulation (u3 governed, u3r raw)
    "u3": G(("VAC", "MOOG", "PROPH", "DRUM"), 13, True, "-DLT_NSRC=2 -DLT_CHORD=5"),
    "u3r": G(("VAC", "MOOG", "PROPH", "DRUM"), 13, True, NOGOV + "-DLT_NSRC=2 -DLT_CHORD=5"),
    # M3: the same with a plausible drum pattern (BD SD CH OH, every 8th note, moderate decay): what a player really sets up
    "u3d": G(("VAC", "MOOG", "PROPH", "DRUM"), 13, True, "-DLT_NSRC=2 -DLT_CHORD=5 -DLT_DLANES=15 -DLT_DGATE=0x5555 -DLT_DDEC=500"),
    "u3dr": G(("VAC", "MOOG", "PROPH", "DRUM"), 13, True, NOGOV + "-DLT_NSRC=2 -DLT_CHORD=5 -DLT_DLANES=15 -DLT_DGATE=0x5555 -DLT_DDEC=500"),
    "pchord_eco": G(("MOOG", "PROPH", "MOOG", "MOOG"), 2, False, NOGOV + "-DLT_ECO -DLT_NSRC=0 -DLT_SEL=1 -DLT_NOARP -DLT_NOSEQ -DLT_NOMOD", ms=6500,
                keys="1000-2500:n0,1000-2800:n2,1000-3100:n4,1000-3400:n7,1000-3700:n9,4300-5900:n0,4300-5900:n2,4300-5900:n4,4300-5900:n7,4300-5900:n9,5000-5500:n11"),
    "ph5": G(("MOOG", "PROPH", "MOOG", "MOOG"), 2, False, NOGOV + "-DLT_CHORD=5", ms=3000), "ph5e": G(("MOOG", "PROPH", "MOOG", "MOOG"), 2, False, NOGOV + "-DLT_CHORD=5 -DLT_ECO", ms=3000),
    "u3hud": dict(G(("VAC", "MOOG", "PROPH", "DRUM"), 13, True, "-DLT_NSRC=2 -DLT_CHORD=5 -DLT_DLANES=15 -DLT_DGATE=0x5555 -DLT_DDEC=500", ms=5200, keys="1500-2200:HOME,1700-1800:REC"), snap="3000,4800"),
    "u3ne": G(("VAC", "MOOG", "PROPH", "DRUM"), 13, True, "-DLT_NSRC=2 -DLT_CHORD=5 -DGOV_ECO=0"),
    "u3er": G(("VAC", "MOOG", "PROPH", "DRUM"), 13, True, NOGOV + "-DLT_NSRC=2 -DLT_CHORD=5 -DLT_ECO"),
    **{f"mt{n}": G(VMD, 15, True, f"-DLT_NSRC=2 -DLT_SEL=1 -DLT_CHORD=5 -DMAXTR_DEF={n} " + NOGOV, ms=6000, keys=PLAYM) for n in (1, 2, 3, 4)},              # MAX TRACKS n (raw cost, no governor), Vactrol + Minimoog (selected) + Prophet + drums, default chains
    "mtchg": dict(G(VMD, 15, True, "-DLT_NSRC=2 -DLT_SEL=1 -DLT_CHORD=5 " + NOGOV, ms=6000, keys=PLAYM), keys=PLAYM + ",2000-3300:HOME,2100:PLAY,2500:PLAY,2900:PLAY"),   # HOME + PLAY three times while it plays: 4 -> 3 -> 2 -> 1
    "mthome": dict(G(VMD, 15, True, "-DLT_NSRC=2 -DLT_SEL=1 -DLT_CHORD=5 " + NOGOV, ms=6000, keys=PLAYM), keys=PLAYM + ",2000-3300:HOME"),   # control: HOME held, no PLAY
    "um": M(), "um_fx": M(fx=True), "umr": M(NOGOV), "umr_fx": M(NOGOV, True),
    "ur_vmd_fx":  G(VMD, 11, True, NOGOV), "ur_vmd": G(VMD, 11, False, NOGOV),
    "gov_save_ui":     V(ms=6500, keys=SAVE),                              # a preset SAVE (flash erase + program with the interrupts off) under load
    "raw_save_ui":     V(NOGOV, ms=6500, keys=SAVE),
}

def sh(cmd, **kw):
    return subprocess.run(cmd, shell=True, check=True, capture_output=True, text=True, **kw)

def build(name):
    v = VARIANTS[name]
    d = os.path.join(OUT, name); os.makedirs(os.path.join(d, "work"), exist_ok=True)
    tc = os.environ["JIELI_TOOLCHAIN"]
    env = dict(os.environ, XCFLAGS=v["xcflags"])
    subprocess.run([os.path.join(HERE, "build.sh")], cwd=HERE, env=env, check=True, capture_output=True)
    sh(f"python3 {HERE}/tools/mkpkg.py --bin {HERE}/build/octavio.bin --out {d}/fw.fwsc")
    syms = sh(f"{tc}/common/bin/objdump -t {HERE}/build/octavio.elf").stdout
    addr = re.search(r"^([0-9a-f]+) .*\sperf$", syms, re.M).group(1)
    bss_end = re.search(r"^([0-9a-f]+) .*\s_bss_end$", syms, re.M).group(1)
    json.dump(dict(perf=addr, bss_end=bss_end), open(os.path.join(d, "syms.json"), "w"))
    open(os.path.join(d, "symtab.txt"), "w").write(syms)                      # tools/profile.py maps the emulator's PC trace to functions with it
    return d, addr

def start(name):
    v = VARIANTS[name]
    d, addr = build(name)
    cmd = [EMU, "--fwsc", "fw.fwsc", "--ms", str(v["ms"]), "--wav", "out.wav", "--dump", addr, "423"]
    if v["keys"]: cmd += ["--keys", v["keys"]]
    if v["snap"]: cmd += ["--snap", v["snap"]]
    t0 = time.time()
    p = subprocess.Popen(["timeout", os.environ.get("LT_TIMEOUT", "590")] + cmd, cwd=d, stdout=open(os.path.join(d, "emu.txt"), "w"), stderr=subprocess.STDOUT)
    return p, t0, d

NAMES = ["audio_max", "audio_last", "audio_sum", "blocks", "audio_max_w", "fx_max", "fx_last", "mod_max", "magic", "nw", "sum_w", "n75", "n90", "n100",
         "uf", "uf_first", "gap_min", "gap_max", "last_in", "fx_sum", "mod_sum", "mod_last", "mod_n", "loop_n", "loop_gap_max", "last_loop",
         "frame_n", "frame_sum", "frame_max", "full_n", "full_max", "frame_gap_max", "last_frame", "rows_sum", "key_n", "key_sum", "key_max", "key_blk_max",
         "enc_steps", "btn_n", "ustack_free", "sstack_free", "bss_end", "ram_free"]

def parse(d):
    txt = open(os.path.join(d, "emu.txt")).read()
    w = []
    for line in txt.splitlines():
        m = re.match(r"\s+0x[0-9a-f]+:((?: [0-9a-f]{8})+)", line)
        if m: w += [int(x, 16) for x in m.group(1).split()]
    if len(w) < 103 or w[8] != 0x4A504552: return None
    r = dict(zip(NAMES, w))
    r["trace"] = [x for v in w[103:423] for x in (v & 0xFFFF, v >> 16)] if len(w) >= 423 else []
    r["hist"] = w[44:68]; r["lt"] = w[68:76]
    r["te_sum"] = w[76:80]; r["te_max"] = w[80:84]; r["tf_sum"] = w[84:88]; r["tf_max"] = w[88:92]; r["gov"] = w[92:100]; r["alloc"] = w[100:103]
    return r

def wav_stats(d):
    import numpy as np
    fn = os.path.join(d, "out.wav")
    if not os.path.exists(fn): return {}
    w = wave.open(fn); n = w.getnframes(); a = np.frombuffer(w.readframes(n), dtype="<i2").reshape(-1, 2).astype(np.int32)
    nb = n // 128
    b = a[:nb * 128].reshape(nb, 128 * 2)
    rep = 0
    for k in range(3, nb):                        # a half played twice (never refilled) = block k identical to block k-2 and not silent
        if b[k].any() and (b[k] == b[k - 2]).all(): rep += 1
    nz = np.nonzero(b.any(axis=1))[0]
    onset = int(nz[0]) if len(nz) else -1
    x = a[:, 0].astype(float); d2 = np.abs(np.diff(x, 2))
    sc = np.convolve(d2, np.ones(257) / 257, "same")
    fl = np.nonzero((d2 > 6 * sc + 700) & (np.arange(len(d2)) > 128 * 40))[0]       # a second-difference spike far above the local average (after the start-up)
    ev = []
    for i in fl:
        if not ev or i - ev[-1][0] > 16: ev.append([int(i), float(d2[i]), float(sc[i])])
    clicks = len(ev); atb = sum(1 for e in ev if (e[0] + 1) % 128 in (0, 1, 2, 127))
    top = sorted(ev, key=lambda e: -e[1] / (e[2] + 1))[:4]
    wl = 4410; wr = [float(np.sqrt((x[i:i + wl] ** 2).mean())) for i in range(0, len(x) - wl, wl)]
    wr2 = wr[8:]; med = float(np.median(wr2)) if wr2 else 0.0
    holes = sum(1 for r in wr2 if r < 0.25 * med)                                   # 100 ms windows far under the median: a hole in the sound
    peak = int(np.abs(a).max()); rms = float(np.sqrt((a[nb // 4 * 128:].astype(float) ** 2).mean()))
    return dict(win_rms_min=round(min(wr2), 1) if wr2 else 0, win_rms_med=round(med, 1), holes=holes, clicks=clicks, clicks_at_block_edge=atb, click_top=[(round(e[0] / 44100, 3), int(e[1]), int(e[2])) for e in top], frames=n, blocks=nb, repeated=rep, peak=peak, rms=round(rms, 1), first_nonzero_block=onset)

def report(name, wall=None):
    d = os.path.join(OUT, name)
    r = parse(d)
    if not r:
        print(f"[{name}] no perf dump (see {d}/emu.txt)"); return None
    v = VARIANTS[name]
    nw = max(r["nw"], 1)
    avg = r["sum_w"] / nw
    r.update(name=name, avg=avg, avg_pct=100 * avg / P, max_pct=100 * r["audio_max_w"] / P,
             fx_avg=r["fx_sum"] / nw, mod_avg=r["mod_sum"] / nw)
    r["eng_avg"] = [s / nw for s in r["te_sum"]]; r["fxt_avg"] = [s / nw for s in r["tf_sum"]]
    r["wav"] = wav_stats(d)
    r["inj"] = injected(v["keys"]) if v["keys"] else None
    if wall: r["wall_s"] = round(wall)
    json.dump(r, open(os.path.join(d, "result.json"), "w"), indent=1)
    h = r["hist"]
    print(f"== {name}  ({v['ms']} ms emulated; {nw} warm blocks)")
    print(f"   block ticks: avg {avg:,.0f} ({r['avg_pct']:.1f} %)  worst {r['audio_max_w']:,} ({r['max_pct']:.1f} %)  [first-block spike, whole run: {r['audio_max']:,}]")
    print(f"   > 75 %: {r['n75']}  > 90 %: {r['n90']}  > 100 %: {r['n100']}   lost audio boundaries: {r['uf']} (first at block {r['uf_first']})   ISR spacing {r['gap_min']:,}..{r['gap_max']:,} (block = {P:,})")
    print("   histogram (5 % bins): " + " ".join(f"{5*i}:{c}" for i, c in enumerate(h[:23]) if c) + (f" 115+:{h[23]}" if h[23] else ""))
    print(f"   parts (avg ticks/block): engines " + "/".join(f"{x:,.0f}" for x in r["eng_avg"]) + f" = {sum(r['eng_avg']):,.0f};  chains " + "/".join(f"{x:,.0f}" for x in r["fxt_avg"]) + f" = {sum(r['fxt_avg']):,.0f};  mod apply+restore {r['mod_avg']:,.0f}")
    print(f"   worst per track: engine " + "/".join(f"{x:,}" for x in r["te_max"]) + "  chain " + "/".join(f"{x:,}" for x in r["tf_max"]))
    fr = r["frame_n"]
    print(f"   UI: main-loop passes {r['loop_n']}, longest gap {r['loop_gap_max']/24000:.1f} ms; frames {fr} ({r['full_n']} full-screen), avg {r['frame_sum']/max(fr,1)/24000:.1f} ms, worst {r['frame_max']/24000:.1f} ms (full {r['full_max']/24000:.1f} ms), longest gap between frames {r['frame_gap_max']/24000:.1f} ms, rows/frame {r['rows_sum']/max(fr,1):.0f}")
    if r["key_n"]: print(f"   note key -> start of the block that plays it: {r['key_n']} keys, avg {r['key_sum']/r['key_n']/24:.0f} us, worst {r['key_max']/24:.0f} us")
    if r["inj"]: print(f"   injected {r['inj']}  taken: encoder clicks {r['enc_steps']}, button presses {r['btn_n']}")
    print(f"   memory: bss_end 0x{r['bss_end']:08x}, {r['ram_free']:,} B to the 0x01C74000 guard; user stack never touched {r['ustack_free']:,} of 24,320 B (used {24320-r['ustack_free']:,}); interrupt stack never touched {r['sstack_free']:,} of 7,936 B (used {7936-r['sstack_free']:,})")
    if r["wav"]: print(f"   wav: {r['wav']}")
    if r["gov"][6] and r["wav"] and name == "lat_idle": print(f"   latency check (silent firmware, drums selected, first note): the key was taken by ISR run {r['gov'][6]} ({r['gov'][7]/24:.0f} us after the scan saw it); the first non-silent block of the wav is block {r['wav']['first_nonzero_block']} (run R fills wav block R+1 if the half-buffer chain is as described)")
    if r.get("gov"): print(f"   governor: slots bypassed {r['gov'][0]}, drum-cap steps {r['gov'][1]}, voices stolen {r['gov'][2]}, lowest cap {r['gov'][3]}, cap at the end {r['gov'][4]}")
    if r["alloc"][0]: print(f"   fx_alloc (a chain's effects changed while playing): {r['alloc'][0]} calls, worst {r['alloc'][1]:,} ticks ({100*r['alloc'][1]/P:.0f} % of a block), avg {r['alloc'][2]//r['alloc'][0]:,}")
    if r.get("trace") and any(r["trace"]):
        tr = [x * 4 / P * 100 for x in r["trace"] if x]
        print("   time series (mean % of a block per 20 blocks, from block 200): " + " ".join(f"{sum(tr[i:i+20])/len(tr[i:i+20]):.0f}" for i in range(0, len(tr), 20)) + f"   max {max(tr):.0f} %")
    print(f"   setup: routes {r['lt'][0]}, bpm {r['lt'][1]}, engines 0x{r['lt'][2]:04x}")
    return r

def table():
    rows = []
    for name in VARIANTS:
        fn = os.path.join(OUT, name, "result.json")
        if os.path.exists(fn): rows.append(json.load(open(fn)))
    print("| run | avg | worst | >75 % | >90 % | >100 % | lost | UI frame avg / worst | longest loop gap |")
    print("|---|---|---|---|---|---|---|---|---|")
    for r in rows:
        fr = max(r["frame_n"], 1)
        print(f"| {r['name']} | {r['avg_pct']:.1f} % | {r['max_pct']:.1f} % | {r['n75']} | {r['n90']} | {r['n100']} | {r['uf']} | {r['frame_sum']/fr/24000:.0f} / {r['frame_max']/24000:.0f} ms | {r['loop_gap_max']/24000:.0f} ms |")

def main():
    a = sys.argv[1:]
    if not a or a[0] == "list":
        for k, v in VARIANTS.items(): print(f"{k:14s} {v['ms']:5d} ms  {v['xcflags']}")
    elif a[0] == "run":
        todo = list(a[1:]); running = []
        while todo or running:
            while todo and len(running) < int(os.environ.get("LT_JOBS", "3")):
                n = todo.pop(0); p, t0, d = start(n); running.append((n, p, t0)); print(f"started {n}", flush=True)
            time.sleep(2)
            for x in list(running):
                if x[1].poll() is not None:
                    running.remove(x); report(x[0], time.time() - x[2]); sys.stdout.flush()
    elif a[0] == "report":
        for n in a[1:]: report(n)
    elif a[0] == "table":
        table()

if __name__ == "__main__":
    main()
