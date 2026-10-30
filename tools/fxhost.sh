#!/bin/sh
# tools/fxhost.sh OUT        : builds host/fxtest.c with gcc (the per-track chain code of src/fxdsp.c on the PC), OUT = the executable
# tools/fxhost.sh test       : builds and runs tests/fxbank_test.c (per-track FX state, budget, serialisation)
H="$(cd "$(dirname "$0")/.." && pwd)"; T="$(mktemp -d)"
sed -n '/^static const char \*const FX_PL/,/^static const uint8_t FX_FIRST/p' "$H/src/pages.c" > "$T/fxtab.h"
sed -n '/^static const uint8_t FX_FIRST/,/P_F15DECAY };/p' "$H/src/pages.c" | tail -n +2 >> "$T/fxtab.h"
if [ "$1" = test ]; then
    gcc -O1 -Wall -Wno-unused-function -I"$T" -I"$H/src" -o "$T/fxbank_test" "$H/tests/fxbank_test.c" -lm && "$T/fxbank_test"
else
    gcc -O2 -Wall -Wno-unused-function -I"$T" -I"$H/src" -o "$1" "$H/host/fxtest.c" -lm
fi
