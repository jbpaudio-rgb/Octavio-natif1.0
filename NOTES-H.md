# octavio-native-H — effects per track (branch m4-H, base m3-merge 9d1d190)

The 16 effects of F (`src/fxdsp.c`) ran once on the sum of the four tracks. They now run **per track**, as in the prototype
(`TRX[t].fxs`, `fxqT[t]`, `F(tb[i]) * t.vol`): each track has its own chain, its own effect knobs, its own delay pools;
the track volume is applied after the chain, then the four tracks are summed (`voices_render`). Nothing pushed, nothing flashed.

## What a track gets

| | |
|---|---|
| slots per track | **4** (`FX_NSLOT`; the prototype has 6, the FX ORDER page shows 4 tiles) |
| effects | all 16, any slot; an effect is in a track's chain at most once (assigning one that is already there swaps places, as the prototype) |
| knobs | every effect has its own knobs **per track** (52 knobs + the delay's sync switch); a chain can hold a different effect with different settings on each track |
| pool | 72 KB of delay-line RAM **per track** (4 x 72 KB = 288 KB, was 1 x 144 KB). A long effect shortens its buffer in proportion when the chain wants more than its pool (same rule as F, per track): e.g. Delay alone gets ~34,000 samples = 0.77 s of its 1.25 s; Delay + Granular in the same chain get ~40 % of their lengths each (Delay ~0.5 s) |
| CPU | only slots with **Mix > 0** cost anything; they are admitted by a budget (below). One that does not fit is bypassed and dimmed on the FX ORDER page, and a toast "CPU FULL / FX BYPASSED" shows |

Effects follow the **selected track** exactly as the prototype does: HOME + SELECT picks the track, the FX page edits that track's
chain (its knobs are the ones the page shows), the other tracks keep running with theirs.

### Panel gestures (as the prototype; the first two did not exist in native before)

* **FX held + a white key** (on the FX page): that effect (white key 1..16 = effect 0..15, FXD order) goes into the selected slot of the selected track.
* **FX held + OP1..OP4** (FX or FX ORDER page): select the slot (the prototype has OP1..OP6). FX pressed again on the FX page still steps through the slots.
* **OP8**: FX ORDER page. **FX held + OP8**: the delay's tempo sync (the prototype's `toggleSync("FX")`, per track).
* FX ORDER: SELECT steps through the slots; the prototype's SELECT-held + knob *reordering* is not implemented (assigning an effect already in the chain swaps places, which gives any order).

## CPU: measurements (fm1-emu, timer ticks at 24 MHz, one audio block = 69,660 ticks = 100 %)

Whole IRQ handler (`perf.audio_*`, `src/main.c`), 4 tracks sounding (Vactrol, Minimoog, Prophet, drums, retriggered every
~0.37 s), all six LFO / S&H routed (three synth destinations + reverb size on every track + phaser rate), master and tanh.
The first 200 blocks (0.6 s: allocation, start-up) are excluded from "worst"; the start block alone reaches 34 k ticks even without effects.

| scenario (bench `-DBENCH`, 2.5 s) | FX ticks | **worst block** | average |
|---|---|---|---|
| engines + mod, no effect | 0 | **25,832 (37.1 %)** | 21,591 (31.0 %) |
| 4 x {Overdrive, Phaser, Flanger, Chorus}, all Mix 1 (the default chains) | 15,613 | 41,311 (59.3 %) | 53.2 % |
| 4 x {Feedback, Bitcrush, Ring mod, Stutter} | 8,430 | 34,213 (49.1 %) | 42.9 % |
| 4 x {Reverb, Freeze, Tape, Shimmer}, no budget | 32,203 | 57,921 (83.1 %) | 77.1 % |
| 4 x Granular alone (slot 1 only), no budget | 24,889 | 50,595 (72.6 %) | 59.1 % |
| 4 x Shimmer alone / 4 x Reverb alone, no budget | 10,201 / 8,908 | 35,940 / 34,693 | |
| **with the budget** T1 {Granular, Reverb, Delay, Chorus} T2 {Shimmer, Reverse, Tape, Phaser} T3 {Overdrive, Bitcrush, Ring, Stutter} T4 {Flanger, Micro, Feedback, Freeze}, all Mix 1, knobs at max | 15,769 | **47,569 -> 40,567 after warm-up (58.2 %)** | 51.2 % |
| with the budget, 4 x {Granular, Reverb, Delay, Chorus} | 11,634 | 36,406 (52.3 %) | 43.8 % |
| with the budget, 4 chains of {Reverb, Reverse, Freeze, Tape, Shimmer} rotations (the heaviest mix) | 17,992 | **43,801 (62.9 %)** | 56.8 % |

