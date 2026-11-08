# Mac OS 9 port (PowerPC, CarbonLib)

## Goal
GPM2 for Mac OS 9 (Carbon PPC, PEF/CFM) built on GitHub Actions (ubuntu, Retro68 `ghcr.io/autc04/retro68`, toolchain `retrocarbon`, like the
iWordle `build.yml`). Builds only in CI; testing is on an OS 9 VM. Artifact `GPM2-os9` (GPM2.bin MacBinary + GPM2.dsk) in `extracted/dumps/os9/`.

## Design
1. New backend `runtime/host/os9/host_os9.c` (not an #ifdef of the OS X Carbon one): CarbonLib 1.x has no HIView, CGImage, AudioUnit nor CFUserNotification; so classic
   Toolbox with WaitNextEvent, Window/Control/List/Menu Manager, CopyBits (8 bpp GWorld with the game's palette, scale 1x/1.5x/2x/... through the View > Window Size menu),
   Sound Manager (4 buffers of 2048 frames, callBackCmd marks the buffer free), Navigation Services 1.x (game folder, Open/Save), StandardAlert (MessageBox).
   Game dialogs: NewControl (button, check, radio, group, edit, text) and List Manager; the dropdown combo becomes a list (limitation).
2. No threads: the guest runs on the main thread (`gpm2_start` calls `run_guest` directly) and every wait goes through `os9_pump` (WaitNextEvent): `win32_input_wait`
   (input.c), `host_sleep_ms` and `os9_tick` (called in `next_msg`, at most every 16 ms, so as not to starve with WM_TIMER). `shim/pthread.h` zeroes mutex/cond.
3. libc: `os9_compat.h` (force-included) redirects fopen/open/stat/fstat/mkdir/unlink/opendir/readdir/realpath/usleep/localtime_r onto the File Manager;
   host path = "/Volume/Folder/file". Network: empty `hn_*` (Link Play unavailable; `host_net_ask_address` warns).
4. Memory: SIZE 100 MB preferred / 80 MB minimum (`gpm2.r`); guest 64 MB (default MEM_SIZE). Warning if `MaxBlock()` < 70 MB. Preferences and log in Preferences ("GPM2 Prefs", "GPM2 Prefs Log"); saves in "<game folder>:GPM2 Saves".
5. Icon: `tools/make_icns.py --plain --rez` generates `icons_gen.r` (ICN#/icl4/icl8/ics#/ics8/icns + BNDL/FREF/'GPM2'); not committed.
6. SDK: `tools/os9-sdk/` = Interfaces (CIncludes, RIncludes) and Libraries from MPW, the same set as iWordle (the Retro68 script needs them; without them it falls back to Multiversal without Carbon).

## Log
- Run 41535825544: `build-os9` OK (PEF PPC via Retro68; artifact `GPM2-os9` = GPM2.bin + GPM2.dsk in extracted/dumps/os9/). be-test and build-ppc remain green.
  Along the way: Standard File does not exist in Carbon (uses Navigation Services); `lround`/`cexp` are missing from Retro68's libm (defined in os9_posix.c, compiled with -fno-builtin).
- NOT tested: it only compiled. Risks: main thread stack (the "GPM2 Prefs Log" log writes StackSpace(); guest and generated code may need more — if it hangs, move the guest
  to a cooperative Thread Manager thread with a large stack); open()/fopen() of libretro with "Vol:Dir:file" paths; 8 bpp CopyBits with the palette swapped on every frame;
  performance on a G3; game dialogs (dropdown combo = list); network messages (Link Play unavailable).
