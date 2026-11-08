#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""fm1_peek: READ-ONLY look at the RAM the chip kept after a start that failed (the FM-1 that ends in the chip's update mode).
Reads the boot trace, the crash record and the boot guard of an Octavio firmware: 0x01C7C000..0x01C7C1A0.  Writes nothing, starts nothing.

  sudo ~/fm1-rescue/bin/python fm1_peek.py          (the FM-1 in update mode: switch it on and wait until it has shown up)
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fm1_rescue as r

ADDR, N = 0x01C7C000, 0x1A0

def main():
    if os.geteuid() != 0:
        print("run it with sudo"); return 1
    import usb.core, usb.util
    bot = r.open_device(usb, 300)
    try:
        u = r.Uboot(bot)
        print("device:", *u.inquiry())
        out = bytearray()
        while len(out) < N:
            k = min(r.IO, N - len(out))
            out += bot.xfer(r.cdb_for(r.CMD_READ_MEMORY, struct.pack(">IH", ADDR + len(out), k)), n_in=k)
        w = struct.unpack("<%dI" % (N // 4), bytes(out))
        print("raw words from", hex(ADDR))
        for i in range(0, len(w), 8):
            print("  %08x:" % (ADDR + 4 * i), " ".join("%08x" % x for x in w[i:i + 8]))
        b = w[0x180 // 4: 0x190 // 4]
        print()
        print("boot trace: magic %08x (4f425354 = ours)  stage %d  previous attempt's stage %d  attempts since the last good start %d" % (b[0], b[1], b[2], b[3]))
        c = w[0:15]
        print("crash record: magic %08x (43525348 = a crash was recorded)  count %d  vec %d  pc %08x  rets %08x  sp %08x  uptime %d ms  early %d" % (c[0], c[1], c[2], c[3], c[4], c[7], c[10], c[14]))
        g = w[0x190 // 4: 0x19C // 4]
        print("boot guard: magic %08x (4f435431 = ours)  pending %d  failed %d" % tuple(g))
    finally:
        try: usb.util.dispose_resources(bot.dev)
        except Exception: pass
    return 0

if __name__ == "__main__":
    sys.exit(main())
