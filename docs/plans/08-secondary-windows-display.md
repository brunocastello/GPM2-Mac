# The game's secondary windows and display options

**Parent:** 07-macos-window-input-menus (M2 done). **Date:** 2026-11-02..
Comes before M3 (dialogs and sound), because it touches the same files (`user32.c`,
`host/macos/`).

## 1. Bug: Options → Main Screen (reported by the user)

**Symptom in the app:** a window stretched in height appears, with nothing visible.

**Cause** (headless trace, script Quick Start → Rookie → OK → OK →
Options → Main Screen):

- The game calls `CreateWindowExA(0, "ChildClass", "GPM 2", 0x00C00000 /*WS_CAPTION*/, 0, 0, 0x234, 0x49, hwndOwner=0x10000, ...)`
  (ra `0x004F4F67`), then `ShowWindow(SW_SHOWNORMAL)` and
  `SetWindowPos(hwnd, HWND_TOPMOST, 0,0,0,0, SWP_NOSIZE|SWP_NOMOVE|SWP_NOACTIVATE)`.
- It is an **owned top-level window**, with a title bar, 564×73 in
  total. The client area is a 558×42 icon bar on which the game draws
  through WinG.
- The M2 backend handles only one window: the headless snap shows only the surface
  of that window, and in the app it replaces the main one.

**Fix (user rule: whatever is Windows becomes macOS):**

1. **Core:** real support for several top-level windows:
   - each top-level HWND has its own surface, window rectangle and client
     rectangle;
   - the non-client area is subtracted as in Win32: 18 px caption + borders
     according to the style, including `WS_CAPTION` on an owned window;
   - z-order, including `HWND_TOPMOST`;
   - owner, activation and focus.
   - `present()` now indicates **which window** was updated.
2. **macOS backend:** each secondary top-level window with a caption becomes a
   **native NSPanel** child of the main window (`addChildWindow:ordered:`),
   with the game's title ("GPM 2") and content the size of the client area at the same
   scale as the main window.
   - The position is relative to the main window, converted from the Win32 coordinates.
   - `HWND_TOPMOST` makes the window float over the main one.
   - Mouse and keyboard in the NSPanel generate messages for that HWND.
   - Closing the NSPanel sends `WM_CLOSE` to it.
   - `DestroyWindow` and `ShowWindow(SW_HIDE)` close or hide it.
   - A top-level window without a caption (pure `WS_POPUP`, if it occurs) becomes a borderless
     NSPanel at the same position.
3. **Headless:** composes the visible top-level windows on a 640×480 "desktop" in
   z-order, so the snaps show what the user would see. The snap of the main window
   alone remains available (`snap-window name hwnd`).

**Also check:** the game moves the main window to (−320, −222) with
`MoveWindow` (ra `0x00401F25`). It centers by the screen, and the reported metrics
must be inconsistent.

- Check in the decompilation which calculation it uses: `GetSystemMetrics(SM_CXSCREEN/SM_CYSCREEN)`
  or `GetDeviceCaps(HORZRES/VERTRES)`.
- Make the virtual desktop metrics coherent: 640×480 in 256 colors, as in
  Win95, so the window lands where it would and the secondary windows appear
  at the right relative position.
- Document the values.

**Acceptance of item 1:**
- Headless script `tests/scripts/mainscreen.txt` (the path above, with the
  clicks from the 07-macos-window-input-menus Log): the composite snap shows the "GPM 2" icon bar
  over the main screen, with visible content.
- In the app, the NSPanel appears with the icons. Visual acceptance is the user's.

## 2. Display options (lead's decision, approved by the user as the "best way")

The game draws 640×460, so "resolution" is scale. No aspect correction:
the pixel is square, and 640×480 is 4:3.

**Modes:**

- **Window:**
  - integer scale 1×–N×, with N limited to the current screen;
  - default on first use: the largest integer scale that fits in the screen's usable area;
  - **resizing the window** snaps to integer multiples;
  - with **Option** pressed during the resize, the size is free,
    with locked proportion and the filter of the "Fill" mode.
