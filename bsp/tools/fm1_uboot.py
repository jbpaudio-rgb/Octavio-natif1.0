#!/usr/bin/env python3
"""Ask a running Octavio / felucca firmware to restart in the chip's update mode (UBOOT). Changes nothing in the flash."""
import sys, time
import mido
names = list(dict.fromkeys(mido.get_output_names()))
outs = [n for n in names if any(k in n.lower() for k in ("fm-1", "fm1", "felucca", "octavio"))]
if not outs:
    sys.exit("FM-1 not found. MIDI outputs seen: %s\n(cable USB de donnees ? Octavio affiche ?)" % names)
with mido.open_output(outs[0]) as o:
    o.send(mido.Message("sysex", data=[0x22, 0x24, 0x35, 0x7D]))
    time.sleep(0.3)
print("sent: the FM-1 restarts into update mode (screen goes dark). Now run fm1_peek.")