Per effect (worst block, 4 identical tracks / 4, so one instance): Granular 6,222, Shimmer 2,550, Reverb 2,227; the chains
{OD, Phaser, Flanger, Chorus} 3,903, {FB, Crush, Ring, Stutter} 2,108, {Reverb, Freeze, Tape, Shimmer} 8,051 (F's single-effect
figures are 0-30 % higher: 7,591 granular, 2,593 reverb, 2,790 shimmer ...).

**Budget.** `FX_COST[]` (fxstate.c) holds F's single-effect worst-block figures (the conservative ones), `FX_BUDGET` = 22,000
of those units for all four chains together. Engines + mod worst case measured ~26 k ticks, so (22 k x measured/F ratio of 0.57..0.91 =
at most ~20 k real ticks) the block stays **under ~66 %** in the worst case the budget allows, and 58-63 % in the heavy
configurations measured above. The default chains (4 x 4 cheap effects) cost 19,056 units and all fit. Rule (`fx_admit`, once per
block, after the mod matrix): an active slot (Mix > 0, modulated value) that already runs keeps its place; a new one is admitted in
track 1..4 / slot 1..4 order while the sum fits, else bypassed (and dimmed on FX ORDER) until something frees up. So a 7.6 k Granular
fits next to ~14 k of other effects; two Granulars (15 k) + Reverbs do not. `-DFX_BUDGET=n` overrides it (bench).

Silent tracks cost nothing: a track whose engine is off and whose chain output has stayed under 3 LSB for 720 blocks (2.1 s, longer
than the 1.25 s delay line) is not processed; its chain still runs after the note ends, so delay / reverb tails ring out as in the prototype.

## RAM

`.data + .bss` 0x01C08000 .. 0x01C6B908 = 407,816 B (was ~256 KB; pools 4 x 72 KB = 288 KB + 4 x 960 B state + 4 x 106 B effective knobs). `app.ld` RAM region
320K -> **416K** (the stacks start at 0x01C74000 = 432K, so 34 KB stay free below them; the link fails if `.data+.bss` outgrow 416K).

## State and serialisation (for octavio-G's presets)

`src/fxstate.c` (included before voices / ctrl / ui):

