# octavio-native — agent E: engine settings pages

Branch `m3-E` (base `fdda9d5`). Scope: the Minimoog, Prophet and drum engines get the prototype's settings pages (title,
2 x 2 knob grid, scope graph, value text, names, ranges, page navigation), K1..K4 edit the selected track's engine through
`voices_param()`, and the pages follow the engine of the selected track.

## What is in

| file | what |
|---|---|
| `src/engine_pages.c` (new) | the prototype's `EPG` tables (titles, cell names, parameters per page and engine), the parameter descriptors (kind, step, options, value text), `ep_on()` (which page is on screen), the knob encoder (1/50 range per detent, snapped to the prototype's slider step) |
| `src/engine_graphs.c` (new) | `dengine()` / `ddrum()` of the prototype in fixed point (mixer + oscillator waveforms, filter response, loudness / contour / amp / filter envelopes, drum decay + pitch sweep) and the six-strip drum mixer |
| `src/ui.c` | hooks only: `ep_on()` in `draw_cell` / `ui_draw_page` / `ui_prepare` / `ui_encoder`, per-engine knob skin and pointer, left cells clip their text at the cell edge (116 px) like the prototype |
| `src/ctrl.c`, `ctrl.h` | pages `PGN_OSC1..4`, `PGN_ENV2`, `PGN_GLO2`; EDIT / GLO per engine; `ctrl_op_key()` (chords with the black keys); the drum voice; **ALGORITHM knob now changes the track's engine** (it only showed the list before) |
| `src/main.c` | black keys go to `ctrl_op_key()` first (white keys keep the bank behaviour) |
| `src/params.c/.h` | `param_text_ms()`: the envelope time text for engine params |
| `assets/knob_{moog,pro,drum}.json`, `tools/chrome_knobs.js`, `tools/pack_assets.py` | the three other knob skins rendered by Chrome from the prototype's own `kv()` (`ENG=1|2|3 OUT=... node tools/chrome_knobs.js`); `assets_gen.c` regenerated |
| `host/render.c` | the host harness now links voices/ctrl/overlay; new args `en N`, `dv N`, `vp id,value` |

## Navigation (as the prototype)

* ALGORITHM knob: engine of the selected track (VACT / MOOG / PROP / DRUM).
* Minimoog / Prophet: EDIT = FILTER, GLO = MIXER, ENV = LOUDNESS (AMP ENV); hold EDIT or GLO + OP1..OP4 (the first four black
  keys, F#, G#, A#, C#) = OSCILLATOR 1/2/3 (A/B/SUB) / NOISE (same key again = back to MIXER); on ENV hold ENV + OP2 =
  CONTOUR (FILTER ENV), OP1 = back.
* Drums: EDIT / ENV show the selected voice (hold EDIT or ENV + OP1..OP6 picks the voice: title `808 KICK`, `808 KICK ENV`),
  GLO = MIXER 1/2 (strips 1..4 on K1..K4), GLO again = MIXER 2/2 (strips 5, 6 on K1, K2).
* Pages the engine does not have fall back like `page()` does (OSCn on the Vactrol -> GLO, ENV2 -> ENV, drum OSCn / ENV2 -> EDIT).
* The button LED follows the prototype's `PNf()` (the Vactrol's FM page lights EDIT, GLOBAL lights GLO; before, they were swapped).

## Verification

Host render (`host/render.c`, gcc) against Playwright captures of the prototype at 240 x 240 with its clock frozen
(`performance.now = () => 123456`, noise level 0; `tools/compare.py` mask), mean abs error per 255 / % pixels off by > 24:

| page | all |
|---|---|
| Minimoog MIXER / OSC1 / FILTER / LOUDNESS / CONTOUR | 1.26 / 1.90 / 0.92 / 1.13 / 1.11 (1.1 .. 1.9 %) |
| Prophet MIXER / OSC A / OSC B / SUB / FILTER / AMP ENV / FILTER ENV | 0.99 / 1.18 / 1.20 / 0.97 / 0.89 / 1.07 / 1.16 |
| Drums EDIT (kick, open hat) / ENV / MIXER 1/2 / 2/2 | 1.49 / 1.5 / 1.5 / 1.08 / 1.05 |

The Vactrol pages are unchanged (cells 0.9 / ENV 1.7 / LFO 0.9 against a fresh capture).

Emulator (`fm1-emu`): see the screenshots in `shots/`.

## Honest limits

* The NOISE pages (and the noise part of the mixer) are not pixel-comparable: the prototype's noise is `Math.sin`-hash noise
  re-seeded 25 times a second; ours uses an integer hash with the same statistics (types, tone, crush) so it looks the same
  but not sample for sample.
* The prototype's drum **ARP** page (Swing / Human. / Chance / Accent per voice, the drum sequencer lane) is not there: the
  native drum engine has no lane parameters yet. The Vactrol ARP page is still what ARP shows.
* The track's settings are the engine's own (`voices_param`); the presets (`ctrl.c` bank) still save the Vactrol's `PV[]` only.
  Saving / loading the other engines' parameters belongs to agent D's preset work.
* Quick keyboard shortcuts of the prototype (keys 1..4 on the computer keyboard for OSCn) do not exist on the panel.
* HOME + OP key (track select) and OP8 (FX order) chords are not part of this change.
