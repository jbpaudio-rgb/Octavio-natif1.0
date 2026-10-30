#!/bin/bash
# usage: emu.sh name fwsc ms snaps keys [wav]
name=$1; fw=$2; ms=$3; snaps=$4; keys=$5; wav=$6
d=/tmp/L/run/$name; rm -rf $d; mkdir -p $d; cd $d; mkdir -p work
args=(--steps 4000000000 --fwsc $fw --ms $ms --snap $snaps --keys "$keys")
[ -n "$wav" ] && args+=(--wav $d/out.wav)
timeout 590 /home/claude/fw/fm1-emu/target/release/fm1-emu "${args[@]}" > $d/log.txt 2>&1
python3 - <<P
import glob,os
from PIL import Image
for f in glob.glob('$d/work/lcd_*.bmp'):
    Image.open(f).convert('RGB').save(f[:-4]+'.png')
P
touch $d/DONE
