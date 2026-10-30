# octavio-native-J: worst-case load test (branch m6-J)

First test of the whole machine at once: four tracks playing together (engines, arpeggiator and step clock running), four effect chains full of
heavy effects, six modulation routes per track, the front panel being driven while the screen redraws. Nothing pushed, nothing flashed.
Method, results, what was changed because of them, and what is **not** known.

**Read this first: every tick figure below is the emulator's** (fm1-emu, instruction-count model, 1 instruction ~ 1 cycle at 240 MHz, TIMER4
at 24 MHz). Nothing here was measured on silicon. A real pi32v2 running from XIP flash through its cache will not run at 1 cycle per
instruction; the true load is the emulated load times an unknown factor >= 1 (1.3x would put the engines alone at ~75 % of a block).
That is why the protection added below (the governor) is driven by the **device's own timer**, not by a table of emulator numbers.

## 1. Method

* **Block**: 128 frames per audio half-buffer. 69,660 ticks at 44.1 kHz (the emulator's ALNK model uses 128 x 544 = 69,632; the code uses the latter for
  its own statistics, `PERF_P`, and the former for the governor, `GOV_P`; the difference is 0.04 %). 100 % = one block. Same unit as NOTES-H.
* **What is timed**: `perf.audio_last` = `fm1_ticks()` at the end of the audio ISR minus at its entry (everything the ISR does: key edges, `mod_apply`, `fx_admit`,
  `voices_render`, master/tanh, **and** the TIMER5 key-scan ISR that preempts it). The first 200 blocks (0.6 s: pool allocation, start-up) are left out of every statistic, as in NOTES-H.
* **Instrumentation added** (`src/fxstate.c` `perf`, hooks in `main.c`, `voices.c`, `fxdsp.c`, `lfo.c`; cheap, always on, `-DNO_PERF` removes it all; with the governor +436 B of .bss and ~+2 KB of flash, the governor alone is 56 B of .bss):
  histogram of block lengths in 5 % bins; blocks over 75 / 90 / 100 %; **lost audio boundaries** (ISR entry spacing >= 2 blocks: when the ISR overruns, the
  half-buffer's pending flag is lost and the codec replays the old half; counts one per lost block); ISR entry spacing min/max; per-track engine ticks and
  per-track chain ticks (average and worst); mod matrix ticks (**H's `mod_max` was wrong**: it timed `mod_apply` -> `mod_restore`, which includes `voices_render`;
  now apply + restore only); main-loop pass count and longest gap between two passes; `lcd_frame` count, full-screen count, average/worst duration, longest gap
  between frames; note-key -> start of the block that plays it (`fm1_in.note_t0`, set by the 10 kHz scan at the first closed sample); encoder clicks and button
  presses taken by the main loop; both stacks painted at boot and scanned every ~1000 passes; `.bss` end and the room to the 0x01C74000 guard; `fx_alloc` calls
  and cost; governor counters. The whole struct is read back with the emulator's `--dump`.
* **Underrun detection, two ways**: the firmware counter above, and the emulator's WAV: a half-buffer that was never refilled is played twice, so block k == block k-2
  and not silent. They agree within the start-up block or two (e.g. 15 vs 13, 10 vs 8, 0 vs 0). The emulator has no underrun counter of its own for ALNK.
* **Scenario** (`src/loadtest.c`, built with `-DLOADTEST`, never in the shipped firmware; `tools/loadtest.py` builds, runs and reports every variant):
  * tracks: Vactrol, Minimoog, Prophet, drums (or 4 x Moog / Prophet / drums, the Vactrol is a singleton). Synth tracks: arpeggiator on (latched 8 notes, 4 octaves, up-down,
    rate index 5, gate legato) **and** the step clock running (every synth step gate on); drum tracks: all six lanes full on 16 steps, decays long (all six voices sounding
    permanently), 180 bpm. Engines are monophonic, so "max polyphony" = drums with six voices per track.
  * chains (all Mix 1000, knobs 600): T1 {Granular, Reverb, Shimmer, Freeze}, T2 {Reverb, Shimmer, Tape, Delay}, T3 {Granular, Tape, Reverse, Delay}, T4 {Reverb, Shimmer, Freeze, Tape}.
  * modulation: the six sources (LFO 1..4, S&H 1..2) of every track routed, fast and deep: 2 engine parameters + the knobs of the chain's effects (drums: 6 drum parameters; the
    prototype gives drums no FX destinations); 24 routes.
  * front panel (`UI` script in `tools/loadtest.py`, 8 s): every page visited, every knob turned (193 encoder clicks), 14 button presses, three held keys, HOME + SELECT changes
    track, the master pot moves, FX held + white keys reassign effects (30 ms and 400 ms after FX went down), full-screen redraws on every page change.
