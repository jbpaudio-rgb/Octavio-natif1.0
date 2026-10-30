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

## LFO page — done (cells + graph), generic cell pages
- Params: tools/gen_pext.py (from tools/proto_ctl.json = every control of the prototype) generates src/pext_*.h; 112 page parameters appended to params.h's P_* (LFO 1-4 + S&H 1-2, 16 effects x 2-4 knobs, ARP/ARP2/ARP3, tempo, MIDI channels/routing, HOME volumes/engines).
  params.c: FMT_RATE (".05*800^s Hz" / note division when synced), FMT_INT.
- src/pages.c: LFO source / FX slot state, dynamic cells (FX: n-1 params, dimmed "—" cell, Mix last), titles ("LFO 2", "S&H 1", "FX3 FLANGER"), cell text rules (ENV "LP", synced delay time).
- src/graphs.c: shared stroke helper (round-join polyline = union of discs per sub-sample column) used by ENV and LFO.
- src/lfograph.c: wave scrolling past the red dot (grey full scale, white x Amount, 4 shapes, tempo-sync rate), S&H rolling history (1/60 s steps, smoothing), "sync" box (static mask cut from the prototype capture, tools/make_masks.py).
- ui.c button map: ENV, EDIT->FM page, GLO->GLOBAL, LFO (again: next source of 6), FX (again: next slot of 6), ARP, HOME, SEQ, SEL (on FX ORDER: next slot).
- Verified (host render vs capture, tools/ref_all.js + tools/cmp.sh): LFO 1 cells 0.94, graph 9.6 (phase animated; same waveform), LFO 5 (S&H) graph 3.2; emulator shows the wave scrolling and LFO 1..4 switching.
- Not implemented: modulation routing (Dest/Amount only stored), the S&H/LFO values do not drive any engine parameter.

## ARP / ARP2 / ARP3 — done
- src/arpgraph.c: darp() port: 16 steps on the 5-line grid, step list from arpBuild() (modes Up/Down/Up-Dn/Dn-Up/Random/Played/Conv/Div, octaves, repeat, pattern), swing/gate/accent/chance, current step follows bpm * rate. `arp_set_held()` is the hook for the key handling (without held keys the prototype shows the 60-64-67 triad).
- ui.h now exports the page enum + `ui_set_page()` / `ui_get_page()` (agent C: OP keys -> PG_ARP2/PG_ARP3, HOME+EDIT -> PG_MENU/PG_MENU2, HOME+GLO -> PG_TEMPO).
- gfx.c: gfx_frect16 (sub-pixel rectangle with opacity).
- Verified: host render vs capture graph 1.28/255 (current step is clock driven), emulator snapshot shows the step moving.
