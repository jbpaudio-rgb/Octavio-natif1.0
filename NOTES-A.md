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

## FX pages (src/fxgraph.c)
16 effect graphs (Overdrive, Phaser, Flanger, Chorus, Micro pitch, Delay, Reverb, Granular, Feedback, Bitcrush, Ring mod, Stutter, Reverse, Freeze, Tape, Shimmer), drawn per band, animated from fx_ms. FX button cycles slot 1-6; Delay shows the sync box.
Host-vs-prototype graph error mostly 0.1-3 /255; higher: fx2b ~9.8, fx7b 11.8, fx10b 5.0, fx15b 8.1 (thin outline differences only).
Known difference: Stutter (11) and Reverb random bars (6b) use an integer hash, not the prototype's sin*43758.5 fraction, so the pseudo-random pattern differs.
Emulator snapshot of FX1 Overdrive verified.

## TEMPO, MENU, HOME, FXO, SEQ (src/screens.c)
Pages that are not a "scope graph": all drawn from the prototype's measured layout (tools: /home/claude/run-A/ref/dumprect.js dumps every element's rect/font; boxes are pixel-snapped like Chrome).
- TEMPO (`PG_TEMPO`, chord HOME+GLO): canvas page, big BPM (Courier New bold 56 px canvas font F_TBIG), "BPM", four beat boxes (the active one fades 1-.7f with the clock: `tempo_beat()` from `ui_tick` ms and P_BPM).
- MENU (`PG_MENU`, HOME+EDIT; MENU2 = same chord again, cells only): 16 channel boxes, assigned tracks listed ("T1,3") from P_CHS0..3.
- HOME (`PG_HOME`): 4 strips (engine name from P_TYP0-3, fader cap from P_VOL0-3, 14-LED peak wells, red frame on the selected track). API: `ui_set_track(t)`, `ui_peak(t, q15)` (call per audio block; decays .9 per 11.6 ms like the prototype). Knobs 1-4 = the four volumes. The peak hook is NOT yet called from main.c (wells stay dark until someone calls ui_peak).
- FXO (`PG_FXO`, entered with the black key 8 in the prototype): six tiles (FX1..6) with the grid pattern; the selected tile (`fxo`) is red, lifted by 2.88 px with a bigger shadow and 4 corner dots. SEL cycles `fxo`. The 0.22 s "hit" bounce animation is not drawn.
- SEQ (`PG_SEQ`): "PITCH (MAJOR)"-style title (destination of the selected line), 16 steps (bar up = cream, down = red, gate dash below, selection frame, playing-step outline), three knobs KEY / STEP VALUE / AMOUNT (a bigger Vactrol knob sprite, KNOB_BIG). State: 6 lines x 16 steps (thousandths), gate mask; API `ui_seq_select(line, step)`, `ui_seq_set`, `ui_seq_gate_toggle`, `ui_seq_cur`. Encoders: knob1 = destination, 2 = key, 3 = step value (written into the step), 4 = amount. The sequencer does not play (no audio side).
- New params (tools/gen_pext.py): P_M4T..P_M9G (line destination (72 full names), key, amount, glide) and P_STEPV (FMT_STEP: "+25").
- New fonts (assets/fonts.json via tools/chrome_fonts.js, fonts2.json via tools/chrome_fonts2.js for canvas texts): hen, mbi, mbb, stt, tbig, tsm, mch, sql, sqv; image grew 167 KB -> 289 KB (XIP window is 581 KB).
- Test builds can start on a page: `XCFLAGS=-DBOOT_PAGE=PG_SEQ ./build.sh`. Host: `tools/cmp.sh seq pg 12`, `pg N` / `tm ms` arguments added to host/render.c.
Errors vs the capture (host render, mean abs /255): TEMPO cells 1.5 / graph area 4.5 (beat box differs: clock), MENU 0.8, HOME 2.2 (title band incl. name plates; rest 0.2), FXO 1.1-1.7, SEQ 0.9-3.5 (the 12th full-height bar is 1 px wider than the reference, rounded corners approximated by coverage constants).
Known differences: the reference captures are RGB888 while the firmware screen is RGB565 (colours differ by up to 4 levels).
