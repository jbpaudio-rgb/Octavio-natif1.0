#!/usr/bin/env python3
"""Ask a running Octavio / felucca firmware to restart in the chip's update mode (UBOOT). Changes nothing in the flash."""
import sys, time
import mido
outs = [n for n in dict.fromkeys(mido.get_output_names()) if "FM-1" in n]
if not outs:
    sys.exit("FM-1 not found (USB data cable? firmware running?)")
with mido.open_output(outs[0]) as o:
    o.send(mido.Message("sysex", data=[0x22, 0x24, 0x35, 0x7D]))
    time.sleep(0.3)
print("sent: the FM-1 restarts into update mode (screen goes dark). Now run fm1_peek.")
