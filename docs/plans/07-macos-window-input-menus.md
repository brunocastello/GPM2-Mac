# M2: macOS window, input and native menus

**Parent:** 05-win32-runtime-boot (M1 accepted: the headless `gpm2run` reaches the "Career Options" menu).
**Date:** 2026-11-02..

## Goal

The game runs in a **real macOS window**, with mouse, keyboard and the native
menu bar. From here on, the user can open and play as far as the
dialogs and sound (M3) allow.

**Out of scope:** native `DIALOG` dialogs and audio (M3). In this plan they
stay with a log and a default value. `MessageBoxA` is in (it is simple).

## Constraints (those of 05-win32-runtime-boot, plus these)

- **Build:** no Xcode project. `runtime/Makefile` assembles `build/GPM2.app`
  with `clang` (Objective-C, `-framework AppKit -framework QuartzCore`), a generated
  `Info.plist` and the binary in `Contents/MacOS/GPM2`. Nothing installed.
- **Backend architecture:** the `runtime/win32/` core **does not** include
  Apple headers. All Objective-C lives in `runtime/host/macos/`, behind
  `runtime/host/host.h`, which keeps the core portable (PowerPC note).
  - The headless backend keeps working, because it is the basis of the automated
    tests.
- **Threads:**
  - AppKit stays on the **main thread**;
  - the guest runs on a pthread with a large stack (256 MB, as in M1);
  - **events:** thread-safe host → core queue;
  - **frames:** `present()` copies the 8-bit frame and the palette under a lock and notifies
    the main thread (`performSelectorOnMainThread:` or
    `dispatch_async(dispatch_get_main_queue())` in the modern backend);
  - **blocking:** `GetMessageA`/`WaitMessage` block on a condition variable
    until an event arrives or the next timer expires;
  - **calls that need an answer from the interface** (`MessageBoxA`, menus):
    the guest posts a request to the main thread and waits for the answer.
- **Real time** by default (`mach_absolute_time`). `--fixed-time` continues
  to exist for the headless tests.
- **Game files:** on first use, the app asks for the GPM2 folder with
  `NSOpenPanel` and stores the choice in `NSUserDefaults`. Then it looks for the
  1.02B `Gpm2.exe` (SHA-256 `266f2147…3546`) in that folder.
  - If the folder's exe is another one, it asks for the 1.02B exe with a second panel.
    In the user's installation, the 1.02B is in `GPM2 Updates v1.02b/`.
  - The writable folder is `~/Library/Application Support/GPM2/`.
  - The `--game`, `--exe` and `--write-dir` command-line arguments take
    precedence, for development.
  - **Never write to the game folder.**

## Window

- One `NSWindow` titled "Grand Prix Manager 2", **content area of
  640×460 game pixels**:
  - the original has a Win32 menu bar (`SM_CYMENU` = 19) and a 640×460 client area
    (see the last item of the 05-win32-runtime-boot Log);
  - **fix the core:** a window with a menu (`hMenu` in `CreateWindowExA`, class
    menu or `SetMenu`) subtracts `GetSystemMetrics(SM_CYMENU)` = 19 from the
    client area.
  - Confirm with the headless that the black strip at the bottom is gone (640×460 frame).
- **Integer scaling with nearest neighbor:**
  - default 2× (1280×920 points);
  - a "View" menu with 1×/2×/3× and "Enter Full Screen" (macOS full screen,
    with centered integer scaling and black borders);
  - the choice is stored;
  - the drawing is an RGBA `CGImage` converted from the 8-bit frame with the current
    palette, in a `CALayer` with `magnificationFilter = kCAFilterNearest`, or
    `drawRect:` with interpolation turned off.
- **Consistent Win32 coordinates:** the window lives on a 640×480
  virtual desktop.
  - `GetWindowRect`/`GetClientRect`/`ClientToScreen`/`ScreenToClient`/`GetCursorPos`
    use the same model as M1, with the menu subtracted.
  - The mouse position on the host is converted to client coordinates
    (0..639, 0..459) by dividing by the scale.
