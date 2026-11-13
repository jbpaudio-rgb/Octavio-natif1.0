#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""octavio.bin -> octavio.fwsc: builds the BSP's update loader, wraps it (LZ4 blocks of 4096 B) and calls the BSP's
fm1pkg.py. usage: JIELI_TOOLCHAIN=... AC79_SDK=... mkpkg.py [--bin build/octavio.bin] [--out build/octavio.fwsc]"""
import argparse, os, struct, subprocess, sys
from pathlib import Path
HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
import lz4block

LOADER_LOAD = 0x01C0A800
LOADER_NAME = b"usb_hid_ota.bin"          # the file name the SPL looks for
PRODUCT = "FM-1_2000"                      # == FELUCCA_ID in src/main.c


def crc16(d, c=0):
    for b in d:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) if c & 0x8000 else c << 1
        c &= 0xFFFF
    return c


def head(dcrc, a, b, attr, name):
    body = struct.pack("<HIIBBH16s", dcrc, a, b, attr, 0, 0, name)
    return struct.pack("<H", crc16(body)) + body


def wrap(image):
    blocks = bytearray()
    for i in range(0, len(image), 4096):
        raw = image[i:i + 4096]
        c = lz4block.compress(raw)
        assert len(c) <= 4096 and lz4block.decompress(c) == raw, "loader block does not compress below 4096 B"
        blocks += struct.pack("<II", len(c), len(raw)) + c
    inner = head(crc16(image), len(image), LOADER_LOAD, 0, LOADER_NAME)
    body = inner + bytes(blocks)
    return head(crc16(body), 0x20, len(body), 0x41, LOADER_NAME) + body


def run(*a):
    subprocess.run([str(x) for x in a], check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=str(ROOT / "build/octavio.bin"))
    ap.add_argument("--out", default=str(ROOT / "build/octavio.fwsc"))
    ap.add_argument("--product", default=PRODUCT)                  # must equal the image's FELUCCA_ID
    a = ap.parse_args()
    tc = Path(os.environ["JIELI_TOOLCHAIN"])
    bsp = ROOT / "bsp"
    ldr = ROOT / "build/loader"
    ldr.mkdir(parents=True, exist_ok=True)
    cc = [tc / "pi32v2/bin/clang", "-target", "pi32v2"]
    run(*cc, "-c", bsp / "loader/crt0_ldr.S", "-o", ldr / "crt0_ldr.o")
    run(*cc, "-Os", "-ffunction-sections", "-fno-builtin", "-Wall", "-Wno-unused-function", f"-I{bsp}/chip", f"-I{bsp}/boards/fm1",
        "-c", bsp / "loader/loader.c", "-o", ldr / "loader.o")
    run(tc / "pi32v2/bin/ld", "--gc-sections", "-e", "_start", "-T", bsp / "loader/loader.ld", ldr / "crt0_ldr.o", ldr / "loader.o",
        "-o", ldr / "loader.elf")
    run(tc / "common/bin/objcopy", "-O", "binary", "-j", ".text", ldr / "loader.elf", ldr / "loader.bin")
    image = (ldr / "loader.bin").read_bytes()
    assert 0 < len(image) <= 0x14000
    (ldr / "ota.bin").write_bytes(wrap(image))
    print(f"loader: {len(image)} B -> ota.bin {(ldr / 'ota.bin').stat().st_size} B")
    run(sys.executable, bsp / "tools/fm1pkg.py", a.bin, ldr / "ota.bin", a.out, "--product", a.product)


main()
