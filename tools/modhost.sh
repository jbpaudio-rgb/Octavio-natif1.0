#!/bin/sh
# tools/modhost.sh OUT : builds host/modtest.c (the audio path of the firmware on the PC: engines + modulation matrix + effects, driven by a script; NOTES-K)
H="$(cd "$(dirname "$0")/.." && pwd)"
python3 "$H/tools/gen_modnames.py" "$H/host/modnames.h"
gcc -O1 -w -I"$H/src" -I"$H/bsp/chip" -I"$H/host" -o "$1" "$H/host/modtest.c" -lm
