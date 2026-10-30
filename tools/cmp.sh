#!/bin/sh
# tools/cmp.sh NAME "render args"  : host render vs /home/claude/run-A/ref/all/NAME.png -> /home/claude/run-A/h/NAME.png + side-by-side NAME_cmp.png
H=/home/claude/run-A/h; N=$1; shift; REF=${REF:-/home/claude/run-A/ref/all}
gcc -O1 -Wall -Wno-unused-function -o $H/render /home/claude/fw/octavio-A/host/render.c 2>&1 | head -20
cd $H && ./render $N.ppm 0 "$@" && python3 -c "
from PIL import Image
a=Image.open('$REF/$N.png').convert('RGB');b=Image.open('$N.ppm').convert('RGB');b.save('$N.png')
w=Image.new('RGB',(480,240));w.paste(a,(0,0));w.paste(b,(240,0));w.resize((960,480),Image.NEAREST).save('${N}_cmp.png')" && python3 /home/claude/fw/octavio-A/tools/compare.py $REF/$N.png $N.png | tail -5
