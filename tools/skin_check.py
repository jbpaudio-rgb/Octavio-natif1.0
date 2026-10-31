#!/usr/bin/env python3
# Regression check for "knobs change skin / go black after SEQ": for each track (Vactrol, Minimoog, Prophet) visit ENV EDIT GLO LFO FX, press SEQ, visit them again
# and compare the knob area (rows 0..122) of every page before / after.  usage: tools/skin_check.py [octavio.fwsc]
import os, subprocess, sys, tempfile
from PIL import Image
fw = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build/octavio.fwsc")
EMU = "/home/claude/fw/fm1-emu/target/release/fm1-emu"
PG = ["ENV", "EDIT", "GLO", "LFO", "FX"]
bad = 0
for trk in range(3):
    keys, snaps, t = ["300-350:HOME"], [], 500
    for i in range(trk): keys.append("%d:enc0:+1" % (t + i * 30))
    t += 400
    order = PG + ["SEQ"] + PG
    for p in order:
        keys.append("%d-%d:%s" % (t, t + 50, p))
        if p != "SEQ": snaps.append(t + 400)
        t += 800
    d = tempfile.mkdtemp()
    subprocess.run([EMU, "--fwsc", fw, "--ms", str(t + 200), "--snap", ",".join(map(str, snaps)), "--keys", ",".join(keys)], cwd=d, capture_output=True, check=True) if os.makedirs(d + "/work", exist_ok=True) is None else None
    im = [Image.open("%s/work/lcd_%d.bmp" % (d, s)).convert("RGB") for s in snaps]
    for k, p in enumerate(PG):
        a, b = im[k].crop((0, 0, 240, 122)), im[k + 5].crop((0, 0, 240, 122))
        diff = sum(1 for x, y in zip(a.getdata(), b.getdata()) if x != y)
        print("track %d page %-4s knob area differs in %d px %s" % (trk + 1, p, diff, "" if diff == 0 else "<-- DIFFERENT"))
        bad += diff != 0
print("skin_check: %s" % ("OK" if not bad else "%d pages differ" % bad)); sys.exit(1 if bad else 0)
