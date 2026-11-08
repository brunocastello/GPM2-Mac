# Import-driven Windows runtime: from the entry point to the main menu

**Parent:** 01-overview-static-recompilation (F3+F4 together, per the session 5 Log). **Base:** F1 accepted
(run 41453881319). **Date:** 2026-11-02..

## Goal

The recompiled `Gpm2.exe` 1.02B runs on the Mac, from the original entry point
(`0x539D08`, CRT startup → `WinMain`), over the game's **original files**.
There are two milestones.

- **M1 — headless (this plan):** `gpm2run` (CLI) runs up to the message loop and
  dumps each presented frame (WinG/GDI → screen) as an 8-bit BMP with the current
  palette.
  - **Acceptance:** the **main menu** frame appears in
    `out/frames/NNNN.bmp` and the game sits idle in the message loop without aborting.
  - The lead checks the BMP visually. The definitive visual acceptance is the user's.
- **M2 — macOS window (next plan, do not implement now):** the same core
  with an AppKit backend (NSWindow, mouse and keyboard → `WM_*`).

The method is **import-driven**:
1. run;
2. the first import without an implementation aborts with name, arguments and
   return address;
3. implement that import;
4. run again.

Repeat until acceptance.

## Constraints (mandatory)

- **Nothing installed on the Mac.**
  - The generated C comes from CI: artifact `recomp-f1-fuzz`, already downloaded to
    `extracted/dumps/recomp-f1/fuzz-run/gen/`.
  - Compile and run **locally** with Xcode's clang. This is allowed; it is not an
    installation.
  - If the translator changes, download the new `gen/` from a green run.
- **Game files:**
  - data in `original/GPM2/`;
  - the whole CD in `original/`;
  - 1.02B exe in `original/GPM2 Updates v1.02b/Gpm2.exe`;
  - SHA-256 `266f214714b6a2d3396f969f0e142dc267b63d80beb255b9998f66ffcd253546`,
    equal to `bin/Gpm2.exe`.
  - **Never write to `original/`.** The game's writes (saves, `.ini`, created
    files) go to a writable directory (`--write-dir`, default
    `out/writable/`), with layered reads: first the writable one, then the
    original.
- **PowerPC portability (01-overview-static-recompilation note, session 5):**
  - C99 compilable by GCC 4.0/4.2: no C11, no blocks, no new builtins;
  - all reads and writes of guest memory through `R8/R16/R32/W8/W16/W32` or through
    helper functions on top of them. For strings and buffers, `guest_ptr(addr, len)`
    with checking is acceptable for bytes; never reinterpret guest memory as native
    `uint16_t`/`uint32_t`;
  - **portable core in C** (`runtime/win32/`) separate from the **platform
    backend** (`runtime/host/`). In this plan, only the `host/headless.c` backend.
- **Windows → macOS interface (user decision):**
  - title bar, Win32 menus, `MessageBoxA`, `DIALOG` dialogs and Open/Save
    become, in M2, native macOS equivalents;
  - in M1, the core only needs to **record** and return plausible results
    (for example, `MessageBoxA` logs the text and returns `IDOK`; a modal dialog
    logs the template and returns a documented default value);
  - **only the client area drawn by the game needs to be 1:1.**
- **Behavior fidelity:**
  - **`rand`/`srand`:** the MSVCRT20 LCG
    (`state = state*214013 + 2531011; return (state>>16)&0x7FFF`).
  - **`qsort`:** MSVCRT20 algorithm (cutoff 8, `shortsort` by selecting the
    maximum, middle element swapped to `lo`, smaller side first). The reference
    is in `app/Sources/GPM2Core/Original/CRT.swift`.
    The comparison calls the guest function through `guest_call`.
  - **`time`/`localtime`/`GetTickCount`/`GetLocalTime`:** real time by default;
    `--fixed-time <epoch>` makes everything deterministic.
  - **`sprintf` and family:** MSVC formatting. Start with the host's `vsnprintf`
    converting the specifiers; note the known differences (`%g`,
    3-digit exponent in MSVC: `1e+005`) and implement the 3-digit
    exponent.
- **Commit messages** in Portuguese, ending with
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Small commits.
  Do not commit `gen/`, outputs or game files.
