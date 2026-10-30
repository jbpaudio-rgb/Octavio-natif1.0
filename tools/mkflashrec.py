#!/usr/bin/env python3
# Test helper: write the persist.c records (presets at 0x93000, scenes at 0xE7000) into a 1 MiB chip image,
# so fm1-emu --raw-flash can boot "after a restart". Same format as src/persist.c.
import struct, sys, zlib
P = 151
def rec(base, flash, ver, payload):
    hdr = struct.pack('<IIII', 0x4F435442, ver, len(payload), zlib.crc32(payload) & 0xFFFFFFFF)
    s = hdr + payload
    for i in range(0, len(s), 0xF00):
        a = base + (i // 0xF00) * 0x1000
        flash[a:a + len(s[i:i + 0xF00])] = s[i:i + 0xF00]
flash = bytearray(b'\xff' * (1 << 20))
name = b'RESTORED'.ljust(13, b'\0')
pre = name + bytes([0]) + struct.pack('<H', 1) + struct.pack('<%dh' % P, *([0] * P))
rec(0x93000, flash, len(pre), pre)
sc = struct.pack('<H', 1) + struct.pack('<%dh' % (16 * P), *([0] * (16 * P)))
rec(0xE7000, flash, 0x10000 | P, sc)
open(sys.argv[1], 'wb').write(flash)