- **Closing:**
  - the red button sends `WM_CLOSE` to the game's WndProc (the game decides);
  - the guest's `ExitProcess`/`exit` ends the app;
  - Cmd-Q goes through the same path as `WM_CLOSE`.

## Input

- **Mouse:**
  - `WM_MOUSEMOVE`, `WM_LBUTTONDOWN/UP`, `WM_RBUTTONDOWN/UP` (right button and
    Ctrl+click), with `wParam` (`MK_LBUTTON`/`MK_RBUTTON`/`MK_SHIFT`/`MK_CONTROL`)
    and `lParam` = client coordinates;
  - `WM_LBUTTONDBLCLK` only if the class has `CS_DBLCLKS`, with Windows' double-click
    time (500 ms) and the same position tolerance;
  - `SetCapture`/`ReleaseCapture`: with capture, events keep going to the
    capturing window even outside it;
  - `GetAsyncKeyState(VK_LBUTTON)` etc. reflect the real state.
- **Cursor:**
  - `SetCursor`/`LoadCursorA` with the system cursors (`IDC_ARROW`,
    `IDC_WAIT`, `IDC_IBEAM`...) mapped to `NSCursor`;
  - **cursors from `.rsrc`** converted into `NSCursor` (image + hotspot, scaled
    by the window scale);
  - `ShowCursor` with a counter.
- **Keyboard:**
  - `keyDown`/`keyUp` → `WM_KEYDOWN`/`WM_KEYUP`, with a virtual-key table
    (letters, digits, F1–F12, arrows, Enter, Esc, Tab, Backspace, Delete,
    Home/End/PgUp/PgDn, Shift/Ctrl/Alt via `flagsChanged`) and `lParam` with
    repeat count and repeat flag;
  - `TranslateMessage` generates `WM_CHAR` from the event's characters (ASCII and
    Latin-1/Windows-1252 for accents);
  - `GetKeyState`/`GetAsyncKeyState` keep the state;
  - Alt/Option + key → `WM_SYSKEYDOWN`;
  - Cmd is reserved for macOS (menus).
- **Accelerators:** real `LoadAcceleratorsA`/`TranslateAcceleratorA`, reading the
  table from `.rsrc` and generating `WM_COMMAND`.

## Menus (Win32 → macOS bar)

- **Full menu model in the core:** `LoadMenuA` (from `.rsrc`),
  `CreateMenu`/`CreatePopupMenu`, `AppendMenuA`, `InsertMenuA` (by position and
  by command), `ModifyMenuA`, `DeleteMenu`/`RemoveMenu` (if imported),
  `EnableMenuItem`, `CheckMenuItem`, `GetMenu`, `GetSubMenu`, `SetMenu` and
  `DrawMenuBar`.
  - Everything in handle tables, with the exact flags (`MF_GRAYED`,
    `MF_DISABLED`, `MF_CHECKED`, `MF_SEPARATOR`, `MF_POPUP`, `MF_BYPOSITION`,
    `MF_BYCOMMAND`, `MF_STRING`, `MF_OWNERDRAW`, if they occur).
  - Check in the decompilation how the game builds the bar (the "Next Race: …" items and
    the money are updated with `ModifyMenuA`) and document it.
- **Mirror on macOS:**
  - the main window's menu becomes the app's menu bar, after the "GPM2"
    application menu (About GPM2, Hide, Quit);
  - a top-level **popup** becomes an `NSMenu` with the same title, without the `&`;
  - a **top-level command item** (no popup, like "Next Race: Melbourne" and
    the money) becomes an `NSMenu` whose title is the item's, containing **one** item with the
    same text, which fires the command. A gray item becomes a disabled item;
  - the mirror is rebuilt at each `DrawMenuBar`/`SetMenu`/`ModifyMenuA` on the bar;
  - selecting an item posts `WM_COMMAND` (`wParam` = id) to the guest's queue;
  - `WM_INITMENU`/`WM_INITMENUPOPUP`: implement only if the game handles them
    (check in the decompiled WndProc). If it does, send them synchronously
    before opening the `NSMenu` (`menuNeedsUpdate:`, waiting for the guest to process)
    and mirror the result.
