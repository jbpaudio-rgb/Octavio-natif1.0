# NOTES-N : second core (CPU1) for the Prophet

- `OCT_CPU1` (main.c; `-DNO_CPU1` in XCFLAGS builds without it, build.sh then skips fm1_cpu1.S). `FM1_CPU1_FALLBACK 1`: a misread, a fault or a late job retires CPU1 until power-off, no restart.
- `fm1_cpu1_start()` in fm1_cstart, at power-on, interrupts off, before the guards / audio / timers.
- `proph_render` (eng_prophet.c): the sounding voices alternate CPU0 / CPU1; CPU1 renders its share into `pj_out` (job `proph_job`, from the flash), CPU0 its own into `out`, then adds. One voice, CPU1 off / retired, job not handed over: CPU0 renders all. A dropped job's voices are rendered by CPU0 after the wait.
- The HUD (HOME + REC) shows `C1+` (running), `C1-` (off), `C1!` (retired) at the end of the VOICES line.
- RAM: CPU1's stacks + mailbox ~3.3 KB; FXPOOL_N 36832 -> 36288 samples per track to make room.
- Known limit (fm1_cpu1.h): on some units CPU1 misreads shared RAM; only the mailbox has a check word, a misread of the voices' own data is not detected (a click, not a crash).
- Not measured on the device when written: the CPU gain; the audio side is only tested on the host (the emulator has no second core).

## CPU1 extended: drums and effects (octavio-c1-govfx-1233)
- Drums: `drum_voice` renders one voice's block into an accumulator; every other sounding voice goes to CPU1 (`drum_job`, `dj_out`), CPU0 sums, then the shared tanh. Single voice is bit-identical to the old loop.
- Effects (`FX_C1`, voices.c; `-DNO_FXC1` turns it off): a track's chain (`fx_track`) is a job for CPU1 while CPU0 renders the next engine (no added latency, two buffers `fxb[2]`). One job at a time, so fx globals (FV, counters) have a single writer. While the job runs, Prophet/drum splitting falls back to CPU0.
- Governor with FX on CPU1: a quarter of the FX cost, admission up to 72 %, voices are shed before effects unless CPU0 waits (hud.fxw > 5 %) or one block exceeds 90 %.
- HUD line `FX C1 JOB / WAIT` (percent of a block). Measured on the FM-1: flanger + chorus JOB 3.2 %, WAIT 0.0, LOST 0, jitter 0.3 %.
- Open: a one-off 94 % peak seen once with the first FX build (not reproduced); the FX state holds pointers, so a CPU1 misread of shared RAM is a theoretical corruption risk (fallback build: octavio-c1-drums-1205).
