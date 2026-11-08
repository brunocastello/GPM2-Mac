# Static recompilation of `Gpm2.exe` (new direction of the port)

**Date:** 2026-11-02 (session 4). **Status:** adopted by the user; replaces the
manual transliteration strategy of PLAN-000 §4 (items 3 and 4).
**References studied:** CM 01/02 Bosman Edition
(open source, GPLv3: `maxevans60/cm0102-bosman-edition`) and
GP4 (Rust, `io.github.jamescarruthers.gp4`).

---

## 1. Decision

The port becomes a **static recompilation** of `Gpm2.exe` 1.02B:

```
Gpm2.exe (x86, PE32)  --recompiler (CI)-->  generated C (1 C function per x86 function)
                                                   |
                       macOS runtime (C + Objective-C/AppKit) replaces
                       USER32/GDI32/WING32/WINMM/KERNEL32/MSVCRT20/...
                                                   |
                       clang (Xcode)  -->  native GPM2.app (arm64 + x86_64)
```

- **It is not an emulator:** no CPU is interpreted at run time. Each x86
  instruction becomes C before compilation, as in CM 01/02 (`f_00401000`..., 11,781
  functions) and in GP4 (`gp4_sim::core_gen::l_403a1e`...).
- **1:1 behavior and UI without reconstruction:** the original code draws each
  screen, calls `rand()` in the same order and applies the same formulas.
- **Bugs:** fixed by replacing functions at verified addresses (§7), with the
  original version available behind an option.
- **Language:** the user lifted the Swift requirement (2026-11-02). The generated C and
  the runtime are in C. Integration with macOS (window, events, audio, file
  dialogs) is in Objective-C/AppKit, using only system frameworks.

## 2. Verified facts about the binary (target)

| Fact | Value | Source |
|---|---|---|
| Executable | `Gpm2.exe` 1.02B, PE32 i386 GUI, 1,651,712 B, SHA-256 `266f2147…3546` | `bin/`, `oracle` workflow |
| Compiler / CRT | MSVC 2.0, `MSVCRT20.dll` | imports |
| Sections | `.text` 0x401000 (0x13CB4A), `.bss` 0x53E000 (0x1D5E88), `.rdata` 0x714000, `.data` 0x715000 (0xAA60), `.idata` 0x720000, `.edata`, `.rsrc` 0x723000, `.reloc` 0x750000 | PE header |
| In-memory image | 0x400000–0x76C000 (≈3.4 MB) | same |
| Functions | 2,258 (own map) / 2,461 (Ghidra) | `extracted/dumps/exe/functions.csv`, `extracted/dumps/ghidra/functions.csv` |
| Calls | 13,360 direct, 4,300 to imports, 3,555 via thunk (114 thunks), **143 indirect** | `calls.csv` |
| Table `switch` | **325** `jmp [4*reg + table]`, with the table right after the `jmp`, inside `.text` | `gpm2-102b.asm` |
| x87 usage | ≈360 instructions (`fld/fstp/fild/fmul/fcomp/fnstsw`...) | same |
| Relocations | complete `.reloc`: every absolute pointer is known | `relocs.csv` |
| Resources | 22 DIALOG, 4 MENU, 306 STRING blocks | `dialogs.json`, `menus.json` |

**Windows surface (calls by import, what the runtime must cover):**