- **No Linux.** Run locally on macOS arm64. CI only needs to stay green
  (the current jobs). If simple, add to the `f1-build-macos` job a step
  that compiles `gpm2run`, without running it, because CI does not have the game files.

## Import ABI (how the generated C calls)

- `gen/imports.c` defines `imp_<DLL>_<name>()`, which today calls
  `guest_import(idx, "DLL!name", pop)`.
  - On entry, `g.esp` points to the guest **return address**; the
    arguments are at `g.esp+4`, `+8`...
  - The import must:
    1. read the arguments;
    2. put the result in `g.eax` (and in `g.edx` for 64 bits; in ST0 via
       `fx_push` for `double`);
    3. pop `4 + pop` bytes (`pop` is the already known stdcall count,
       coming from `recomp/importsig.py`);
    4. call `RETCHK(ra)`.
  - See `mock_import` in `tests/fuzz_driver.c`, which does exactly this.
- **Implementation:** replace the dispatch by name with a table registrable in the
  runtime (`win32_register("USER32.dll!CreateWindowExA", impl)`), built at
  initialization and indexed by `idx`.
  - Anything without an implementation falls into `unimplemented(idx, name)`. This function
    prints the name, the return address and the first 8 arguments (hex, and the
    string if the value points to printable ASCII in guest memory), and exits with
    code 3.
  - Keep `guest_import_hook` working for the test drivers; they must
    not break.
- **Conventions:**
  - USER32/GDI32/KERNEL32/WING32/WINMM/SHELL32/comdlg32/DPLAYX = stdcall, with
    the exception of the cdecl variadics already listed in `importsig.py` (`wsprintfA`);
  - MSVCRT20 = cdecl (`pop` = 0);
  - if `importsig.py` does not know the `pop` of an import you implement,
    add it there (with source: Win32 SDK prototype) and regenerate. A wrong `pop`
    misaligns the stack; `RETCHK` and the shadow stack flag it.
- **Host → guest callbacks** (`guest_call(addr, n, args...)`), for WndProc,
  DlgProc, TimerProc, the `qsort` comparator and `_initterm`:
  1. push the arguments (from last to first) and a sentinel return address
     (`0x00F00010`, for example);
  2. `shadow_push(sentinel)`;
  3. `dispatch(addr)`;
  4. on return, `g.esp` must be at the expected value: the stdcall callee already removed the
     arguments; for cdecl (`qsort`, `_initterm`), the caller removes them;
  5. return `g.eax`.
  - Save and restore the guest's callee-saved registers per the
    convention, so as not to corrupt the guest caller's state.
  - Reentrancy is mandatory: a `SendMessage` inside the WndProc calls the WndProc
    again.

## Organization

```
runtime/win32/core.c        import table, unimplemented(), guest_call, log/trace, guest strings
runtime/win32/heap.c        guest heap from 0x01000000 (malloc/free/realloc/calloc/_msize, GlobalAlloc/LocalAlloc)
runtime/win32/msvcrt.c      CRT (FILE* and _open/_read files over the vfs, string, mem, rand, qsort, time, sprintf, startup: __getmainargs, _initterm, __p__*, exit)
runtime/win32/kernel32.c    module, startup info, files (_lopen/_lread/CreateFileA...), time, Get/WritePrivateProfileStringA, GetVersion etc.
runtime/win32/vfs.c         DOS path -> host mapping (case-insensitive, `\` -> `/`, drive letter -> game or CD folder), writable layer
runtime/win32/user32.c      windows (HWND table), classes, message queue, Send/Post/Get/Peek/Dispatch, timers, cursor, resources (LoadStringA, LoadIconA, LoadCursorA, LoadMenuA, LoadAcceleratorsA), MessageBoxA (log), dialogs (log + default value in M1)
runtime/win32/gdi32.c       DCs, palette (CreatePalette/SelectPalette/RealizePalette/SetDIBColorTable...), objects, BitBlt/StretchBlt over 8-bit surfaces, minimal text
runtime/win32/wing32.c      WinGCreateDC/WinGCreateBitmap (DIB in guest memory), WinGBitBlt/WinGStretchBlt to the window surface, WinGRecommendDIBFormat, WinGSetDIBColorTable
runtime/win32/winmm.c       timeGetTime, multimedia timers (register; callbacks in the loop), waveOut/sndPlaySound/mci: in M1, only register and return success, no audio
runtime/win32/misc.c        SHELL32, comdlg32, DPLAYX: stubs with log
runtime/win32/resources.c   reading the exe's .rsrc into guest memory (string tables, dialogs, menus, icons, cursors, bitmaps)
runtime/host/host.h         backend interface: present(frame 8bpp, palette), poll_events(), now_ms(), sleep_ms()
runtime/host/headless.c     present -> BMP in out/frames/; no events (or a script of events from --script, optional)
runtime/gpm2run.c           main: arguments, loads the image, sets up the stack/TIB, registers the imports, calls the entry point, exits
runtime/Makefile            target `run` -> build/gpm2run (lib without GUEST_CHECK; target `run-chk` with GUEST_CHECK for debugging)
```

