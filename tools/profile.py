#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Where the instructions go (NOTES-M): python3 tools/profile.py <loadtest variant> [blocks=3] [from_ms=1300]
Runs fm1-emu on the variant's firmware (built by tools/loadtest.py; LT_OUT as there) with FM1_TRACE_RANGE over `blocks` audio blocks
after from_ms, maps every traced pc to the function that holds it (the objdump symbol table saved by the build) and prints
instructions per audio block, per function and grouped (ISR / main loop).  1 instruction = 1 cycle on the emulated core; a multiply
or a memory access can take more on silicon, so this is a ranking, not a time."""
import os, re, subprocess, sys, bisect, collections
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.environ.get("LT_OUT", os.path.join(HERE, "build", "loadtest"))
EMU = "/home/claude/fw/fm1-emu/target/release/fm1-emu"
name = sys.argv[1]; nb = int(sys.argv[2]) if len(sys.argv) > 2 else 3; ms0 = int(sys.argv[3]) if len(sys.argv) > 3 else 1300
d = os.path.abspath(os.path.join(OUT, name))
syms = []
for l in open(os.path.join(d, "symtab.txt")):
    m = re.match(r"^([0-9a-f]{8}) .*?\s(\.text\S*)\s+([0-9a-f]{8}) (\S+)$", l.rstrip())
    if m: syms.append((int(m.group(1), 16), int(m.group(3), 16), m.group(4)))
syms.sort()
starts = [s[0] for s in syms]
def fn(pc):
    i = bisect.bisect_right(starts, pc) - 1
    return syms[i][2] if i >= 0 else "?"
a0 = ms0 * 240000; a1 = a0 + nb * 700000
env = dict(os.environ, FM1_TRACE_RANGE=f"{a0}:{a1}")
p = subprocess.Popen([EMU, "--fwsc", os.path.join(d, "fw.fwsc"), "--ms", str(ms0 + 40), "--steps", str(a1 + 1000)], cwd=os.path.join(d, "work"), env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
tot = collections.Counter(); isr = collections.Counter(); n = nisr = 0
off = offmax = offpc = 0; offfn = ''; run_isr = isrmax = 0
for l in p.stdout:
    if not l.startswith("T "): continue
    f = l.split()
    pc = int(f[3], 16); irq = f[5] if len(f) > 5 else "irq=[]"
    name_ = fn(pc); n += 1; tot[name_] += 1
    ie = len(f) > 6 and f[6] == "ie=true"
    if not ie:
        off += 1
        if off > offmax: offmax = off; offfn = name_
    else: off = 0
    if irq != "irq=[]": run_isr += 1; isrmax = max(isrmax, run_isr)
    else: run_isr = 0
    if irq != "irq=[]": nisr += 1; isr[name_] += 1
print(f"{name}: {n} instructions traced over ~{nb} blocks ({n // nb} per block, block = ~700k cycles); in an ISR {nisr} ({100 * nisr // max(n, 1)} %)")
print("ISR (audio) functions, instructions per block:")
for k, v in isr.most_common(40): print(f"  {v // nb:8d}  {100 * v / max(nisr, 1):5.1f} %  {k}")
print("outside the ISR (main loop):")
for k, v in (tot - isr).most_common(8): print(f"  {v // nb:8d}  {k}")
print(f"longest run with interrupts disabled (ie=false): {offmax} instructions (= {offmax / 240:.0f} us at 240 MHz), ending in {offfn}")
print(f"longest contiguous stretch inside any interrupt handler (audio ISR + the 10 kHz scan nested): {isrmax} instructions ({100 * isrmax / 700000:.0f} % of a block)")