- **Full screen** (macOS's own, with its own Space), with two modes:
  - **"Pixel perfect"**: largest integer scale, centered, with black borders;
  - **"Fill the screen"**: largest scale keeping the proportion, *sharp
    bilinear* filter:
    - enlarges by nearest neighbor to the largest integer <= scale;
    - then applies bilinear up to the final size;
    - implementation: intermediate texture or two passes with
      `CGContext`/`CALayer`. Choose the simplest that gives the result.
- **Filter in the integer-scale window:** always nearest neighbor.

**Interface (native):**

- **View menu:**
  - "Actual Size (1×)", "2×", "3×"... up to N, with shortcuts Cmd-1…Cmd-4;
  - "Zoom to Fit Screen" (largest integer scale);
  - separator; "Enter Full Screen" (Ctrl-Cmd-F);
  - "Full Screen Mode" submenu with "Pixel Perfect" and "Fill Screen".
- **Preferences** (GPM2 → Settings…, Cmd-,): simple NSPanel with:
  - window mode (integer scale);
  - full-screen mode (Pixel perfect / Fill);
  - "Open in full screen";
  - game folder (show and change; changing requires a restart).
- **Persistence:** `NSUserDefaults` stores the scale, the full-screen mode,
  "open in full screen", the window position (frame autosave) and the game folder.
- **Command line** (development): `--scale N`, `--fullscreen`,
  `--fill`. They take precedence and are not saved.

**The secondary windows** (item 1) follow the main window's scale. In full screen,
they stay over the main one in the same Space.

**Acceptance of item 2:**
- Compiles; the menus and the Preferences exist; the values persist.
- "Fill" uses sharp bilinear.
- Nothing changes in the `win32/` core, which does not include Apple headers.
- Visual acceptance is the user's.

## Constraints

Those of 05-win32-runtime-boot/013:
- nothing installed;
- never write to `original/`;
- do not commit frames, `gen/`, outputs or game files; check
  `git status` before each commit;
- portable C99 core;
- translator untouched;
- CI green;
- commits in Portuguese with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`;
- no `screencapture`.

## Log

- 2026-11-02 — specification created (session 5).
- 2026-11-02 (session 5) — **Implemented** (old development repo `90691af` core + headless, `5ccbd75` macOS backend). Visual acceptance: the user's.
  - **Real cause of the bug (different from the spec's hypothesis).** Besides the backend handling only one window, the main window's `WM_CREATE`
    (WndProc `0x00401710`, case 1) centers with `GetWindowRect(hwnd)` + `GetWindowRect(GetDesktopWindow())` and `MoveWindow`. Our
    `GetWindowRect` of the desktop (`HWND_BASE-4`) returned 0 without filling the rectangle; the game used stack garbage and moved the window to
    (-320, -222) (the trace showed this). **Fix:** the desktop has rectangle (0,0,640,480) (and the desktop's `GetClientRect` returns 1).
    The game computes `x=(right-0x280)>>1`, `y=(bottom-height)>>1` (and subtracts 0x26 only if `right > 0x280`, the taskbar): with 640x480 the
    main window (640x479) ends up at **(0,0)**. Position changed from (-320,-222) to (0,0).
  - **Screen metrics (checked, already coherent):** `SM_CXSCREEN/SM_CYSCREEN` = 640/480 (and `GetDeviceCaps(HORZRES/VERTRES)` = 640/480,
    BITSPIXEL 8, RASTERCAPS with RC_PALETTE). The game uses only the desktop `GetWindowRect` above (not `GetSystemMetrics`) in this calculation.
    Kept: `SM_CYCAPTION` 19, `SM_CXBORDER` 1, `SM_CXDLGFRAME` 3, `SM_CXFULLSCREEN/CYFULLSCREEN` 640/433.
  - **Core (`win32/user32.c`):** each top-level window (without `WS_CHILD`) has a window rectangle, client origin/size (`cx,cy,cw,ch`),
    `bw`/`cap`, z (`HWND_TOPMOST/NOTOPMOST/TOP/BOTTOM`, after-another, `BringWindowToTop`, a shown window goes on top), topmost flag, owner
    (`hwndParent` of a non-child window). The main window is the first top-level one without an owner. A child window (`WS_CHILD`) computes its screen origin
    by adding its parent's. Host events carry `win` (originating HWND; 0 = main); the core converts to the virtual screen and then to the
    client of the target window (capture included); `GetCursorPos` returns virtual-screen coordinates. `WM_CLOSE` goes to the originating window.
    Activation returns to the owner when the active window is destroyed. We do **not** send `WM_ACTIVATE`/`WM_NCACTIVATE` when activating the bar: the
    main window's `WM_ACTIVATE` has a branch that destroys auxiliary windows, and the game only calls `SetActiveWindow` (no focus); focus
    stays on the main one. NC of the bar window: 3 px borders (`WS_DLGFRAME`) + 19 px caption (not 18 as in the spec; 19 = the
    `SM_CYCAPTION` the game itself uses to size it), client 558x48.
  - **`host.h`:** `host_window_update(HostWindow*)`, `host_window_destroy(id)`, `host_present(win, ...)` (frame per window).
  - **Headless:** keeps the last frame of each top-level window; `snap` composes the 640x480 desktop (teal background, windows visible in
    topmost/z order, gray border and navy caption strip on the top one / gray on the others, no text) in a 24-bit BMP;
    `snap-window name [hwnd-hex]` saves the 8-bit frame of a window. `frames/NNNN.bmp` and `frames.txt` continue (secondary ones with
    ` hwnd=...`). Script `tests/scripts/mainscreen.txt` (the clicks from the 07-macos-window-input-menus Log). Composite snap:
    `extracted/dumps/run-m2/mainscreen.png` (the 558x48 "GPM 2" bar with the 8 icons, on top of the main screen, at corner 0,0).
  - **macOS:** the main window is `gWin`; each visible secondary top-level window becomes a child `NSPanel` (`addChildWindow:ordered:`),
    with the game's title; `WS_CAPTION` -> `Titled` (+`Closable` if `WS_SYSMENU`), no caption -> `Borderless`; `HWND_TOPMOST` ->
    `floatingPanel`; `hidesOnDeactivate = NO`; `FullScreenAuxiliary` (stays in the same full-screen Space). The content is `cw*s x ch*s`
    (same scale as the main one) and the position is the Win32 offset between the client areas times the scale, from the corner of the
    main window's image. `layout_panels` runs on every size/scale/full-screen/mode change. Mouse and keyboard of the NSPanel generate `HostEvent` with its
    HWND; closing sends `WM_CLOSE` to it; `host_window_destroy`/hide closes or hides the panel. Verified with a temporary test hook
    (removed): main window on an 1800x1130 screen, panel at +3,+3 px from the client corner; in Pixel Perfect full screen
    (2x) the panel follows the centered image (3 px x 2 offset).
  - **Display (item 2):**
    - Window: integer scales 1..N (N = largest that fits in the usable area; `max_scale_for`), default on first use = N; resizing snaps
      to integers (`windowWillResize`), **Option** during the resize = free size with the game's proportion and sharp
      bilinear filter (the free size is not saved). Nearest filter whenever the scale in physical pixels is an integer.
    - Native full screen (own Space): **Pixel Perfect** (largest integer in physical pixels, black borders) and **Fill Screen** (largest
      scale with the proportion; sharp bilinear = a `CGBitmapContext` without interpolation enlarges by k = floor(scale in px) and the `CALayer`
      (linear filter) takes it to the final size; we chose this as the simplest option).
    - View menu: "Actual Size (1x)" Cmd-1, "2x".."Nx" (Cmd-2..4; the others without shortcut), "Zoom to Fit Screen" (Cmd-0), "Enter Full Screen"
      (Ctrl-Cmd-F; becomes "Exit Full Screen"), "Full Screen Mode" submenu (Pixel Perfect / Fill Screen). GPM2 > Settings... (Cmd-,):
      window scale, full-screen mode, "Open in full screen", game folder (show/change; changing asks for a restart).
    - Persistence (`NSUserDefaults`, domain `com.brunocastello.gpm2`): `ViewScale`, `FullScreenMode`, `OpenFullScreen`, `GameFolder`,
      `ExePath` and the window position through `setFrameAutosaveName:@"GPM2MainWindow"` (the size always follows the scale). CLI: `--scale N`,
      `--fullscreen`, `--fill` apply only to the run and are not saved.
  - **Open items:** (1) the bar window appears at (0,0) of the virtual desktop, that is, on top of the corner of the main window's area
    (as the game asks: x=y=0); (2) if the user drags the NSPanel, the guest is not notified (`WM_MOVE`); (3) visuals in the app (panel, full screen
    with panel, Fill) were not seen by me (no `screencapture`); (4) `WM_ACTIVATE`/focus between windows stayed minimal (see above);
    (5) on small screens (e.g. 1440x900) the largest integer scale that fits is 1x, which is literal from the spec.
  - **Rebuild/open:** `make -C runtime app GEN=extracted/dumps/recomp-f1/fuzz-run/gen` and
    `open runtime/build/GPM2.app --args --game "<original/GPM2>" --exe "<original/GPM2 Updates v1.02b/Gpm2.exe>"`
    (optional: `--scale N`, `--fullscreen`, `--fill`). Headless script: `gpm2run ... --fixed-time 851990400 --script tests/scripts/mainscreen.txt`.
