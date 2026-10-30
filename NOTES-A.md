# NOTES-A (milestone 2, agent A: pages)

## ENV page (AD ENV) — done
- src/envgraph.c (included from ui.c): fixed-point (int64 Q30, no 64-bit divide on target) port of envCurves()/denv(): AD envelope + vactrol response at 8 kHz,
  recomputed only when ATK/EDEC/DECAY change; strokes = union of discs along the polyline; fill, #333 axis, dashed attack marker, red dot (loops T+0.6 s,
  `ui_env_retrigger()` restarts it), "T s" label in proportional Helvetica (F_GNUM, assets/gnum.json from tools/chrome_gnum.js).
- params.c: VACT (P_DECAY) now printed with fmt() like the prototype ("356 ms").
- Verified: host render and fm1-emu snapshot vs Playwright capture (tools/ref_page.js): graph 3.0/255, cells 1.7, title 1.4 (dot animated, excluded).
  Label shows 0.66 s vs 0.67 s: prototype's initial edec is exactly .3 s, ours is the 640/1000 detent = .29985 s (tie rounding).
- Cost: a recompute is ~70k int64 steps at worst (long envelopes); not measured on hardware.
## Not done yet: LFO, EDIT/GLO already M1, FX, ARP, HOME, SEL, TEMPO pages.
