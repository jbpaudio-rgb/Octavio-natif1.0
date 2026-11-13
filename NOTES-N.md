# NOTES-N : second core (CPU1) for the Prophet

- `OCT_CPU1` (main.c; `-DNO_CPU1` in XCFLAGS builds without it, build.sh then skips fm1_cpu1.S). `FM1_CPU1_FALLBACK 1`: a misread, a fault or a late job retires CPU1 until power-off, no restart.
- `fm1_cpu1_start()` in fm1_cstart, at power-on, interrupts off, before the guards / audio / timers.
- `proph_render` (eng_prophet.c): the sounding voices alternate CPU0 / CPU1; CPU1 renders its share into `pj_out` (job `proph_job`, from the flash), CPU0 its own into `out`, then adds. One voice, CPU1 off / retired, job not handed over: CPU0 renders all. A dropped job's voices are rendered by CPU0 after the wait.
- The HUD (HOME + REC) shows `C1+` (running), `C1-` (off), `C1!` (retired) at the end of the VOICES line.
- RAM: CPU1's stacks + mailbox ~3.3 KB; FXPOOL_N 36832 -> 36288 samples per track to make room.
- Known limit (fm1_cpu1.h): on some units CPU1 misreads shared RAM; only the mailbox has a check word, a misread of the voices' own data is not detected (a click, not a crash).
- Not measured on the device when written: the CPU gain; the audio side is only tested on the host (the emulator has no second core).
