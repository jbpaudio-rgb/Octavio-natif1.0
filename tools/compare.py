#!/usr/bin/env python3
"""compare two 240x240 screens (PNG/PPM/BMP): per-region mean abs error and the share of pixels that differ.
usage: compare.py REF OURS [--diff out.png]   (the 12-px corners and the 2-px border are ignored: they are the prototype's CSS)"""
import sys
import numpy as np
from PIL import Image
def load(p):
    return np.array(Image.open(p).convert('RGB').resize((240, 240)) if Image.open(p).size != (240, 240) else Image.open(p).convert('RGB')).astype(int)
def q565(a):  # what the panel can show
    r = (a[..., 0] >> 3) << 3; g = (a[..., 1] >> 2) << 2; b = (a[..., 2] >> 3) << 3
    return np.stack([r, g, b], -1)
def mask():
    m = np.ones((240, 240), bool)
    m[:2, :] = m[-2:, :] = False; m[:, :2] = m[:, -2:] = False
    for cy in (0, 239):
        for cx in (0, 239):
            for y in range(240):
                for x in range(240):
                    if abs(x - cx) < 13 and abs(y - cy) < 13: m[y, x] = False
    return m
def report(ref, ours, name=''):
    m = mask()
    d = np.abs(q565(ref) - q565(ours)).max(-1)
    regions = {'all': (0, 240), 'title y<30': (0, 30), 'cells 30..125': (30, 125), 'graph 125..206': (125, 206), 'below': (206, 240)}
    out = {}
    for k, (a, b) in regions.items():
        mm = m.copy(); mm[:a] = False; mm[b:] = False
        out[k] = (d[mm].mean(), (d[mm] > 24).mean() * 100)
    return out, d
if __name__ == '__main__':
    ref, ours = load(sys.argv[1]), load(sys.argv[2])
    out, d = report(ref, ours)
    for k, (mae, pc) in out.items():
        print(f'{k:16s} mean abs err {mae:6.2f}/255   pixels off by >24: {pc:5.2f} %')
    if '--diff' in sys.argv:
        Image.fromarray(np.clip(d * 4, 0, 255).astype('uint8')).resize((480, 480), Image.NEAREST).save(sys.argv[sys.argv.index('--diff') + 1])
