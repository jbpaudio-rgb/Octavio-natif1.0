# Agent D — persistence + bank/chord checks (branch m3-D)

## 1. Flash persistence (src/persist.c, no change to the linker map)
- `bank_store()` (presets) and `scene_store()` (the 16 key scenes), called by `save_bank()` / `sc_save()` in ctrl.c;
  `persist_boot()` after `ctrl_init()` restores both (`bank_restore`, `scene_restore`).
- Flash layout (inside the BSP's FL_XDATA ranges; the app image ends ~0x6D8F4, the loader writes only below 0x93000,
  Felucca's store 0x97000.. is untouched): presets 0x93000 (4 sectors, 318 B x 48 max), scenes 0xE7000 (2 sectors).
- Record = 16 B header (magic "OCTB", format id, length, CRC-32) + payload, cut in 0xF00-byte pieces, one per sector:
  bytes 0xF00..0xFFF of each sector stay erased (update-loader record rule). All sectors erased, then pages programmed
  from the END to the start, so the header lands last; a power cut leaves either no valid record or the complete new one.
- IRQs off around each `fl_*_ram` call. A bad/foreign record (magic, format id, size, CRC) is ignored: factory state stays.
- Costs: 4 erases (~40-80 ms each on real parts) on each preset save, 2 for a scene; done synchronously in the UI path.

### Verified (fm1-emu)
- SAVE, SAVE (name editor commit): trace shows erases of 0x93000/94000/95000/96000, then program 0x93100 (82 B), then 0x93000 (256 B) = 338 = 16 + 318.
- fm1-emu does not persist the chip between runs, and with `--fwsc` it overwrites `--raw-flash` with the package.
  So "survives a restart" was tested as: records built by tools/mkflashrec.py (same format) put in the chip, boot ->
  preset RESTORED listed in PRESETS, HOME+key 0 = "BANK 1 LOADED" (values from flash), key 1 = "BANK 2 DEFAULT".
  That needed a local copy of fm1-emu (/home/claude/run-D/emu, one line: re-apply the chip image >= 0x93000 after
  stock_boot). The original fm1-emu is unchanged. The write path and the read path were each tested in the emulator,
  not chained in one process.
- Not tested: real flash timing/erase endurance (every preset save erases 4 sectors), power cut on real hardware.

## 2. Banks, chords, locks (compared with the prototype JS: scLoad/scSave, pointerdown/up, btn)
Logic in ctrl.c already matched the prototype; checked in fm1-emu with --keys:
- HOME held + white key -> `BANK n` toast; empty bank -> "DEFAULT" (**fixed**: previously silent and params untouched;
  now resets to defaults like scLoad), stored bank -> "LOADED". Key index -> white index matches the prototype (F-based keyboard).
- HOME+SAVE -> "SAVE BANK / PRESS A KEY", next white key saves ("BANK n SAVED / 4 TRACKS"), 8 s timeout (code path
  matches; the save toast itself not re-screenshotted).
- HOME+EDIT -> MIDI CHANNELS, again -> MIDI ROUTING; HOME+GLO -> INTERNAL CLOCK (TEMPO 120): all seen.
- Double tap (<350 ms) = lock: locked HOME stays "held" (bank keys work without holding; BANK 2 DEFAULT seen), next press
  unlocks (note key plays again, no bank toast). Lock of LFO not visible on screen (LED only).
- **Added**: key LEDs in bank mode (only the current bank's white key lit), as the prototype's leds(). Compiled, not
  seen (the emulator screenshot has no LEDs).
- Known gap: scenes keep only the current engine's parameters (one engine), not "4 TRACKS" like the prototype; the toast says it anyway.
