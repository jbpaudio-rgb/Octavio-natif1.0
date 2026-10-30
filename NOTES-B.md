# Milestone 2, part B (controller layer) — status
Done: src/ctrl.h, src/ctrl.c = port of the prototype's btn(), latch/lock/chord (350 ms), SELECT special press,
HOME+SAVE bank mode, preset bank (sort, save, name editor state, load), PRESETS/ALGORITHM/SELECT wheel (1/50 steps,
unrounded base), list/toast timers, lit/locked masks. Host test: tests/ctrl_test.c (gcc, links with a PV stub).
NOT done: overlay.c drawing (list/name/toast glyphs), hooks in main.c/ui.c (release edges, enc0/1/6, ctrl_tick,
ui_goto), flash persistence (bank_store/bank_restore are weak hooks), emulator verification.
Shared files modified: none yet.