## Details that matter

- **Stack and TIB:**
  - guest stack `0x00100000–0x00400000`, initial `esp` near the top;
  - `fs:` with a fake TIB (the recompiler already handles `fs:`; see how the fuzz
    driver initializes `FS_BASE`);
  - `_except_handler3`: SEH is only registered; there are no exceptions.
- **Host stack:** guest recursion becomes C recursion. Run the guest on a
  pthread with a configurable stack (default 256 MB, `--host-stack-mb`), not 1 GB.
- **Guest heap:**
  - from `0x01000000` to the end of guest memory;
  - simple, deterministic allocator (first-fit with a size header),
    aligned to 8, never returns 0 for size 0;
  - `_msize` works;
  - **addresses must be deterministic between runs.**
- **Resources:** `LoadStringA`, `LoadMenuA`, dialogs etc. read the image's `.rsrc`.
  There is already a dump in `extracted/dumps/exe/` (`dialogs.json`,
  `menus.json`), useful for checking, but the runtime reads from the exe itself.
- **Window and screen:**
  - each HWND has a rectangle and an 8-bit client-area surface;
  - `GetSystemMetrics` reports a 640×480 screen;
  - the game uses WinG: `WinGBitBlt` from the guest DIB to the window's DC copies to the
    window surface and marks "present";
  - the headless backend presents at the end of each `WinGBitBlt`/`BitBlt` to the
    window and, preferably, only when the image changed (hash), so as not to generate
    thousands of identical BMPs;
  - write a `frames.txt` log with the frame number and the import that generated it;
  - **check the real resolution in the code (decompiled in
    `extracted/dumps/ghidra/decompiled.c`)** and note it.
- **Palette:**
  - keep the game's logical palette (`PALETTEENTRY` entries) and the 20 reserved
    system colors of a Windows 95 in 256-color mode;
  - document which entries the game defines;
  - the BMP uses the effective palette at the moment of `present`.
- **Message loop:**
  - `GetMessageA` with no messages and no expired timers = idle;
  - in headless, after N consecutive idles (`--idle-exit 200`), exit with
    code 0 and the message "idle in message loop";
  - `PeekMessageA` with `PM_REMOVE` works;
  - **timers** (`SetTimer`, `timeSetEvent`) fire by virtual time: with
    `--fixed-time`, time advances 1 ms at each idle `PeekMessage`/`GetMessage`,
    or another documented deterministic rule.
  - send `WM_CREATE`, `WM_SIZE`, `WM_ACTIVATE`, `WM_PAINT` (with `BeginPaint` and
    `EndPaint`) and `WM_QUERYNEWPALETTE`/`WM_PALETTECHANGED` in the order that Windows 95
    sends them on `CreateWindowExA` + `ShowWindow` + `UpdateWindow`. Document the
    sequence used.
- **Trace:**
  - `GPM2_TRACE=1` logs each import call with arguments and return,
    numbered, in `out/trace.txt`;
  - without trace, only the final summary: imports called and count.
  - This also feeds the list of imports needed for M2.

## Execution

1. **Skeleton:**
   - `gpm2run` + `core.c` + `unimplemented()`;
   - the first run should already abort at the 1st import of the CRT startup
     (something like `GetVersion` or `__getmainargs`);
   - commit.
