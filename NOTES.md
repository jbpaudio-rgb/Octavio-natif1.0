# octavio-native — milestone 1

New FM-1 firmware written from scratch (only the felucca-bsp hardware layer in bsp/ is reused, MPL-2.0).
Target: AC791N / pi32v2, fixed point, no float.

## Done
- Boots in the fm1-emu emulator from build/octavio.fwsc (identity FM-1_2000, 611 KB package, 142 KB code+assets).
- Main screen (GLO page, title "FM", 2x2 knob grid, scope graph) rendered by an 8-row band renderer.
- Vactrol engine (fixed point) playing on the keys; knobs (1/50 detents), EDIT / GLO / ENV page buttons, octave shift.
## Missing
- ENV envelope graph, LFO/FX/ARP pages, presets, SELECT/ALGORITHM, button latch/lock and chords, KIT/DRUM, other engines, sequencer, key LEDs scenes.
## Verified
- Host render vs Playwright capture of the prototype: mean abs err 0.88/255 (title 0.80, cells 0.91, below graph 0.32).
- Emulator screenshot (key held, graph phase differs from the reference): title 0.80, cells 0.91, below 0.32 — identical to the host render. Graph region differs only by scope phase (animated).
- Emulator audio: key n7 pitch 260.9 Hz (expected 261.6), exponential decay of the AD envelope.
## Unverified
- Not run on real hardware. Audio level/envelope shape only checked roughly against the float model (tools/vactrol_ref.py); no sample-exact comparison.
- Chrome text rendering reproduced via pre-rendered DejaVu Sans Mono 4-phase atlases; RGB565 quantization; RATIO knob steps may differ by ±0.01 (float noise in prototype).
