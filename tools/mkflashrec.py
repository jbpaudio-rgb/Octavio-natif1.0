#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Test helper: write the src/persist.c records (presets at 0xEB000, scenes at 0xE7000) into a 1 MiB chip image, so
# fm1-emu --raw-flash can boot "after a restart". Same format as persist.c / state.c (NOTES-G.md).
#   mkflashrec.py OUT.bin            one preset "RESTORED" (track 1 = Minimoog, CORE fold 900, Minimoog cutoff of track 2 = 123)
#                                    and a scene in key slot 1 (HOME + first white key)
#   mkflashrec.py OUT.bin --v1       the first format (Vactrol only) at 0x93000 / 0xE7000, P = P_COUNT of that build
import struct, sys, zlib

MAGIC = 0x4F435442

def rec(base, flash, ver, payload):
    hdr = struct.pack('<IIII', MAGIC, ver, len(payload), zlib.crc32(payload) & 0xFFFFFFFF)
    s = hdr + payload
    for i in range(0, len(s), 0xF00):           # one 0xF00 piece per 4 KiB sector
        a = base + (i // 0xF00) * 0x1000
        flash[a:a + len(s[i:i + 0xF00])] = s[i:i + 0xF00]

def section(sid, rev, nvals, diffs):             # sparse value section: (index, value) pairs against the factory values
    body = bytes([1, nvals]) + b''.join(struct.pack('<Bh', i, v) for i, v in diffs)
    return struct.pack('<BBH', sid, rev, len(body)) + body

def trk(engines, owner):
    return struct.pack('<BBH', 0x08, 1, 5) + bytes(engines) + bytes([owner])

def entry_preset(name, etype, k, body):
    n = name.encode().ljust(13, b'\0')
    e = n + bytes([etype]) + struct.pack('<H', k) + body
    return struct.pack('<H', 2 + len(e)) + e

def entry_scene(slot, body):
    return struct.pack('<HB', 3 + len(body), slot) + body

if __name__ == '__main__':
    out = sys.argv[1]
    flash = bytearray(b'\xff' * (1 << 20))
    if '--v1' in sys.argv:
        P = 151
        name = b'RESTORED'.ljust(13, b'\0')
        pre = name + bytes([0]) + struct.pack('<H', 1) + struct.pack('<%dh' % P, *([0] * P))
        rec(0x93000, flash, len(pre), pre)
        sc = struct.pack('<H', 1) + struct.pack('<%dh' % (16 * P), *([0] * (16 * P)))
        rec(0xE7000, flash, 0x10000 | P, sc)
    else:
        # P_FOLD is PV[0] (CORE section 0x01, 14 values); the Minimoog of track 2 is section 0x10 + 3 * 1 + 0 (31 values), MG_CUT = 19
        body = trk([1, 1, 2, 3], 0) + section(0x01, 1, 14, [(0, 900)]) + section(0x13, 1, 31, [(19, 123)])
        rec(0xEB000, flash, 0x00020001, entry_preset('RESTORED', 1, 1, body))
        rec(0xE7000, flash, 0x00030001, entry_scene(1, body))
    open(out, 'wb').write(flash)