- **Text and strings:** `LoadStringA` 2,636 · `sprintf` 991 · `strcpy` 827 · `strcat` 367.
- **RNG and files:** `rand` 407 · `_lread` 266 · `_lwrite` 258 · `_lopen` 31 · `_lclose` 47.
- **Dialogs:** `SendMessageA` 94 · `GetDlgItem` 92 · `EndDialog` 37 · `DialogBoxParamA` 22 · `SendDlgItemMessageA` 27 · `CheckDlgButton` 19 · `IsDlgButtonChecked` 17 · `CheckRadioButton` 5 · `CreateDialogParamA` 4.
- **Timers and callbacks:** `SetTimer` 30 · `KillTimer` 33 · `qsort` 27 (the comparator is a callback).
- **WinG:** `WinGBitBlt` 12 · `WinGSetDIBColorTable` 9 · `WinGStretchBlt` 6 · `WinGCreateBitmap`/`WinGCreateDC` 3.
- **GDI:** `SelectObject` 26 · `CreatePen` 15 · `GetTextMetricsA` 7 · `DrawTextA` 6 · `TextOutA` 2 · `ExtTextOutA` 2 · `CreateFontA` 2 · `Polygon` 4 · `Pie` 3.
- **Sound:** `mciSendCommandA` 8 (`waveaudio` device) · `sndPlaySoundA` 5 · `waveOutOpen` 2.
- **Math:** `atan2` 18 · `sin` 5 · `cos` 5 · `sqrt` 3 · `asin`/`acos` 2 · `_ftol` 19.
- **Outside single-player (becomes an empty function in phase 1):** DDE (`DdeClientTransaction` 21, `NDDEAPI.DLL`, `NETDDE.EXE`: network game), `DPLAYX` #1/#2, `WinHelpA`, `DragAcceptFiles`, `WinExec`, `LoadLibraryA`.

Compared with CM 01/02 (≈9,400 functions, DirectDraw/DirectSound), GPM2 has
a quarter of the code and a smaller, older Windows surface.

## 3. What runs where