- **Shortcuts:** `&X` shows the underlined letter on Windows; on macOS ignore it.
  The game's accelerators continue through the keyboard.

## MessageBoxA

Modal `NSAlert` on the main thread, with the game's text and title.
- Buttons per `MB_OK`/`MB_OKCANCEL`/`MB_YESNO`/`MB_YESNOCANCEL`/`MB_RETRYCANCEL`
  and icon per `MB_ICON*`.
- Returns `IDOK`/`IDCANCEL`/`IDYES`/`IDNO`/`IDRETRY`.
- The guest waits.

## Automated tests (headless with a script)

`gpm2run --script script.txt` injects events and allows reaching screens without an
interface. Simple format, one action per line:

- `wait-idle` — waits for the game to go idle;
- `wait-frames N` — waits for N frames;
- `click X Y` — left button at client coordinates;
- `rclick X Y`;
- `key VK` — virtual key;
- `type text`;
- `menu ID` — `WM_COMMAND`;
- `snap name` — saves the current frame under the given name.

Acceptance script `tests/scripts/quickstart.txt`:
1. wait for the "Career Options" menu;
2. `snap career`;
3. click **Quick Start** (center of the button in the M1 frame: about x=130, y=127);
4. wait for idleness;
5. `snap after-quickstart`.

Document where it stopped: next screen, dialog (logged) or import.

## Execution

1. Fix the menu subtraction in the client area and check it in the headless (640×460).
2. Headless script + menus in the core (testable in the headless with `menu ID`).
3. macOS backend: window, presentation, scale, mouse, keyboard, cursor, menus,
   `MessageBoxA`, game-folder panel and `GPM2.app`.
4. Run `GPM2.app` locally. You can launch it with `open build/GPM2.app --args
   --game … --exe …` to test whether it opens and does not abort in the first seconds;
   the log goes to `~/Library/Logs/GPM2/run.log`.
   - Do not depend on inspecting the window: visual acceptance is the user's.
   - `screencapture` requires permission and must not be used.
5. Small commits, in Portuguese, with the `Co-Authored-By`. Keep CI green.
   The `f1-build-macos` job now also builds `GPM2.app` (without running it).
6. Update `docs/plans/06-win32-imports.md` (state of the menu, cursor,
   keyboard and mouse imports) and write the Log below.

## M2 acceptance

- Headless:
  - 640×460 frames, without the black strip;
  - the `quickstart.txt` script moves from the "Career Options" menu to the next
    screen, or to the next point that depends on a native dialog (M3),
    documented.
- `build/GPM2.app` builds and opens, shows the intro and the menu in a 2× window,
  accepts clicks and shows the native menu bar mirroring the game's.
- **Visual and playability acceptance is the user's.** The lead checks the code
  and the headless frames.
- Nothing written to the game folder. `make -C runtime fuzz` and CI stay
  green.

## Log

