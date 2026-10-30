# NOTES-C (agent C, branch milestone2-C)

Scope: keyboard / voices / key LEDs / the prototype's other engines.

## Done
- `src/voices.c/.h`: 4 tracks, one monophonic engine per track (prototype semantics: kOnT/kOffT, down-set, note-off only when
  no key is held), key routing (`voices_keys`) with the note/track remembered per key (octave or track change while held is safe),
  per-track arpeggiator (all 9 modes, octaves, rate, gate, swing, latch, accent, chance, transpose, repeat, humanise, 6 patterns),
  mix `tanh(sum(track * .8) * 1.5 * .7)`. MONO/POLY: the prototype gives them no function (engines are monophonic, keys drive the
  selected track), so nothing is invented; chords = keys held with the arpeggiator on.
- `src/eng_moog.c` + `src/cdsp.c`: Minimoog in fixed point (3 polyBLEP oscillators, noise white/pink/brown/blue/crackle with
  tone+crush, 4-pole tanh ladder x2 sub-steps, key tracking, contour + amp ADSR, glide, release=decay).
- `src/leds.c`: LED levels (off/dim/breath/on); keys lit while pressed, notes held by the arp glow, the playing arp step breathes,
  BTN_ARP glows while the selected track arpeggiates.
- `tools/gen_c_tables.py` -> `src/c_tables_gen.h` (tanh, 1-exp(-x) tables, log2 constants); output committed.

## Verified (fm1-emu, wav + numpy)
- Moog on a key: pitch (osc1 8' at note 53 = 174.6 Hz, osc3 16' = 87.1 Hz, i.e. tune law ok), gate/release shape.
- Arp up, 2 octaves, 1/16 at 120 bpm with keys 53/57/60 held: notes 53,57,60,65,69,72 repeating, 125 ms steps.
- ISR load with the Moog playing: ~10 % of the CPU in the emulator.

## Not verified / missing
- LEDs: logic only, not observable in the emulator.
- No UI yet selects track / engine / arp / Moog parameters (API only: `voices_select`, `voices_set_engine`, `voices_param`, `voices_arp`);
  `C_BOOT_TRACK` / `C_BOOT_ARP` compile-time defines in voices.c were used to test.
- The Vactrol is a singleton: only one track can play it.
- Sequencer-driven LED scenes (step grids) not ported: they depend on pages/sequencer owned by A/B.

## Shared files modified
- `src/main.c` (includes, `voices_init`, IRQ key loop -> `voices_keys` + `voices_render`, LED block -> `leds_update`).
- `.gitignore` added and `build/` untracked.