* **Reproduce** (3 emulators at most; the machine this ran on is shared, so runs took 3-10 minutes; `LT_TIMEOUT` / `LT_JOBS` override the 590 s / 3 defaults):

```
export JIELI_TOOLCHAIN=/home/claude/jieli/jieli-linux-toolchains-20260730.1 AC79_SDK=/home/claude/sdk
python3 tools/pack_assets.py
python3 tools/loadtest.py list
python3 tools/loadtest.py run gov_full_ui gov_drum4 full_ui      # builds each variant (XCFLAGS), runs fm1-emu, prints the report; results in build/loadtest/NAME/
python3 tools/loadtest.py table
```
  Each variant is `build.sh` with `XCFLAGS="-DLOADTEST ..."`, `mkpkg.py`, then `fm1-emu --fwsc fw.fwsc --ms N --wav out.wav --dump <perf> 103 --keys '...' --snap ...` with `work/` in the cwd.
  Variants named without `gov_` are built with `-DNO_GOV` (the firmware as it was before this branch): they show what the machine costs. `gov_*` is the shipped firmware.
  The emulator's own accounting agrees: `cpu0: 75.2 % in an ISR, 2.5 % nested` for the raw full run (186.5 M ISR instructions per emulated second = 77.7 % of 240 MHz) against 80.6 % measured in ticks
  (the difference is multi-cycle instructions, which the tick count includes), idle 6.0 % against 4.8 % + the key scan.

## 2. Results

### (a) CPU of the audio block, ticks of the emulator, 100 % = 69,632

What each part costs (average per block, warm):

| part | ticks | % of a block |
|---|---|---|
| Vactrol (always rendering, even silent) | 2,070 | 3.0 |
| Minimoog (sounding) | 7,620 | 10.9 |
| Prophet (sounding) | 4,920 | 7.1 |
| **Drums, 6 voices sounding** | **25,300** | **36.3** (4.2 k per voice) |
| mod matrix apply + restore, 24 routes | 650 | 0.9 (+1,080 more inside the engines: modulated parameters are re-derived every block) |
| chains, all 16 slots running (T1..T4) | 10,750 / 7,230 / 8,300 / 8,550 = 34,800 | 50 |
| single-track chains admitted by the NOTES-H budget (T1 + T2) | 10,700 + 5,450 = 16,150 | 23 |
| `fx_alloc` of one track (chain order changed while playing) | 3,400 | 4.9 (4 x at boot: 13.7 k) |