```c
typedef struct { uint8_t ord[FX_NSLOT]; int16_t p[FX_NP]; } fxt_t;   /* per track */
static fxt_t FXT[4];       FX_NSLOT = 4, FX_NP = 53, FXBANK_VER = 1
```
`ord[s]` is the effect (0..15, FXD order: Overdrive Phaser Flanger Chorus MicroPitch Delay Reverb Granular Feedback Bitcrush RingMod
Stutter Reverse Freeze Tape Shimmer) of slot s; `p[k]` is the knob `PV[P_F0DRIVE + k]` in thousandths (k = 0..51 the 16 effects'
knobs in FXD order, mix last of each; k = 52 the delay's sync switch = `P_F5SYNC`).

**The selected track's knobs live in `PV[]`** (the FX pages edit PV like every other parameter, so `PV`-based presets / scenes keep
saving the selected track's FX); `FXT[t].p` holds the others. `fx_select(t)` (called by `ctrl_sync` when the track changes) swaps
them, `fx_commit()` copies PV -> `FXT[view]` (the audio does it every block). Bytes (little endian), `fxbank_save(buf)` / `fxbank_load(buf, n)`:

```
[0] version (1) [1] FX_NSLOT (4) [2] tracks (4) [3] FX_NP (53)
then for each track: ord[nslot] (u8 each), p[np] (int16 LE each)       = 4 + 4 * (4 + 106) = 444 bytes (FXBANK_BYTES)
```
`fxbank_load` returns 0 and changes nothing for another version or a short buffer; it accepts a different slot / knob count
(what fits is taken, the rest default), clamps every value to its range, repairs a repeated effect, reloads PV from the selected
track and re-runs the CPU admission. Call `fxbank_save` before writing a preset (it commits PV first), `fxbank_load` after reading
one. `ctrl.c`'s existing scenes / presets copy `PV[]` and are untouched: they hold the selected track's chain parameters only
(not the order, nor the other three tracks) until G stores `fxbank_save()` too. `tests/fxbank_test.c` (`tools/fxhost.sh test`) checks the round trip.

## Files

* `src/fxstate.c` (new): per-track state, `FX_COST` / `FX_BUDGET`, `fx_select`, `fx_assign`, `fxbank_save` / `fxbank_load`, the `perf` struct.
* `src/fxdsp.c`: the 16 effects take their state (`fxst_t`) and knobs (`FEFF[t]`) from the track; per-track pools / allocation; `fx_begin`, `fx_admit`, `fx_mod`, `fx_track`, `fx_awake`.
* `src/voices.c`: chain per track before the volume, silent-track skip; the master `* 1.05` + tanh unchanged. `ui_peak` (HOME meters) is now taken after the chain.
* `src/lfo.c`: FX destinations go through `fx_mod` (below). `src/main.c`: `fx_begin` / `fx_admit` in the IRQ, perf counters, toast, FX_TEST / BENCH hooks. `app.ld`: 416K.
* `src/ctrl.c/.h`, `ui.c`, `screens.c`, `pages.c`: `% FX_NSLOT`, FX chords, OP8, FX ORDER shows `FX_NSLOT` tiles (dimmed when bypassed), `fxs` is the selected track's order.
* `host/fxtest.c`, `tools/fxhost.sh`, `tests/fxbank_test.c`: host harness (below). `-DBENCH -DBENCH_FX={{..},{..},{..},{..}} [-DBENCH_KNOB=n -DBENCH_MIX=n -DBENCH_ACTIVE=mask]` builds the worst-case bench.

## Fidelity against the prototype's mkFX (host build of `fxdsp.c` vs node)

`host/fxtest.c`: the same 3 s test signal (two decaying saw+sine notes with a gap) through four tracks at once, each with a different
effect and knobs, against `mkFX` of the prototype (F's `ref/mkfx.js`, effect + mix only, no 1.25 / .8 scaling any more). Error rms re the
reference: Overdrive -61 dB, Phaser -59, Flanger -53, Chorus -58, Delay -45, Reverb -43, Micro pitch -26, Feedback -35, Bitcrush -36,
Ring mod -40, Reverse -39, Freeze -46, Tape -48, Shimmer -13 (near-unstable feedback, also in F), Granular and Stutter random (rms within 10 % / 1 %).
The four tracks never disturb each other (each runs a different effect in the same pass). Unit test of the state / budget / serialisation: `tools/fxhost.sh test`.

## Emulator (fm1-emu, real image `build/octavio.fwsc`)

`shots/ui_fx_per_track.png` (240 x 240 grabs at 2.3 / 2.9 / 4.2 / 5.8 / 7.3 s): FX held + white key 7 puts **REVERB** in slot 1 of track 1 (FX1 REVERB, mix raised with K4);
HOME + SELECT to track 2: its FX1 is still **OVERDRIVE** (own order, own knobs); FX held + white key 6 puts **DELAY** there, mix 46 %; OP8 shows **FX ORDER** with
the four tiles of track 2 (DELAY, PHASER, FLANGER, CHORUS). Sound (left channel, rms / peak per 0.5 s): track 1 pluck with the reverb tail ringing out after the engine stops
(0.067 / 0.15 -> 0.003 / 0.005 over 2 s), then track 2 (Minimoog) with delay 0.077-0.093 / 0.23-0.29. Second image (a build with `-DFX_BUDGET=1000`): raising the
Mix of the Reverb on a full budget -> **CPU FULL / FX BYPASSED** toast and the FX1 tile dimmed on FX ORDER, no sound from that slot.

## Deviations from the prototype (honest)

* **4 slots per track instead of 6**, 72 KB pool per track instead of 6 effects sharing the browser's memory, and the CPU budget above: the prototype runs all 6 x 4 = 24 effects at once.
* One shared mod matrix (the native one): an FX destination modulates the same knob **on every track** (relative to each track's own value). The prototype's matrix is per engine.
* The long buffers shrink when a chain shares its pool (Delay alone 0.77 s max, Delay + Granular: Delay ~0.5 s and the grains start within ~0.2 s back instead of 0.52 s; with Reverb in the chain too, less); a Delay time above what fits clamps. (F's Granular killed every grain whose start lay beyond a shortened buffer, which thinned it out; it now narrows the start spread instead and keeps the density.)
* Mix > 0 is needed for an effect to run (as in the prototype); with the budget, a slot may also be silent because the CPU is full (toast + dimmed tile).
* FX ORDER: 4 tiles (two rows), no SELECT-held reordering, OP5/OP6 do not select slots.
* Level destination (OPT_DST 11) and sequencer lanes are still not routed (F's limits).

## Not tested

* In the emulator only one track's tail and one track's note were audible at a time (keys play the selected track only); the overlap of effects on several tracks is what the bench measures (CPU, rms 0.05-0.25), not a recorded musical test.
* Not run on the real device: the tick figures are the emulator's (instruction-count based timer), not measured on silicon.
* The first audio block after boot reaches ~50 k ticks in some configurations (allocation + memset of 4 pools); a block that long is still < 100 %, but nothing measured it on hardware.
* Mix modulated across 0 by the LFO flaps the admission (a bypassed slot may toast repeatedly); not exercised.
* Presets / scenes of G: only the PV part persists until `fxbank_save` is wired in.
