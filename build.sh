#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# octavio-native build:  JIELI_TOOLCHAIN=/path/to/jieli-linux-toolchains ./build.sh   ->  build/octavio.bin
set -e
TC="${JIELI_TOOLCHAIN:?JIELI_TOOLCHAIN must point at the JieLi Linux toolchain}"
HERE="$(cd "$(dirname "$0")" && pwd)"
BSP="$HERE/bsp"
OUT="$HERE/build"
mkdir -p "$OUT"
CC="$TC/pi32v2/bin/clang -target pi32v2"
python3 "$HERE/tools/gen_tables.py"
[ -f "$HERE/src/assets_gen.c" ] || python3 "$HERE/tools/pack_assets.py"
$CC -c "$BSP/chip/crt0.S" -o "$OUT/crt0.o"
$CC -c "$BSP/chip/fm1_vec.S" -o "$OUT/vec.o"
$CC -c "$BSP/chip/fm1_isr.S" -o "$OUT/isr.o"
$CC -Os -ffunction-sections -fno-builtin -Wall -Wno-unused-function $XCFLAGS \
    -I"$BSP/chip" -I"$BSP/boards/fm1" -I"$HERE/src" -c "$HERE/src/main.c" -o "$OUT/main.o"
"$TC/pi32v2/bin/ld" -T "$HERE/app.ld" "$OUT/crt0.o" "$OUT/vec.o" "$OUT/isr.o" "$OUT/main.o" -o "$OUT/octavio.elf" -Map "$OUT/octavio.map" 2>&1 | head -20
"$TC/common/bin/objcopy" -O binary -j .text "$OUT/octavio.elf" "$OUT/text.bin"
"$TC/common/bin/objcopy" -O binary -j .data "$OUT/octavio.elf" "$OUT/data.bin"
"$TC/common/bin/objcopy" -O binary -j .ram_text "$OUT/octavio.elf" "$OUT/rt.bin"
python3 - "$TC" "$OUT" <<'PY'
import re, subprocess, sys
tc, out = sys.argv[1:]
syms = subprocess.run([f"{tc}/common/bin/objdump", "-t", f"{out}/octavio.elf"], capture_output=True, text=True, check=True).stdout
load = int(re.search(r"^([0-9a-f]+) .*\s_data_load$", syms, re.M).group(1), 16)
rt_load = int(re.search(r"^([0-9a-f]+) .*\s_rt_load$", syms, re.M).group(1), 16)
start = int(re.search(r"^([0-9a-f]+) .*\s_start$", syms, re.M).group(1), 16)
assert start == 0x02000120, hex(start)
img = bytearray(open(f"{out}/text.bin", "rb").read())
data = open(f"{out}/data.bin", "rb").read()
rt = open(f"{out}/rt.bin", "rb").read()
for at, part in sorted([(rt_load, rt), (load, data)]):
    if part:
        assert at - 0x02000120 >= len(img), "sections overlap"
        img += b"\xff" * (at - 0x02000120 - len(img)) + part
img += b"\xff" * (-len(img) % 4)
open(f"{out}/octavio.bin", "wb").write(img)
print(f"octavio.bin: {len(img)} bytes")
PY
