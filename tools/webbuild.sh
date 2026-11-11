#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The whole firmware as one WebAssembly module (host/fwweb.c) and a single-file page around it (host/web/page.tpl.html).
#   tools/webbuild.sh [OUT_DIR]   ->  OUT_DIR/fw.wasm and OUT_DIR/octavio-panel.html   (needs clang with the wasm32 target; no wasm-ld flags beyond the ones below)
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$HERE/build/web}"
mkdir -p "$OUT"
[ -f "$HERE/src/assets_gen.c" ] || python3 "$HERE/tools/pack_assets.py"
python3 "$HERE/tools/gen_tables.py"
clang --target=wasm32 -O2 -ffreestanding -fno-builtin -nostdlib -Wno-everything -I"$HERE/src" -I"$HERE/bsp/chip" \
  -Wl,--no-entry -Wl,--export-dynamic -Wl,--initial-memory=8388608 -Wl,-z,stack-size=262144 \
  -o "$OUT/fw.wasm" "$HERE/host/fwweb.c"
python3 - "$OUT" "$HERE" <<'PY'
import base64, sys
out, here = sys.argv[1:3]
w = base64.b64encode(open(out + '/fw.wasm', 'rb').read()).decode()
open(out + '/octavio-panel.html', 'w').write(open(here + '/host/web/page.tpl.html').read().replace('__WASM__', w))
PY
ls -l "$OUT"