| Stage | Where | Why |
|---|---|---|
| Recompiler (Python + `capstone`) | **GitHub Actions**, a private development repository | rule of not installing anything locally (brew's `capstone` exists, but without a Python binding). The exe is already in `bin/` |
| Recompiler output (`gen/*.c`, tables) | `recomp-c` artifact of the workflow | downloaded with `gh run download` |
| Game build | **local** (Xcode's clang) and also in CI (`macos-latest`) | local build with tools already present; CI ensures the generated C compiles |
| Differential oracle (Unicorn) | GitHub Actions (already exists) | validates the generated C against the original x86 |

Proposed layout in ``:

```

  recomp/            recompiler (pe.py, disasm.py, cfg.py, translate.py, emit.py)
  recomp/hooks/      declared replacements (address + hash of the original bytes)
  runtime/           machine.h, mem.c, x87.c, dispatch.c, win/*.c, mac/*.m
  tests/             differential and screen tests
  .github/workflows/recomp.yml
```

The local project `GPM2-Mac/` keeps documentation, dumps and the old Swift package
(§8). If the volume of code grows, the runtime may become its own repository
later. **The exe and the game files never go to a public repository.**

## 4. Architecture

### 4.1 Recompiler

1. **PE reading** (`pe.py`): sections, imports (IAT → name), `.reloc`,
   `.rsrc`. The code of the existing map (PLAN-002) serves as a base.
2. **Code discovery** (`cfg.py`): starts at the entries (entry point,
   `call` targets, pointers into `.text` coming from `.reloc`, 143 indirect
   targets) and follows the flows. The 325 `switch` tables are recognized by the
   pattern `jmp [4*reg + T]` and by `T` being a relocation target. The table bytes
   are data and are never disassembled. Function boundaries are checked
   against Ghidra (`ghidra/functions.csv`); any difference is listed in the log.
3. **Translation** (`translate.py`): one C function per x86 function, one label per basic
   block, `goto` for jumps. Each instruction becomes C over the machine state:
   ```c
   // 0x0042BF70
   void f_0042bf70(Cpu *c) {
   L_42bf70: PUSH(c, c->ebp); c->ebp = c->esp; ...
   L_42bf7a: c->eax = R32(c->ebp + 8); ...
   }
   ```
   - **Flags:** per-function liveness analysis. It only computes flags that some
     `jcc`/`setcc`/`adc`/`sbb`/`pushf` will read; the rest is discarded.
   - **Calls:** a direct `call` becomes a C call. An indirect `call`/`jmp` becomes
     `dispatch(c, target)` (sorted table of all entries; an unknown
     target aborts with the address). A call to an import becomes
     `imp_USER32_SendMessageA(c)`, which reads the arguments from the guest stack and
     applies the convention (`stdcall` removes the arguments; `cdecl` does not).
   - **`switch`:** becomes `switch (index) { case k: goto L_target; }` with the
     targets read from the table at recompilation time.
   - **Mnemonics:** the recompiler's first pass inventories the mnemonics
     used in `.text`. A mnemonic without a translation **stops the build** and is not
     ignored.
4. **Emission** (`emit.py`): `gen/f_XXXX.c` files grouped by address range,
   `gen/dispatch_table.c`, `gen/data_init.c` (how `.data`/`.rdata`
   reach guest memory) and `gen/manifest.json` (functions, SHA-256 of each
   body, recompiler version).

### 4.2 Machine model (runtime)

- **Flat 32-bit guest memory:** one host block
  (`mmap`, ≈64 MB) and `guest_ptr = base + addr`. The ranges are:
  - stack: 0x00100000–0x003FFFFF;
  - image: 0x400000–0x76C000, with `.data`/`.rdata` copied **from the user's exe** at run time and `.bss` zeroed;
  - guest heap: from 0x01000000, for `malloc`/`calloc`/`GlobalAlloc`.

  The addresses stay the same as in the original, so the oracle dumps and the
  FMT-016/017 maps remain valid.
- **Registers:** `struct Cpu { uint32_t eax..edi, esp, ebp; flags; x87; }`.
- **Strings and structures:** the host always accesses guest memory (`R8/R16/R32`,
  `W8/W16/W32`, little-endian, unaligned access allowed via `memcpy`).
- **x87:** stack of 8 `double`. The game calls `_controlfp`; the effective
  precision (53 or 64 bits) is measured in the oracle (§5, F2) before fixing the model. It is only
  about 360 instructions, so if 80 bits must be emulated the cost is low.
- **Host → guest callbacks:** WndProc, DlgProc, hooks (`SetWindowsHookA`),
  timers and the `qsort` comparator. The function `guest_call(addr, nargs, ...)` pushes the
  arguments and a sentinel return address and calls `dispatch`.
- **SEH:** there is only 1 use of `_except_handler3` (the CRT startup); it stays empty until
  a test shows that some function depends on it.
- **Win32 handles:** `HWND`, `HDC`, `HBITMAP` etc. are small integers that
  index host tables; the guest never dereferences them.

### 4.3 Windows runtime (replacements)

| Module | Implementation | Notes |
|---|---|---|
| `msvcrt20.c` | native in C | `rand`/`srand` = MSVC LCG (already validated in Swift); `sprintf` with MSVC formatting (`%g`, rounding); `qsort` **with the same algorithm as MSVCRT20** (the order of equal elements changes the result); `time`/`localtime` with a fixable time for tests |
| `kernel32.c` | native | `_lopen/_lread/_lwrite/_hread` over the game folder, with DOS path mapping (`C:\GPM2\...`, case-insensitive); `Get/WritePrivateProfileStringA` in its own `.ini`; `GetTickCount`, `Sleep` |
| `user32.c` | own window manager | message queue, `SendMessage`/`PostMessage`, timers, focus, mouse capture, cursors; **dialogs built from the 22 DIALOG templates of `.rsrc`** with Win95 controls (button, check, radio, static, edit, listbox, combobox) drawn in the framebuffer; menus from the 4 MENU; `LoadStringA` from `.rsrc` |
| `gdi32.c` | own rasterizer, 8-bit indexed | DC, palette, pen/brush, `BitBlt`, `Polygon`, `Pie`, `LineTo`, text (`TextOutA`/`DrawTextA`/`ExtTextOutA`) with bitmap fonts (§6, risk 2) |
| `wing32.c` | 8-bit DIB | `WinGCreateBitmap` gives a buffer in guest memory; `WinGBitBlt/StretchBlt` copy to the window surface |
| `winmm.c` | Core Audio | `waveOut`, `sndPlaySoundA` (WAV), `mciSendCommandA` for `waveaudio` |
| `comdlg32.c`, `shell32.c` | AppKit (`NSOpenPanel`/`NSSavePanel`) or the own Win95 dialog | decide by fidelity (§9) |
| DDE, `DPLAYX`, `WinHelpA`, `WinExec`, `LoadLibraryA` | empty functions with log | network game stays out until the single-player game is finished |
| `mac/*.m` | AppKit | one `NSWindow`; the 640×480 8-bit guest screen (resolution to be confirmed) is converted to RGBA with the current palette and presented with integer scaling and nearest neighbor; mouse and keyboard become `WM_*` |

### 4.4 App

- Universal `GPM2.app` (arm64 + x86_64), without Wine and without external dependencies.
- On first use the app asks for the installed GPM2 folder. It checks the SHA-256 of
  the 1.02B `Gpm2.exe` (like GP4) and refuses other versions with a clear message.
- Saves and `.ini` live in `~/Library/Application Support/GPM2/`. The original
  files are read and never altered.

## 5. Phases and acceptance criteria

Each phase ends with a green workflow and an entry in section 10.

| Phase | Deliverable | Acceptance |
|---|---|---|
| **F0 — Base** | `recomp/` + `recomp.yml` (Python 3.12, `pip install capstone` on the runner); mnemonic inventory and function list | report published; differences against Ghidra explained |
| **F1 — Full translation** | generated C for 100% of the functions; compiles with `clang -O2` on `macos-latest` | no mnemonic without translation; no `switch` table without resolution; green build |
| **F2 — Equivalence** | headless executor (`gpm2_headless`) that runs isolated functions over a memory snapshot | recompiled `FUN_0042bf70` (boot) produces **the same bytes** as the oracle (`oracle-boot`) and as `BootTests`; same comparison for `FUN_0043c3a0`, `FUN_004f1b80` and the `staff`/`newcareer` scenarios; x87 precision measured |
| **F3 — Startup** | `WinMain` runs up to the message loop; ordered log of each import called | no import without implementation on the boot path; RNG and files read match the oracle |
| **F4 — First screen** | AppKit window, WinG, palette, mouse | main menu **pixel-identical** to a reference capture (to be obtained, §9) |
| **F5 — Dialogs and text** | `user32` (dialogs/menus/controls), GDI text | new career flow → first save; with a fixed time, the generated save is **byte-for-byte equal** to the oracle's/reference emulation's; the 3 real saves load and show the same screens |
| **F6 — Weekend and race** | timers, aerial race view, sound, FLI | full season playable; deterministic results with a fixed time |
| **F7 — App** | universal bundle, folder choice, SHA-256 check, Application Support | clean install on a Mac with nothing but the app and the game folder |
| **F8 — Bugs** | fix hooks (§7), one per bug in `docs/bugs/` | test for each bug showing the original behavior (hook off) and the corrected one |

Phases F1–F3 do not depend on an interface and can be validated entirely in CI.

## 6. Risks

1. **Math functions:** `sin/cos/atan2/sqrt` from MSVCRT20 use x87 and
   may differ from macOS's libm in the last bit. **Mitigation:** compare in the
   oracle for the real recorded arguments; if they diverge, implement as
   MSVCRT20 does (80-bit x87).
2. **Windows fonts:** the dialogs use the system font (MS Sans Serif /
   System) and the game creates fonts with `CreateFontA`. Microsoft fonts
   cannot ship in the app. **Mitigation:** read the `.FON` files from the user's Windows installation
   or recreate the bitmaps from captures (FMT-014 already covers the game's own fonts).
3. **Appearance of Win95 controls:** 3D borders, `GetSysColor` colors and
   dialog metrics (`GetDialogBaseUnits`) must match pixel for pixel.
   **Mitigation:** reference captures from a real Windows 95/98 or from a VM
   outside the Mac, used only as a template.
4. **Self-modifying code or computed calls outside the pattern:** nothing
   found so far; the recompiler aborts if an indirect target is not in
   the table, so any case shows up in the tests.
5. **Time dependence:** `srand(time())` makes games differ; the
   tests use a fixed time. This is already solved for the oracle (seed
   `0x63D6BD17`).
6. **License:** the recompiler and the runtime are **written from scratch**. The CM 01/02
   project (GPLv3) is studied as an architecture reference; no code
   from it is copied. If we later want to reuse code from it, the project
   becomes GPLv3 (user's decision).

## 7. Bug fixes (hooks)

```c
// recomp/hooks/bug023_fastest_lap.c
HOOK(0x004XXXXX, "sha256-of-the-original-bytes", bug023_fastest_lap);
```

- The recompiler checks the hash of the original function's bytes before swapping
  the call; if it does not match, **the build stops**.
- Each hook has an option in `settings.ini` (default: fix on) and a test
  with both behaviors, fulfilling rule 5 of PLAN-000.
- The documentation of each bug in `docs/bugs/` gains the hook's address.

## 8. Effect on existing work

| Item | Fate |
|---|---|
| Format docs (FMT-001...018), inventory, exe map, bug lists | **kept**; they serve to locate bugs and write hooks |
| Unicorn oracle (`oracle/`) | **kept**; becomes the F2 template |
| Ghidra (`gpm2-102b-decompiled`) | **kept** for reading and for checking function boundaries |
| Swift package `app/GPM2Core` (37 tests) | **frozen**; the boot/selection tests become comparison data for F2. It receives no new transliterations |
| Open items of PLAN-008 and PLAN-010 (18 `rand()`, 282 bytes) | **canceled**: the recompilation runs the real code, so the divergence disappears without hunting for consumers |
| `AGENTS.md` | language restriction updated (Swift is no longer mandatory; native macOS) |

## 9. User pending items

1. **Reference captures:** screens of the original game running on a real
   Windows/VM (main menu, a dialog, the race view), at native resolution,
   unscaled. Needed for F4/F5.
2. **File dialogs:** use the macOS panel (more practical) or recreate
   the Windows 95 dialog (100% faithful). Proposed default: recreate the Win95 one, to
   follow the identical-UI rule.
3. **Windows system fonts:** confirm whether there is a Windows installation
   from which to read the `.FON` files (risk 2).

## 10. Log

- 2026-11-02 — plan created and adopted; recompilation on GitHub Actions; language
  free as long as it is native to macOS.
- 2026-11-02 — §9.2 decided: **`GetOpenFileNameA`/`GetSaveFileNameA` use macOS's
  `NSOpenPanel`/`NSSavePanel`.** §9.1: reference captures come from an
  internet search (they serve to check layout; for pixel-by-pixel comparison
  they are only valid if at native resolution and without lossy compression).
  §9.3: the user has a Windows 98 from which `C:\WINDOWS\FONTS` can be extracted.
- 2026-11-02 — **where the game uses Windows fonts** (static survey):
  - the game's text (screens, buttons, tables) is drawn with its own bitmap fonts
    (`FONT15/FONTGSM/FONTGLG/HANDFONT/THINFONT.EDY`, FMT-014) in the
    WinG framebuffer, without GDI;
  - the 22 DIALOGs use `MS Sans Serif` 8. In single-player only
    `EDITAMOUNT` ("Change Details") appears. The others are debug ones (`CHEATING`,
    `DRIVER_EDIT`, `TRACK_EDIT`, `RACE`, `DebugWindow - Setup`) or network ones
    (101–107, 112, 134–140);
  - `CreateFontA("MS Sans Serif", 8)` at `0x5053E0`, a WndProc registered at
    `0x504EF7`. Together with the rest of the GDI text (`DrawTextA`/`TextOutA`/
    `GetTextExtentPoint32A` at `0x505AC0`, `0x507650`, `0x5079B0`, `0x50A1D0`)
    it belongs to the network chat/lobby windows (`ChatClass`, `CMBClass`,
    `NullClass`, `myedit`);
  - `0x4F9150`/`0x4F93C0` (`GetTextMetricsA`/`ExtTextOutA`) = "Debug Win" window;
  - `MessageBoxA`: 20 calls (errors, confirmations) with the system font.

  **Conclusion:** in single-player, the Windows font only appears in `MessageBoxA` and
  in the "Change Details" dialog. Extracting `SSERIFE.FON` (MS Sans Serif) from Win98
  remains the most faithful path, but it is a small, localized risk.
  It remains to confirm at run time, in F3, that no other screen goes through GDI.
- 2026-11-02 — **user decision: "whatever is Windows becomes whatever is
  macOS".** The 100% identical UI rule applies to what the *game* draws (the client
  area). Everything that Windows draws is replaced by the native equivalent:

  | Windows | macOS |
  |---|---|
  | blue title bar, borders, min/max buttons | standard `NSWindow` |
  | Win32 menu bar (`LoadMenuA`, 4 MENU: "File/Options/Help"; the items on the right "Next Race: …" and "$…" are menu items changed with `InsertMenuA`/`ModifyMenuA`) | app menu in the macOS menu bar, built from the MENUs of `.rsrc` and the game's menu calls. Where the informational items will appear is left for F5 |
  | `MessageBoxA` | `NSAlert` |
  | DIALOG dialogs (`EDITAMOUNT` etc.) | native AppKit panel, generated from the template; the game's `DlgProc` keeps receiving the equivalent Win32 messages |
  | `GetOpenFileNameA`/`GetSaveFileNameA` | `NSOpenPanel`/`NSSavePanel` |
  | system cursors | macOS cursors (the game's own cursors remain) |

  **Consequences:** (1) §9.3 canceled, because **we do not need the Windows
  fonts**; (2) risk 2 and risk 3 of §6 drop to whatever GDI remains in the client
  area (nothing, according to the survey above); (3) the Mac window has the
  size of the game's client area. From the captures, the area below the title
  bar is 640×458 including the menu bar; without it should be close to
  640×438. **The exact value comes from `CreateWindowExA`/`GetClientRect` in F3.**
- 2026-11-02 — reference captures from the internet in
  `extracted/reference-screens/web/` (catalog in `SOURCES.md`).
- 2026-11-02 (session 5) — **User decision: visual acceptance is theirs.** The user personally checks pixel-by-pixel fidelity; the reference captures (§9.1) are no longer needed. The Windows fonts and the file dialog (§9.2–3) had already fallen with the decision for native macOS elements.
  - **Simplified roadmap:** since the original code runs in full and draws with the original files, there is no separate work for screens, race or video. The remaining work is implementing the 259 imported APIs, in the order in which the game calls them:
    - USER32 95, MSVCRT20 81, KERNEL32 33, GDI32 32, WING32 7, WINMM 5, others 6;
    - the whole image goes through WING32 + palette;
    - dialogs are translated generically from the exe's templates.
  - **Order:** close F1 → F3/F4 together (run, stop at the first missing import, implement, repeat until the menu appears) → career, race and save → bug hooks.
  - F2 stays as a cheap safety net, not a blocker.
- 2026-11-02 (session 5) — **Design note: future PowerPC portability** (user request: evaluate, at the end of the port, a build for an iBook G4 with Mac OS X Tiger/Leopard and, if possible, Mac OS 9). This is not a phase now. These are cheap constraints so as not to close the door:
  1. **Portable C:**
     - the generated C and the runtime stay in C99 compilable by GCC 4.0/4.2 (Xcode 2.5/3.1);
     - no C11 (`_Static_assert`, `_Atomic`, `_Generic`), no builtins later than GCC 4.0 and no blocks;
     - no dependency on clang-specific `#pragma`/attributes without an alternative.
  2. **Byte order:**
     - all guest memory access goes through `R8/R16/R32/W8/W16/W32` (already the case);
     - the host byte order becomes a compile-time choice (byte swap on a big-endian host), in place of the current little-endian assertion;
     - no code outside those macros may read guest memory as native `uint16_t`/`uint32_t`.
  3. **Runtime Windows layers:**
     - **portable core in C:** Win32 API semantics, message queue, palette, files, dialog templates;
     - **thin platform backend:** window, framebuffer blit, sound, timer, menus, panels;
     - current backend: modern AppKit; possible future ones: Tiger/Leopard AppKit (Objective-C 1.0/2.0) and Carbon/Toolbox for OS 9.
  4. **Configurable resources:**
     - the guest memory size (today 64 MB) and the host stack are parameters, to fit in the iBook's RAM;
     - no 1 GB stack outside the test driver.
  5. **Performance:** it is the main risk on the G4, because of x86 flags computed in software. The planned optimization is to eliminate computing flags that no instruction reads; it serves both targets and is evaluated when the modern port is playable.
  6. **Build:**
     - current Xcode does not generate PowerPC;
     - the options are to compile on the iBook itself (Xcode 2.5/3.1) or a cross PowerPC GCC on GitHub Actions; nothing is installed on the user's Mac;
     - OS 9 would require CodeWarrior/MPW (evaluate later).
  - For the PowerPC work, use the `retro-os-dev` skill (historical constraints of Tiger/Leopard/OS 9).