Chain ticks per FX_COST unit (NOTES-H's units): 0.67 .. 0.90 over the four tracks.

Raw machine (governor off, `-DNO_GOV`; 3.8 s runs, ~985 warm blocks; "lost" = lost audio boundaries, the first one or two are the start-up block):

| scenario | average | worst block | blocks > 75 % / > 100 % | lost |
|---|---|---|---|---|
| idle (no loadtest, front panel only) | 4.8 % | 9.7 % | 0 / 0 | 0 |
| engines V+M+P+D, arp + step clock, no chain, no mod | 58.7 % | 59.4 % | 0 / 0 | 1 |
| + 24 mod routes | 60.2 % | 61.1 % | 0 / 0 | 1 |
| + the four full chains, static budget of NOTES-H (T1 and T2 run, T3 and T4 are bypassed) | **83.3 %** | **87.0 %** | 985 of 985 / 0 | 1 |
| + the four full chains, budget off (all 16 slots) | 110.2 % | 114.4 % | 393 of 393 / 393 | **592** |
| 4 x Minimoog + the budgeted chains | 69.6 % | 73.0 % | 0 / 0 | 1 |
| 4 x Prophet + the budgeted chains | 54.1 % | 57.6 % | 0 / 0 | 0 |
| 4 x drums (24 voices) + the budgeted chains | **183.0 %** | 185.9 % | 392 / 392 | **592** |
| the budgeted worst case with the front panel driven (8 s) | 80.6 % | 87.0 % | 2433 of 2433 / 0 | 1 |

So the NOTES-H budget (22,000 units for the chains) was sized on engines + mod ~ 26 k ticks. One drum track with all six voices sounding alone costs 25 k. **The measured worst legal
case is above the 70-75 % target (83 % average, 87 % worst), and two or more busy drum tracks, or any chain set the budget lets through next to them, overrun the block**
(a drum track with six voices = 36 %; four = 145 % before any effect). No amount of FX budget can fix that: the engines are the load.

### (b) Underruns

Raw, 4 drum tracks: 592 lost boundaries in 3.8 s (every other block: the codec replays the previous half); raw, budget off: 592 as well (not recorded to WAV, the counter is the evidence; the governed runs have both). The 87 %
case does not underrun, it just has no margin: any factor > 1.14 on silicon would. After the governor (below): 1 (the start-up block) for every scenario except 4 drum tracks (15, see below).

### (c) UI under load (same 8 s panel script, UI keeps working, but slower)

| | idle | raw worst case (80.6 %) | governed worst case (68 %) |
|---|---|---|---|
| `lcd_frame` | 344 frames / 8 s (43 per s), avg 10.1 ms, worst 29 ms | 71 frames (9 per s), avg 91 ms, worst 276 ms | 168 frames (21 per s), avg 37 ms, worst 145 ms |
| longest gap between two main-loop passes | 29 ms | 281 ms | 221 ms |
| encoder clicks taken / injected | 193 / 193 | 193 / 193 | 193 / 193 |
| button presses taken / injected | 14 / 14 | 14 / 14 | 14 / 14 |
| note keys: first closed scan -> start of the block that plays it | avg 2.8, worst 3.7 ms | avg 2.6, worst 4.0 ms | avg 2.6, worst 3.7 ms |

The main loop gets whatever the audio ISR leaves (a full 240-row redraw is ~27 ms of CPU when idle, ~10 x that at 85 %). Nothing is lost: buttons and encoders are latched by the 10 kHz scan, the watchdog
(8 s) is fed every pass (gap <= 0.3 s). The screen is ~3.6x slower (average frame) at the governed ceiling, ~9x at the raw worst case. **One real defect found and fixed**: chords
(FX held + a white key, ENV/EDIT/GLO/ARP + OP key, HOME + bank key) were decided in the audio ISR from the held-button state that only the **main loop** updates; under load the loop
is up to 0.2-0.3 s behind, so a key pressed within that time of the button went to the engine as a note instead of the chord (the 30 ms chord failed at 68 % load, worked idle).
`ctrl_hw_held()` now gives the chord handlers the buttons physically down at the moment the key arrives (`fm1_in.buttons`); with it both chords take under the governed worst case
(`fx_alloc` 2 calls, as idle). The main loop's own logic (latching, page changes on release, HOME+EDIT chords) is untouched.

### (d) Memory

All static: no heap, no malloc. `.data` + `.bss` = 0x01C08000 .. **0x01C7082C** = 428,076 B. Room: **14,292 B** to the 0x01C74000 stack guard, but only **2,004 B** to the linker's own limit
(`app.ld` RAM = 420K = 0x01C71000: the link would fail first; the limit can go to 432K without touching the guard, not done here). The pools are 4 x 72 KB of that.
Stacks (painted at boot, scanned during the runs): user stack (main loop, 24,320 B) **7,248 B used**; interrupt stack (7,936 B) **2,636 B used** (audio ISR with the key scan nested). Same figures in every scenario;
paths **not** exercised: USB / MIDI traffic, an OTA session, the flash writes of a preset SAVE beyond the name editor. Peak RAM = `.bss` end + the two stacks = nothing grows at run time.

### (e) Latency, note -> sound

Key -> start of the block that plays it: **2.6-2.8 ms average, 3.7-4.0 ms worst** (debounce 1.1-2.2 ms plus waiting for the next block), identical idle and loaded (the ISR starts on the boundary whatever its previous length).
Then the double buffer adds exactly one block (the half being filled plays after the half now playing): **~5.5 ms average, ~7 ms worst** to the first sample, plus the engine's own attack. The WAV onset of a Vactrol note is 11 blocks (32 ms) after the
ISR that took the key (the engine's attack), so the one-block figure is derived from the buffer structure and **not independently confirmed at sample level**. Under overload (> 100 %) the latency would grow by the lost blocks.
A preset SAVE (`irq_save` around every 4 KB erase / page program, up to 8 sectors) shows no stall in the emulator (the SAVE does commit there, "SAVED" toast and sector erases seen, but its flash model never reports busy, so erase and program take no time: ISR spacing max 69,874 ticks): on silicon a 4 KB erase takes tens of ms with the interrupts off, i.e. several audio blocks lost per sector. **Not measured.**

## 3. What was changed because of it: the CPU governor (`src/fxstate.c`, `fxdsp.c`, `voices.c`, `main.c`; `-DNO_GOV` removes it)

The audio ISR already measures its own length. Once per block `gov_block()` keeps the **second longest of the last 4 blocks** (`gov_peak`: one start-up / reallocation spike is ignored).

* Above `GOV_SHED` (74 % of a block): the lowest-priority **running effect slot is bypassed** (track 4 slot 4 first, the order the budget admits in, in reverse), as many as it takes to free about the aim (71 %), then nothing for `GOV_WAIT` (12) blocks.
  The slot is dimmed on FX ORDER and the existing toast "CPU FULL / FX BYPASSED" shows. Mix is not touched: **presets and the displayed values are unchanged**, it is a run-time state.
* With **no effect left running** and still over: the number of **drum voices sounding in all** (4 tracks x 6) is capped; the quietest tail is stolen (ties: the track with most voices, then the oldest). The cap is computed from the measured cost of a drum voice (`dv_cost`, smoothed; 4,400 initial = the emulator's).
* Coming back: after `GOV_COOL` (480 blocks = 1.4 s; drum cap: 120) and only while `gov_peak + the slot's cost x 7/8 (GOV_UNIT: measured 0.67..0.90 ticks per unit) <= GOV_ADMIT` (68 %). A slot / voice that asks while the load is over that line waits (new slots are not admitted before 4 blocks are measured).
* The static `FX_BUDGET` of NOTES-H stays as the upper bound. `tests/fxbank_test.c` got a governor section (shed from the end, one shed per `GOV_WAIT`, cool-down, return, admission refused at 70 %); `tests/state_test.c` green (`ALL OK (0 failures)`); `tools/fxhost.sh test` green.

Governed results (same scenarios, shipped firmware):

| scenario | average | worst block | blocks > 75 % / > 100 % | lost | what the governor did |
|---|---|---|---|---|---|
| default mix + budgeted chains | 70.2 % | 73.3 % | 0 / 0 | 1 | 5 slot bypasses (T1 keeps part of its chain, T2/T3 a slot or two, T4 none) |
| default mix + chains, budget off | 70.0 % | 73.7 % | 0 / 0 | 2 | 14 bypasses; 16 slots offered, 2-4 run |
| 4 x Minimoog | 69.7 % | 73.0 % | 0 / 0 | 1 | none needed |
| 4 x drums | 69.4 % | 71.5 % | 0 / 0 | 15 | all FX bypassed, cap 10 voices of 24 |
| worst case, front panel driven (8 s) | 68.1 % | 74.2 % | 0 / 0 | 1 | 8 bypasses (FX gestures re-admit) |
| 4 x drums, front panel driven | 69.8 % | 77.2 % | 2 / 0 | 15 | cap 10, 1,309 voices stolen |
| default mix + a preset SAVE | 69.5 % | 75.2 % | 2 / 0 | 1 | 10 bypasses |

The ceiling now holds around 70 % average / 74 % worst (77 % once, in the drum-stealing case). The price: the worst case no longer plays all its effects (the default mix keeps roughly one chain's worth of effects out of four) and a 4-drum-track pile-up plays 10 voices, not 24.
The 15 lost blocks of the 4-drum case are all in the first 0.6 s: 24 voices start on the same step before anything is measured (a PLAY with four full drum machines would click once).

Not fixed, by choice: the drum engine itself costs 4.2 k ticks per voice (an `exp2_fx` per envelope per sample); a recursive-decay rewrite would roughly double the headroom but changes the sound path and was out of scope.

## 4. Recommendations

1. **Measure on a real FM-1 before trusting any number here.** Expose `perf` (a hidden page, or SysEx): `audio_max_w`, the histogram, `uf` are already collected; the governor then tells how much the silicon is slower than the emulator by what it sheds.
2. Keep the governor (the `-DNO_GOV` build exists to measure raw cost). If silicon is 1.3x slower the default engine mix alone sits at ~75 %: expect effects to be bypassed early; consider shipping `GOV_SHED` / `GOV_ADMIT` lower (70 / 64 %) until measured.
3. The drum engine is the lever (36 % per busy track): cheaper envelopes, or a hard cap of ~12 voices in all.
4. Raise `app.ld` RAM to 432K (2 KB of linker room left); do not add pools.
5. A preset SAVE must mute / not run under `irq_save` for 8 sectors, or run in slices between blocks (not measurable in the emulator).
6. Chord keys: `ctrl_hw_held()` makes them correct under load; the cleaner fix (not done) is a main loop that polls the buttons between the bands of `lcd_frame`.
7. Possible flapping at the border (a bypassed reverb coming back, the load rising, shedding again every ~1.4 s): not seen in 8 s runs, not tested for minutes; an exponential back-off on `GOV_COOL` is the easy cure.

## 5. Not measured / limits

* Silicon timing, USB/MIDI input load, OTA, flash erase stalls, long runs (>8 s), the encoder / button feel in the hand, anything with real MIDI clock.
* One scenario family; the engines' cost is nearly constant (drums depend on voices sounding), so other settings move the numbers little, but chains with other effect mixes were not swept (NOTES-H did).
* The emulator's tick = 10 cycles of its 1-IPC model; multi-cycle instructions are charged, cache misses and flash waits are not.

## Files

* `src/fxstate.c`: `perf` (extended, `NO_PERF`), `perf_audio()`, the governor state and `gov_block()` (`NO_GOV`).
* `src/main.c`: ISR hooks (key latency, `gov_block`, `perf_audio`), main-loop / frame statistics, stack painting and scan, `CBOF` + `ctrl_hw_held` in the key callback.
* `src/fxdsp.c`: `fx_admit()` with the governor, `fx_alloc` timing. `src/voices.c`: per-track engine timing, `gov_drums()`, drum voice cost. `src/lfo.c`: mod timing fixed.
* `src/ctrl.c/.h`: `HELD()`, `ctrl_hw_held`, `ctrl_bank_mode_key`. `src/loadtest.c` (`-DLOADTEST`), `tools/loadtest.py`, `tests/fxbank_test.c` (governor section).
