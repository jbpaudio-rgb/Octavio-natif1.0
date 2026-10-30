# Milestone 2, part B (controller layer) — status
Done: src/ctrl.h, src/ctrl.c = port of the prototype's btn(), latch/lock/chord (350 ms), SELECT special press,
HOME+SAVE bank mode, preset bank (sort, save, name editor state, load), PRESETS/ALGORITHM/SELECT wheel (1/50 steps,
unrounded base), list/toast timers, lit/locked masks. Host test: tests/ctrl_test.c (gcc, links with a PV stub).
NOT done: overlay.c drawing (list/name/toast glyphs), hooks in main.c/ui.c (release edges, enc0/1/6, ctrl_tick,
ui_goto), flash persistence (bank_store/bank_restore are weak hooks), emulator verification.
Shared files modified: none yet.

## Update: wired and verified in fm1-emu
- main.c: release edges + buttons -> ctrl_button, enc0/1/6 -> ctrl_side (SELECT/ALGORITHM/PRESETS), ctrl_tick; ctrl.c/overlay.c included.
- ui.c: ui_goto, redraw on ctrl_version, overlay_draw() at the end of ui_draw. octavio.h: 7 new overlay fonts.
- overlay.c: list, name editor, toast laid out from the prototype's CSS (Courier-style glyphs rendered by Chrome).
- Emulator snapshots (work dir /home/claude/run-B/t1): name editor "VACT 01", list with "+ NEW"/preset + "SAVED · VACT",
  ALGO list "TRACK 1", "SAVE BANK / PRESS A KEY" toast, "PRESS SAVE" note. Not yet pixel-diffed against Playwright shots.
- KNOB1..4: unchanged 1/50 wheel (the prototype has no detent/acceleration beyond it). MASTER: ADC pot (the prototype's
  "MASTER xx %" LCD line is not drawn). Persistence (bank_store/restore) still not wired; presets live in RAM.
- Shared files modified: src/main.c, src/ui.c, src/octavio.h, tools/fonts_spec.json, tools/chrome_fonts.js, assets/fonts.json (+ generated assets_gen.c).
