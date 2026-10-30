# octavio-native — merged milestone 2 (A pages + B controls/presets + C voices/engines)

From-scratch FM-1 firmware following the prototype. Only the felucca-bsp hardware layer (MPL-2.0) is reused.

## Built and checked in fm1-emu
- Boots; main FM page; ENV page; preset SAVE opens the NAME PRESET editor (VACT 01, OK / CANCEL); key n7 sounds at 261 Hz.
- Image 448 628 B (flash window ~581 KB). Package: build/octavio.fwsc via tools/mkpkg.py (build.sh makes only octavio.bin: run mkpkg after it).
- RAM: .data+.bss = 82.4 KB (0x01C08000..0x01C1C1C0). app.ld now gives the RAM region 256 KB (the SRAM runs up to the stacks at 0x01C74000; Felucca splits it as 72 KB RAM + POOL). The earlier "73.7 KB" figure was Felucca's partition, not a limit of this firmware.

## Not done / not verified
- Never run on hardware. SDK uboot.boot and eq_cfg_hw.bin differ from the tested version.
- Presets are RAM only (no flash save), engine parameters only.
- Only the Vactrol has parameter pages; Minimoog, Prophet and drums play but have no screens; sequencer/FX have screens but no audio.
- Text is approximated with Liberation Mono / DejaVu glyph atlases; overlay positions not pixel-diffed.
- Chords HOME+EDIT / HOME+GLO and latching not exercised in the emulator; the bank key path is wired but untested.