2. **Loop:** run → implement the missing import (with Win32 semantics from the
   official documentation; do not guess) → run. Commit at each group of imports.
   - For each implemented import, one line in the inventory
     `docs/plans/06-win32-imports.md` (create it): name, state (complete / partial /
     stub), notes.
   - The path is
     `docs/plans/06-win32-imports.md`.
3. **Suspected divergence of the translated code** (for example, an
   unbalanced-stack trap): **before** touching the translator, check the import's `pop`
   and `guest_call`. If it is still the translator, describe the case (instruction, VA) and stop
   for the lead to decide. The translator is validated by F1; a bug in it is rare.
4. **End of M1:**
   - the BMPs show the main menu;
   - `gpm2run` ends idle with code 0;
   - the trace is saved;
   - note in the Log below: imports implemented (count per DLL), the last
     frame, the initialization sequence and what remained a stub.
   - Copy the menu BMP to
     `extracted/dumps/run-m1/`.

## M1 acceptance

- `gpm2run --game "<original/GPM2>" --exe "<Gpm2.exe 1.02B>" --write-dir out/writable --fixed-time 851990400`
  reaches the message loop and ends idle with code 0.
- A BMP of the main menu, with correct colors (the game's palette), is in
  `extracted/dumps/run-m1/`.
- No writes to `original/`.
- `make -C runtime fuzz insn` still compile; the CI tests stay
  green.

## Log

- 2026-11-02 — specification created (session 5).
- 2026-11-02 — **M1 reached** (old development repo, commits `11b4314` and `9c710db`). `gpm2run` runs the 1.02B `Gpm2.exe`
  from the entry point `0x539D08` to the menu and ends idle with code 0, deterministically
  (`--fixed-time 851990400`: two runs give the same 85 BMPs; it also passes with `GUEST_CHECK`, `run-chk`).
  - **Result:** 85 distinct frames in `out/frames/` (fade of the "Grand Prix Manager 2" logo, animations,
    "CAREER OPTIONS" screen with Quick Start / Long Term Contract / Short Term Contract / Link Play / Load Game).
    Last frame `0084.bmp`; copy in `extracted/dumps/run-m1/` (`menu-0084.bmp/.png`, `logo-0040.bmp`,
    `frames.txt`, `trace.txt`, `run.log`). Colors checked visually (game palette correct).
    Stopped by idleness: 200 polls (empty PeekMessage or WaitMessage) with no new frame.
  - **Screen:** `GetDeviceCaps` reports 640x480, 8 bpp, `RASTERCAPS` with `RC_PALETTE` (the game refuses to run without
    it: "requires Windows to be in 256 colour mode"). The game creates 1 top-level window with a 640x479 client
    (`CreateWindowExA` style `0xA0000` = `WS_SYSMENU|WS_MINIMIZEBOX`, no `WS_CAPTION`, height
    `2*SM_CYBORDER + SM_CYCAPTION + 0x1ca`), with a WinG back buffer of 638x-1455 (top-down, stride 640,
    logical screen 640x460) and does `WinGBitBlt(win, 0,0,640,460, dib, 0,0)`; the BMP is the window surface
    (640x479; the bottom strip 460..478 comes out black; origin not verified: either the game paints black in `FillRect`, or the background is never erased in the current model).
  - **Imports:** 260 registered (101 called up to the menu, 11959 calls): USER32 95 (36 stubs), KERNEL32 33
    (8 stubs), MSVCRT20 81 (2 stubs), GDI32 32 (8 stubs), WING32 7 (0), WINMM 5 (4), SHELL32 3 (3),
    comdlg32 2 (2), DPLAYX 2 (2). Full inventory: `06-win32-imports.md`. Stubs actually called on the
    path to the menu: `sndPlaySoundA`, `DragAcceptFiles`, `mciSendCommandA`. Stubs not exercised
    (DialogBoxParamA, CreateDialogParamA, Dde*, menus, TextOutA/LineTo/Polygon/Pie/DrawTextA, hooks,
    GetOpenFileNameA/GetSaveFileNameA, DPLAYX) only log and return a default value; they will be needed when
    clicking the buttons (M2).
  - **Initialization sequence used:** `CreateWindowExA` -> `WM_GETMINMAXINFO`, `WM_NCCREATE`,
    `WM_NCCALCSIZE`, `WM_CREATE`; `ShowWindow(SW_SHOWDEFAULT)` (1st time) -> `WM_SHOWWINDOW`,
    `WM_WINDOWPOSCHANGING`, `WM_ACTIVATEAPP`, `WM_NCACTIVATE`, `WM_ACTIVATE`, `WM_SETFOCUS`,
    `WM_QUERYNEWPALETTE`, `WM_WINDOWPOSCHANGED` (-> `WM_SIZE`, `WM_MOVE`); `WM_PAINT` comes out of the queue when there is no
    posted message (`BeginPaint` sends `WM_ERASEBKGND`). Child windows (EDIT) do not activate. Built-in classes
    (BUTTON, EDIT, STATIC, LISTBOX, COMBOBOX, SCROLLBAR) use a host DefWindowProc.
  - **Palette model:** the system palette (`sys_pal`) starts with the 20 reserved Windows 95 colors;
    `RealizePalette` copies the selected logical palette (`SYSPAL_NOSTATIC` = identity over the 256 entries;
    `SYSPAL_STATIC` = only 10..245); the frame uses `sys_pal` at the moment of `present`; blits copy indices
    (identity). The DIB color table (`WinGSetDIBColorTable`) is stored but not used in the output.
  - **Time:** with `--fixed-time` the clock is virtual (`GetTickCount`, timers, `time`): +1 ms per idle poll;
    `WaitMessage` jumps to the next timer. `localtime` uses UTC in this mode. `rand`, `qsort` and `sprintf`
    follow MSVCRT20 (LCG 214013/2531011; qsort cutoff 8; 3-digit exponent).
  - **Data:** `X:\gpm2\...` (any letter) -> `original/GPM2`; other absolute paths -> CD root;
    writes always in `--write-dir` (copy-on-write); `.ini` without a directory goes to `C:\WINDOWS\` (writable
    layer only). Nothing was written to `original/` (checked by mtime). In this run the game wrote no file.
  - **Translator:** no suspected divergence; no import `pop` needed changing (`importsig.py`
    untouched). `RETCHK` with `GUEST_CHECK` did not flag an unbalanced stack.
  - **Open for M2 / next steps:** input (mouse/keyboard -> `WM_*`, `WM_CHAR` in
    `TranslateMessage`), native dialogs and menus, audio (`sndPlaySoundA`/`waveOut`/MCI), GDI text
    (the game uses its own text in the DIB, so only the dialogs need it), `LoadMenuA`/`LoadIconA`
    with real resources, `.ini` behavior on first use (`GPM2MICP.INI` in C:\WINDOWS), and check
    the bottom strip 460..478 against real Windows.
- 2026-11-02 (session 5, lead's review) — **M1 ACCEPTED.**
  - `extracted/dumps/run-m1/menu-0084.png` shows the "CAREER OPTIONS" screen with the correct colors, generated by the original recompiled code over the original files. The game sits idle in the message loop.
  - **19-line black strip at the bottom (460–478) explained:**
    - in the original, the window has a Win32 menu bar (File/Options/Help + "Next Race" + money, see `original/savegames/Screenshot*.png`), and `SM_CYMENU` = 19 on Win95;
    - so the real client area is **640×460**, the same size as the `WinGBitBlt`;
    - the M1 runtime did not subtract the menu.
    - **In M2:** a window that has a menu subtracts `SM_CYMENU` from the client area, the menu goes to the macOS bar, and the NSWindow has 640×460 content.
- 2026-11-02 (session 5, lead's correction) — **The explanation of the black strip in the previous item was wrong.**
  - The game **has no Win32 menu bar**: "File / Options / Help", the icons, "Next Race" and the money are drawn by the game itself inside the 640×460 frame (see `extracted/dumps/run-m2/game-file-menu.png`).
  - The real cause was the title bar: every *overlapped* window (without `WS_POPUP`/`WS_CHILD`) has a title bar in Win32, even without an explicit `WS_CAPTION`, and the M1 runtime did not subtract it.
  - The menus the game builds (File/Options/Help, resources 0x83/0x81/0x84) live in the window's **system menu** and arrive as `WM_SYSCOMMAND` (see the 07-macos-window-input-menus Log).
  - The result (client area 640×460) stays the same.
