# PowerPC port (Mac OS X 10.4 Tiger / 10.5 Leopard)

## Goal
GPM2.app for iBook G4 (ppc, Tiger/Leopard), built on GitHub Actions (ubuntu-latest, image
`ghcr.io/variantxyz/gcc-powerpc-apple-darwin8:build-gcc-14.2-MacOSXSDK10.4u`), without running anything locally.

## Design
1. **Endian**: guest memory `M` stays little-endian; `machine.h` gains `GPM2_BIG_ENDIAN` (detected through
   `__BYTE_ORDER__`) and `LE16_/LE32_/LE64_`; `R16/R32/W16/W32/R64D/W64D`, the image loader (creates the IAT in LE) and the WAV dump
   swap bytes. The rest of the core was already byte by byte (WAV, BMP, EDY, DirectPlay packets, resources). `strnlen` (does not exist
   on 10.4) got a replacement in `dplay_eng.c`. `MEM_SIZE` can be overridden (-DMEM_SIZE).
2. **Carbon backend** `runtime/host/carbon/host_carbon.c`: one window per HWND (8 bpp blit -> ARGB CGImage via
   `QDBeginCGContext`, 1x/2x scale, View menu), game dialogs mirrored with HIView/DataBrowser, menus, mouse/keyboard through
   Carbon Events, Open/Save through Navigation Services, MessageBox/IP through CFUserNotification, audio through AudioUnit (Component
   Manager), own SHA-256 to validate the 1.02B exe. The guest thread only enqueues commands; the main thread executes
   (Carbon is not thread-safe). Guest thread stack: 32 MB (`--host-stack-mb`), guest 64 MB.
   With no saved folder and no arguments: always asks (Navigation Services).
3. **CI** (`recomp.yml`): `build-ppc` (cross-compiles, zip `GPM2-ppc`) and `be-test` (static powerpc-linux-gnu + qemu-ppc-static:
   endian_test, dplay-test, menu_test, m3_test, hooks_test, and the fuzz/insn drivers compared to unicorn x86).
4. **macOS app**: Fixes tab removed (all fixes always on; `--fix` only for tests).

## Log
- Run 41517438635/41519242430: `build-ppc` OK on the first build (Mach-O ppc, 7.6 MB; artifact `GPM2-ppc` in extracted/dumps/ppc/).
- `be-test` OK (run 41519242430): endian_test, dplay-test, menu_test, m3_test, hooks_test under qemu-ppc; fuzz (400 functions) and insn of the
  big-endian driver with no divergences against unicorn x86. Fixes along the way: `host_net_ask_address` stubs in the tests; the fuzz with 120
  functions failed only on the form-coverage criterion.
- NOT tested on a real machine (no PPC): the Carbon backend (windows, HIView dialogs, Navigation Services, AudioUnit) was only compiled.
  Risks: NavDialogRun on the guest thread, QDBeginCGContext on a non-composited window, DataBrowser in the list dialogs.
- Bug A2 (app found the game without asking): there is no embedded path in the code; probable cause: `GameFolder`/`ExePath` saved in
  NSUserDefaults (cfprefsd keeps the cache after deleting the plist) with the game folder still existing. To force it:
  `defaults delete com.brunocastello.gpm2; killall cfprefsd`.
- Unified setup (macOS and Carbon): asks only for the folder with GPM2.EXE (case-insensitive); the SHA-256 of GPM2.EXE must be that of 1.02B,
  otherwise alert "This app requires Grand Prix Manager 2 version 1.02B" (install the patch) and the app does not run. No exe prompt nor search in
  subfolders; `ExePath` removed; `--exe` stays only as a development override. The image is still loaded from the user's exe.
- Carbon: icon derived in CI from `bin/Gpm2.exe` itself (`tools/make_icns.py`, stdlib; the 32x32 icon of the RT_GROUP_ICON resource has the same
  image bytes as GPM.ICO; .icns with is32/il32/ih32/it32 + masks for Tiger and ic08/ic09 PNG for Leopard); View > Window Size (1x, 1.5x, 2x,
  3x... that fit on the screen); resizable main window with locked 4:3 proportion (free scale, smoothed if not an integer, nearest if an integer).
  Not tested on a PPC machine.
- Window Size (Carbon): the loop only listed what fit on the screen (the small VM cut the 2x); now 1x, 1.5x and 2x always appear, larger ones only if they fit.
- Icons per build: modern app = current squircle with a Windows 98 teal background (#008080, `tools/make_icon.py`); PPC (Mac OS X) and Mac OS 9 = only the GPM.ICO (from the exe's resource),
  no background, alpha preserved, enlarged by nearest neighbor with an integer factor (`tools/make_icns.py --plain`: 32/64/128/256/512 integer, 48 = 1x centered, 16 = 2x2 average).
