# Agent G - presets and key scenes save every engine (branch m4-G)

## What is saved
A preset (SAVE) and a key scene (HOME + SAVE, then a white key) now hold the whole machine state, not only `PV[]` of the Vactrol:

| section | content |
|---|---|
| 0x08 TRK | the engine of each of the 4 tracks + the track that owns the (single) Vactrol |
| 0x01 CORE | Vactrol controls `PV[P_FOLD..P_MODE]` |
| 0x02 MOD | LFO 1..4 / S&H 1..2 and their destinations `PV[P_M0T..P_M11Y]` |
| 0x03 FX | effect knobs `PV[P_F0DRIVE..P_F5SYNC]`, raw, rev = `FXP_REV` (see below) |
| 0x04 ARP | arpeggiator page `PV[P_ARPMODE..P_ARPPAT]` |
| 0x05 SEQ, 0x06 STEPS | sequencer lines (destination, key, amount, step value) and the 6 x 16 step values |
| 0x09 TARP | the 4 tracks' arpeggiators as `voices_arp` holds them (4 x 12) |
| 0x10 + 3 * track + (engine - 1) | the Minimoog (31), Prophet (32) and drum (66, DR_KIT slot left out: it is a command) parameters of that track: all 12 (track, engine) pairs, not only the engine a track runs now |
| 0x07 MIX | track volumes `PV[P_VOL0..3]` (key scenes only, like the prototype's `q.vol`) |

Not saved (global settings, the prototype's `snapAll()` does not have them either): tempo, MIDI channels, MIDI routing. Before, a preset restored these too (it copied all of `PV[]`); now loading a preset no longer changes the MIDI channels.
Loading: engines of the tracks first (`voices_set_engine`, `PV[P_TYP*]` follows so HOME shows them), then the values; every value is clamped to its range. A never-saved scene still resets to the factory state ("DEFAULT"): engines VACT, MINI, PRO70, DRUM and all factory values (`state_defaults()`).
`ALGORITHM` now also updates `PV[P_TYP*]` (HOME's engine name followed only the presets before: one line in ctrl.c).

## Format (src/state.c, src/ctrl.c, src/persist.c)
Flash record = 16-byte header (magic "OCTB", version, payload length, CRC-32 of the payload; same as before) + payload.
* payload version `0x00020001` (presets) / `0x00030001` (scenes): a list of entries, kept in RAM in exactly this form (ctrl.c pools `bpool` 23 024 B and `spool` 15 344 B: written to / read from flash with no second copy).
* preset entry: `u16 len | char name[13] | u8 type | u16 k | sections`; scene entry: `u16 len | u8 slot | sections`.
* section: `u8 id | u8 rev | u16 len | data`. Value sections: `enc u8 | nvals u8 | body`; enc 0 raw int16, enc 1 sparse `(index u8, int16)` pairs against the factory values (the writer takes the shorter). A reader skips a section whose id it does not know or whose rev / nvals differ (the layout it describes is gone) and leaves a missing section's values alone. Factory values of the engines and of the sequencer are read once at boot (`state_init()` in `ctrl_init`).
* Version of the format: record version (entry framing), section rev (each block). Appending sections needs no version bump: an old firmware skips them.
* **FX zone for agent H**: the FX section is `PV[P_F0DRIVE..P_F5SYNC]` raw with `rev = FXP_REV` (state.c, currently 1). When the effects become per track and `PV` changes, change the range / the table `PVR[]` in state.c and bump `FXP_REV`: old FX blocks are then ignored instead of landing on the wrong knobs. Ids `0x40..0x4F` are reserved for per-track FX sections (`secd_t` kind to add in `st_sec` / `st_get` / `st_put` / `st_def`). Nothing else needs touching.
* Old format (Vactrol only, version = sizeof(preset_v1_t) for presets, `0x10000 | P_COUNT` for scenes): still read at boot and converted (PV sections only; the engines stay as they are when such a preset is loaded). Presets of the old format lived at 0x93000: they are read there and the sector is erased after the first save in the new region, so a stale copy cannot come back. The conversion only works while `P_COUNT` is the one of the old record.

## Flash layout (checked against bsp/chip/fm1_flash.h: `FL_XDATA1` = 0xE7000..0xFC000 "a firmware's own data")
| region | before | now |
|---|---|---|
| presets | 0x93000, 4 sectors (15 344 B) | 0xEB000, 6 sectors (23 024 B), 0xEB000..0xF1000 |
| scenes | 0xE7000, 2 sectors | 0xE7000, 4 sectors (15 344 B), 0xE7000..0xEB000 |
0xF1000..0xFC000 stays free. Records are cut in 0xF00-byte pieces, one per 4 KiB sector (bytes 0xF00..0xFFF stay erased: update-loader record rule, as before; the 0xE8F00 stock record location is inside a scene piece boundary and untouched). Only the sectors a record needs are erased. Pages are programmed from the end to the start, header last, IRQs off around each call (as before).
Why bigger: a full entry is up to 1 729 B (all 12 engine blocks + Vactrol/LFO/FX/arp/seq), the old 4 sectors would hold 8 worst-case presets. With the sparse coding a typical preset (one Minimoog edited, 7 Vactrol knobs, 3 FX knobs) is 215 B + 18 B header.
RAM: the pools (23 024 + 15 344 B + a 1.9 KB entry buffer) replace the old `bank[48]` + `scn` + `ps_buf` (36 KB): `_bss_end` 0x01C46A00 -> 0x01C47140 (+1.8 KB net). The 4-sector scenes and 6-sector presets are checked by static asserts in persist.c against the pools.

## Limits when full
BANK_MAX is still 48. A save that does not fit (pool full or 48 entries) is refused with the toast "BANK FULL / NOT SAVED" and changes nothing (the old preset stays). Capacity (host test, random values): 25 % of all values edited: 32 presets (avg 715 B) fit; every one of ~600 values edited: 13 presets, 8 of 16 scenes (a scene is 1 728 B worst case, the 15 344 B scene region fits 16 scenes up to ~950 B each: typical scenes are 130..700 B).

## Tests
`tests/state_test.c` (gcc on the host, same sources as the firmware, an in-memory NOR flash that rejects erases / programs outside the allowed ranges, over-programming and crossing a 0xF00 piece):
`gcc -O1 -w -Isrc -Ibsp/chip -o /tmp/state_test tests/state_test.c && /tmp/state_test [image from tools/mkflashrec.py]`
* round trip through the real UI path (SAVE, SAVE; HOME + SAVE + key; PRESETS knob; HOME + key): random state in all 4 tracks, engines in random layouts, all of Minimoog / Prophet / drum / Vactrol / LFO / FX / arp / sequencer values, then a scramble, then reload, compared value by value (also with every value changed, twice) - identical;
* overwrite keeps the count; a never-saved scene gives the factory state;
* "reboot": RAM pools wiped, rebuilt from the flash image only - presets (names, count), scene mask and a scene's content identical;
* old-format records (built byte for byte like the old persist.c) at 0x93000 / 0xE7000: read, converted, loaded; first new save rewrites in the new format and erases the old copy;
* power cut injected at each of the first 23 flash operations of a save: always the old record, the new record or an empty bank (the sector holding the header is erased first: the old record is gone from then on, as before this change), never a mixed one;
* an image written by `tools/mkflashrec.py` (independent Python encoder) boots and loads;
* a full bank refuses a save without changing anything; clean under `-fsanitize=address,undefined`.
Firmware image (build.sh + mkpkg, octavio.bin 521 268 B) run in fm1-emu: SAVE, SAVE lists "VACT 01"; HOME + SAVE + first white key shows "BANK 1 SAVED / 4 TRACKS"; ALGORITHM to MOOG, then HOME + the same key shows "BANK 1 LOADED" and the Vactrol page is back. FM1_FLASH_TRACE: boot reads the headers at 0x93000, 0xEB000, 0xE7000; a preset save erases 0xEB000 (one sector) and programs 161 B there; a scene save erases 0xE7000 and programs 152 B (only the sectors needed).
`host/render.c` links again (a `fx_bus` stub). The old `tests/ctrl_test.c` does not link without voices.c (it did not before either).

## Not tested / honest limits
* Not on hardware: real erase/program time (a preset save now erases up to 6 sectors instead of 4, a scene save up to 4 instead of 2, synchronously in the UI path as before; only the sectors the record needs), flash endurance.
* fm1-emu does not keep the chip between runs (and `--fwsc` overwrites `--raw-flash`), so "survives a restart" was proved on the host with the real persist.c against an in-memory flash, not in the emulator. The emulator was used for the save and load UI paths in one run.
* Notes held while a preset loads are cut (`vac_note_off`, engine changes); the sound of other engines' running voices is not reset.
* Voice-level runtime state (envelopes, phases) is not saved; neither is the arpeggiator's running position.
* `PV[P_VOL*]` is not wired to the track level by the rest of the firmware (voices_track_level is only called when a scene is loaded now); the HOME faders' wiring is not part of this work.
* The saved arpeggiator exists twice (PV page values and the per-track tables of voices.c) because the firmware does not link them yet; both are restored.
* A preset made on an older firmware keeps only the Vactrol / LFO / FX / arp / seq PV sections; loading it leaves the engines untouched.