- 2026-11-02 — specification created (session 5).
- 2026-11-02 (session 5) — **M2 implemented** (old development repo `48d840d`, `adfa9a2` + README adjustment). Visual and
  playability acceptance: the user's.
  - **Discovery that corrects the spec's premise: the game does NOT have a Win32 menu bar.**
    - "File Options Help", the icons, "Next Race: Melbourne" and the money are drawn by the game itself in the
      640x460 frame (game font and bevel; even the "File" popup opens drawn by it: `extracted/dumps/run-m2/game-file-menu.png`).
      There are no `SetMenu`, `CreateMenu`, `DrawMenuBar` nor `TrackPopupMenu` in the exe's imports, and the class has `lpszMenuName = 0`.
    - The game builds the menus in the window's **system menu** (the icon menu/Alt+Space), in `FUN_00523150`
      (called before several screens, only if `DAT_0057b2ee == 0`):
      `GetSystemMenu(hwnd,1)` (reverts); `m = GetSystemMenu(hwnd,0)`; `InsertMenuA(m,6,MF_BYPOSITION|MF_SEPARATOR)`;
      then, each one with `InsertMenuA(m,6,MF_BYPOSITION|MF_POPUP|MF_STRING = 0x410, LoadMenuA(hinst,id), LoadStringA(...))`,
      in the order Help (RT_MENU 0x84, string 0x2524), Options (0x81, 0x2523), File (0x83, 0x2522). Since each insertion is at
      position 6, the result is: 0 Restore, 1 Move, 2 Size, 3 Minimize, 4 Maximize, 5 separator, **6 File, 7 Options, 8 Help**,
      9 separator, 10 Close.
      - File = Open 40023, Save 40024, Save As 40025, ---, Restart 40020, Exit 40021 (0x9C57..0x9C55);
      - Options = Sound 40011, Race Screen 40012, News After Race 40013 (0x9C4B..0x9C4D);
      - Help = Help 40030, Credits 40031, About 40029.
    - `FUN_005232f0`/`FUN_00523390`: `GetSubMenu(sys,7)` and `(sys,8)` + `EnableMenuItem(.., id, 1 or 0)`
      (grays/enables Options and Help; the id 0x9C4F does not exist in the resources);
      `FUN_00523290`: `ModifyMenuA(sys, 7, 0x401 = BYPOSITION|GRAYED, 0, "Options")`, that is, the Options popup becomes gray text.
      The 1.02B also uses `GetSystemMenu` for the cheat "Click Me For A Cheat" (`AppendMenuA(sys,..,100,..)`) and for the chat
      window (`ChatClass`: `InsertMenuA(sys,6,0x400,1,...)`).
    - The commands arrive through **`WM_SYSCOMMAND`** (wParam = id), handled in the WndProc `0x00401710` (`FUN_00402590` -> `FUN_00523440`,
      the dispatcher of ids 0x9C4B..0x9C5F). **`WM_COMMAND` is not handled** and neither are `WM_INITMENU/WM_INITMENUPOPUP`.
    - There are no accelerators or cursors in `.rsrc` (only DIALOG, MENU, STRING, ICON); only system cursors (`IDC_ARROW`, `IDC_WAIT`).
  - **640x460 client area:** top-level window without `WS_POPUP`/`WS_CHILD` and without caption bits (style 0xA0000) = overlapped, with a
    19 px caption; the border is only subtracted in the caption (real Win95 would cut 2 columns/lines). `GetClientRect` = 640x460, the
    headless frames are 640x460, without the black strip. A window with a Win32 menu bar subtracts 19 px more (`menu_has_bar`);
    the game does not use this.
  - **What was built (runtime):**
    - `win32/menu.c`: full menu model (handles, flags, `LoadMenuA` from `.rsrc`, `Create*/Append/Insert/Modify/Delete/Remove/
      EnableMenuItem/CheckMenuItem/GetSubMenu/GetMenu/SetMenu/DrawMenuBar`, per-window system menu) and the mirror to the host
      (`menu_sync`, only when something changed): popups of the system menu (without the default `SC_*` items) become top-level menus, top-level
      commands become one-item menus; items fire `WM_SYSCOMMAND`. If the window has a real menu bar, the mirror uses the bar
      and `WM_COMMAND`. Tested by `tests/menu_test.c` (replays the game's calls; `make -C runtime menutest`).
    - `win32/input.c` + `user32.c`: thread-safe host->core queue; mouse (`WM_MOUSEMOVE`, left/right/middle buttons, `MK_*`, capture,
      double click only with `CS_DBLCLKS`: 500 ms/4 px), `WM_SETCURSOR` before each `WM_MOUSEMOVE`, keyboard (`WM_KEYDOWN/UP`,
      `WM_SYSKEYDOWN/UP` with Alt, `lParam` with repeat/scan/extended, `TranslateMessage` -> `WM_CHAR` with the host's character,
      stored in `pt.x`), `GetKeyState`/`GetAsyncKeyState`, `GetCursorPos`/`ClientToScreen`/`ScreenToClient`, host `WM_CLOSE`,
      `WaitMessage` sleeping on a condition until the next timer/event.
    - `host/host.h` (new interface), `host/headless.c` (script, menus in `out/menu.txt`, `MessageBoxA` with default) and
      `host/macos/host_mac.m` (AppKit: 2x `NSWindow` with integer scaling 1/2/3 (View, Cmd-1/2/3, resizes in multiples), centered native
      full screen with black borders, `CALayer` with `kCAFilterNearest`, `NSCursor`, keyboard/mouse -> `HostEvent`,
      Ctrl+click = right button, Cmd reserved for macOS, `NSAlert` for `MessageBoxA` (buttons per `MB_*`, icon per `MB_ICON*`),
      GPM2 app menu (About/Hide/Quit; Cmd-Q and the red button send `WM_CLOSE` to the game, which decides), the game's menus right
      after, `NSOpenPanel` for the game folder and for the 1.02B exe (SHA-256 checked, choice in `NSUserDefaults`), data in
      `~/Library/Application Support/GPM2/`, log in `~/Library/Logs/GPM2/run.log`).
    - `Makefile`: targets `app` (assembles `build/GPM2.app` + `Info.plist` + ad hoc signature, without Xcode) and `menutest`; CI
      (`f1-build-macos`) now assembles the app without running it. The core still has no Apple headers.
  - **Script `tests/scripts/quickstart.txt`** (`--fixed-time 851990400`, deterministic; also passes with `GUEST_CHECK`):
    `wait-idle 150` -> `snap career` (CAREER OPTIONS, frame 84) -> `click 130 127` (Quick Start) -> `wait-idle 150` ->
    `snap after-quickstart` = **SELECT DIFFICULTY** screen (Rookie..Ace, Cancel/OK). It did not stop: a longer script (click
    Rookie, OK, OK) reaches **QUICK START OPTIONS: Benetton** and then the **main game screen** (Personnel, Sponsors,
    Finance, ... with the "menu bar" drawn by the game, "Next Race: Melbourne $20,000,000"), with no native dialog on the way.
    Frames: `extracted/dumps/run-m2/` (`career.png`, `after-quickstart.png`, `main-screen.png`, `game-file-menu.png`).
    `wait-idle 150` is necessary: with 60 the script fires during the intervals between logo frames.
  - **Real time:** the first time the logo/intro takes ~20 s to reach Career Options (the game's own 3 s and 5 s timers, plus the
    100 ms animations); it is not a freeze. The app sits idle at ~0.1% CPU in the menu.
  - **Still stub / pending (M3 and later):** audio (`sndPlaySoundA`, `waveOut*`, `mciSendCommandA`, `MessageBeep` only log),
    dialogs (`DialogBoxParamA`, `CreateDialogParamA`, `GetOpenFileNameA`/`GetSaveFileNameA` return the default; child `EDIT`/`LISTBOX`
    use the host's DefWindowProc and do not receive the keyboard), `DragAcceptFiles`, DDE/DPLAY, `WinHelpA`, hooks. Focus
    (`WM_ACTIVATEAPP` when switching apps) is not sent to the game. `SetMenu` does not readjust the client area. No app icon (`.icns`).
    The native-menu mirror only appears when the game builds the system menu (`FUN_00523150`): this did not occur on the script's path
    to the main screen, so the macOS bar shows only GPM2/View/Window in that stretch; the File/Options/Help menus
    appear when the game builds them (tested in `menu_test` and in the headless log `menu.txt`, not visually in the app).
  - **How to open:** `open runtime/build/GPM2.app --args --game "<original/GPM2>" --exe "<original/GPM2 Updates v1.02b/Gpm2.exe>"`
    (without arguments the app asks for the folders). Build: `make -C runtime app GEN=<gen>`.
  - **Translator:** no suspected divergence; `RETCHK` clean in `run-chk` with input. The trace now prints `t=<ms>` per call.
