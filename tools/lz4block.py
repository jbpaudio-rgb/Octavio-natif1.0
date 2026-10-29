# SPDX-License-Identifier: MPL-2.0
"""A small LZ4 block compressor / decompressor (the public LZ4 block format), written for this project."""


def compress(src: bytes) -> bytes:
    n = len(src)
    out = bytearray()
    table = {}
    i = anchor = 0
    limit = n - 12                      # the last match must start at least 12 bytes before the end
    while i < limit:
        key = src[i:i + 4]
        cand = table.get(key)
        table[key] = i
        if cand is not None and i - cand <= 65535:
            m = 4
            end = n - 5                 # the last 5 bytes are always literals
            while i + m < end and src[cand + m] == src[i + m]:
                m += 1
            lit = i - anchor
            tok = (min(lit, 15) << 4) | min(m - 4, 15)
            out.append(tok)
            if lit >= 15:
                r = lit - 15
                while r >= 255:
                    out.append(255); r -= 255
                out.append(r)
            out += src[anchor:i]
            out += (i - cand).to_bytes(2, "little")
            if m - 4 >= 15:
                r = m - 4 - 15
                while r >= 255:
                    out.append(255); r -= 255
                out.append(r)
            i += m
            anchor = i
        else:
            i += 1
    lit = n - anchor
    out.append(min(lit, 15) << 4)
    if lit >= 15:
        r = lit - 15
        while r >= 255:
            out.append(255); r -= 255
        out.append(r)
    out += src[anchor:]
    return bytes(out)


def decompress(src: bytes) -> bytes:
    out = bytearray()
    i = 0
    while i < len(src):
        tok = src[i]; i += 1
        lit = tok >> 4
        if lit == 15:
            while True:
                b = src[i]; i += 1; lit += b
                if b != 255:
                    break
        out += src[i:i + lit]; i += lit
        if i >= len(src):
            break
        off = src[i] | (src[i + 1] << 8); i += 2
        m = tok & 15
        if m == 15:
            while True:
                b = src[i]; i += 1; m += b
                if b != 255:
                    break
        m += 4
        for _ in range(m):
            out.append(out[-off])
    return bytes(out)


if __name__ == "__main__":
    import os
    for t in (b"", b"a", b"abc" * 100, os.urandom(500), bytes(4096), bytes(range(256)) * 16):
        assert decompress(compress(t)) == t, len(t)
    print("lz4block ok")
