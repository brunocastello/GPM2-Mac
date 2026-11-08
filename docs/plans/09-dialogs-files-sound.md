# M3: native controls and dialogs, files (Open/Save) and sound

**Parent:** 07-macos-window-input-menus/013b. **Date:** 2026-11-02..
**Starts after 08-secondary-windows-display**, because it uses the support for several top-level windows.

## Goal

Everything that still returns a default value starts to work:
- Win32 controls (EDIT, LISTBOX, COMBOBOX, BUTTON, STATIC);
- the 22 `DIALOG` dialogs as native macOS panels;
- Open/Save as `NSOpenPanel`/`NSSavePanel`;
- sound (`sndPlaySoundA` and MCI `waveaudio`).

With this, the user can save, load, type names and values and hear
the game.

## Facts gathered (lead)

**Dialogs** (`extracted/dumps/exe/dialogs.json`): 22 templates, font
MS Sans Serif 8.

- From the normal game:
  - `EDITAMOUNT` "Change Details" (Edit + 2 buttons);
  - `RACE` "Race Set-up" (9 buttons, Edit);
  - 101 (Edit + 2 buttons), 102 (3 buttons);
  - 103 (Combo, List, 5 buttons), 104 (Combo, 3 buttons);
  - 107 (Edit + 2 buttons), 109/113/136 "Modal".
- Development and network:
  - `CHEATING`, `DRIVER_EDIT`, `TRACK_EDIT`, 111 "DebugWindow - Setup";
  - 105/106 "Data to send.", 112/134/135/137/139/140 (connection, modem,
    chat, DirectPlay).

**Calls:**
- `DialogBoxParamA`: about 22 sites;
- `CreateDialogParamA` (modeless): at `0x00507D60`, `0x00509B20`,
  `0x00509C50` and in the CRT;
- `GetOpenFileNameA`/`GetSaveFileNameA`: at `0x004AD420`/`0x004AD480`.

**Controls outside dialogs:** at boot the game creates a child `EDIT`
(`WS_CHILD`, size 0, ra `0x00504E03`). It seems to be the text input
of the screens drawn by the game. Check in the decompilation how this EDIT is used
(focus, `WM_CHAR`, `WM_GETTEXT`, `EM_*`).

**Sound:**
- `sndPlaySoundA(file, flags)` (`SND_ASYNC|SND_NODEFAULT`, sometimes
  `SND_LOOP`), in `FUN_004b2dc0`/`FUN_004b3e90`. The file is looked up in the
  current folder and in the game directory. There is a `null.wav` used to stop.
- MCI (`FUN_004b3770` and neighbors): `MCI_OPEN` with
  `MCI_OPEN_TYPE|MCI_OPEN_ELEMENT` ("waveaudio" + file), `MCI_PLAY` with
  `MCI_NOTIFY` (1) and optional `MCI_FROM`, `MCI_STATUS` (0x814),
  `MCI_STOP`/`MCI_CLOSE`.
  - **`MCI_NOTIFY` requires posting `MM_MCINOTIFY` (0x3B9) to the callback
    window when finished**, with `MCI_NOTIFY_SUCCESSFUL` (1), or
    `MCI_NOTIFY_ABORTED`/`SUPERSEDED` (4/2) when playback is interrupted.
    The game probably chains the commentary through there. Check in the WndProc.
  - `mciGetErrorStringA`.
- `waveOutOpen`/`waveOutClose`: device detection only. Return success
  with a handle.
- **WAVs:** 862 files, all PCM:
  - 11025 Hz/8-bit mono (789);
  - 22050 Hz/16-bit stereo and mono;
  - 22050/8-bit;
  - 11025/16-bit.
- The 05-win32-runtime-boot Log says that up to the menu only `sndPlaySoundA`,
  `mciSendCommandA` and `DragAcceptFiles` were called.

## Architecture

The same rules:
- the `runtime/win32/` core is portable C99, without Apple headers;
- the backend lives in `runtime/host/macos/` behind `host.h`;
- the headless always works.

### 1. Controls in the core (full model, no drawing)

Built-in classes with Win32 state and messages:

- **EDIT:**
  - text, selection and caret;
  - `ES_NUMBER`, `ES_PASSWORD` if they occur, `ES_AUTOHSCROLL`,
    `EM_LIMITTEXT`;
  - `WM_CHAR` (includes Backspace), `WM_KEYDOWN` (arrows, Home, End, Delete);
  - `WM_GETTEXT`/`WM_SETTEXT`/`WM_GETTEXTLENGTH`, `EM_SETSEL`/`EM_GETSEL`;
  - `EN_CHANGE`/`EN_SETFOCUS`/`EN_KILLFOCUS` to the parent via `WM_COMMAND`.
- **LISTBOX:**
  - `LB_ADDSTRING`/`INSERTSTRING`/`DELETESTRING`/`RESETCONTENT`;
  - `LB_GETCOUNT`/`GETCURSEL`/`SETCURSEL`/`GETTEXT`/`GETTEXTLEN`/`GETITEMDATA`/`SETITEMDATA`/`FINDSTRING`;
  - `LBS_SORT` (Windows sorting, case-insensitive);
  - `LBN_SELCHANGE`/`LBN_DBLCLK` if the class has `LBS_NOTIFY`.
- **COMBOBOX** (`CBS_DROPDOWNLIST`/`DROPDOWN`): the equivalent `CB_*`
  messages, `CBN_SELCHANGE`.
- **BUTTON:**
  - push, defpush, checkbox, autocheckbox, radio, autoradio (group by
    `WS_GROUP`), groupbox;
  - `BM_GETCHECK`/`SETCHECK`;
  - `BN_CLICKED`.
- **STATIC:** text (`SS_LEFT`/`CENTER`/`RIGHT`) and icon.
- **APIs:** `GetDlgItem`, `GetDlgItemTextA`/`SetDlgItemTextA`,
  `GetDlgItemInt`/`SetDlgItemInt`, `SendDlgItemMessageA`, `CheckDlgButton`,
  `IsDlgButtonChecked`, `CheckRadioButton`, `EnableWindow`, `SetFocus`/`GetFocus`
  and those the exe imports in this family.
- **Keyboard:** real keyboard focus.
  - `WM_CHAR`/`WM_KEYDOWN` go to the window with focus, including an invisible
    child EDIT of the game window.
  - Tab and Shift-Tab navigate between `WS_TABSTOP` controls inside the dialog
    (`IsDialogMessageA`). Enter = default button (`IDOK`), Esc = `IDCANCEL`.

### 2. Dialogs

- **`DialogBoxParamA`/`DialogBoxIndirectParamA`:**
  1. reads the template from `.rsrc`;
  2. creates the dialog HWND and the controls in the core;
  3. sends `WM_SETFONT` (if there is `DS_SETFONT`) and `WM_INITDIALOG` to the guest's DlgProc
     (`guest_call`), following the return rule (TRUE = focus on the
     first tabstop);
  4. runs the **nested modal loop on the guest's thread**, which dispatches the
     dialog's messages **and** keeps delivering timers and messages of the
     other windows, as Win32 does. The owner window is disabled;
  5. exits on `EndDialog(hDlg, result)` and returns `result`;
  6. `WM_COMMAND` from the controls, `WM_CLOSE` (= `IDCANCEL`) and the minimal
     DefDlgProc (`DM_GETDEFID`, Enter/Esc) are handled.
- **`CreateDialogParamA`:** modeless, with the same model; `IsDialogMessageA`
  in the game's loop.
- **macOS backend (native):**
  - each dialog becomes an **NSPanel** (modal: `NSApp runModalForWindow:`
    fired on the main thread; the guest waits for events through the queue and never
    blocks the main thread) with the template's title;
  - native controls at the position converted from DLU to Win32 pixels
    (MS Sans Serif 8 on Win95: `cxChar` 6, `cyChar` 13, `x = dlu*6/4`,
    `y = dlu*13/8`) and then to points, with a factor of 1.0;
  - small system font; widths may grow so the text fits;
  - **mapping:**
    - BUTTON push → `NSButton` (push bezel; the defpush becomes `keyEquivalent "\r"`);
    - checkbox and radio → `NSButton` switch/radio;
    - groupbox → `NSBox`;
    - EDIT → `NSTextField`;
    - LISTBOX → single-column `NSTableView` in an `NSScrollView`;
    - COMBOBOX dropdownlist → `NSPopUpButton`; dropdown → `NSComboBox`;
    - STATIC → non-editable `NSTextField`;
  - **two-way synchronization:**
    - user actions become Win32 messages to the guest (`BN_CLICKED`,
      `EN_CHANGE`, `LBN_SELCHANGE`...) and update the model;
    - guest changes to the model (`SetDlgItemText`, `LB_ADDSTRING`,
      `EnableWindow`, `CheckDlgButton`...) update the widget;
  - closing the panel = `WM_CLOSE`;
  - a hidden control (`WS_VISIBLE` absent or `ShowWindow(SW_HIDE)`) stays
    hidden.
- **Visible child controls in the game window** (outside dialogs, if
  they exist): native `NSView` overlaid at the scaled position. Invisible ones: no
  view, only the model and the focus.

### 3. Open/Save (`GetOpenFileNameA`/`GetSaveFileNameA`)

- **Reading `OPENFILENAMEA`:** `lpstrFilter` (description/pattern pairs),
  `lpstrInitialDir`, `lpstrFile`/`nMaxFile`, `lpstrDefExt`, `Flags`
  (`OFN_OVERWRITEPROMPT`, `OFN_FILEMUSTEXIST`...), `lpstrTitle`.
- **Panel:** `NSOpenPanel`/`NSSavePanel` with the filter's types (for example,
  `.GPS`). The default initial folder is the saves folder in the writable layer
  (`~/Library/Application Support/GPM2/<game saves folder>`).
- **Return:** the chosen path becomes a DOS path in `lpstrFile`. The `vfs`
  gains a **synthetic drive** for host paths outside the
  known folders, so the user can open the saves they have anywhere,
  for example `original/savegames/`.
  - Reading is allowed anywhere.
  - **Writing never in `original/`** nor in the game folder. If the user
    chooses to save there, show an alert and return cancel.
- **Output fields:** `nFileOffset`, `nFileExtension`. Cancel returns 0 and
  `CommDlgExtendedError` = 0.

### 4. Sound

- **Portable core (`runtime/win32/audio.c`):**
  - C mixer with fixed output of 44100 Hz, 16 bits, stereo;
  - PCM sources 8-bit (unsigned) and 16-bit, mono and stereo, 11025 and 22050 Hz,
    with linear resampling;
  - volume per source;
  - the WAV is read through the `vfs` (RIFF parser with `fmt ` and `data` chunks, ignoring
    the other chunks).
- **`sndPlaySoundA`:**
  - `SND_ASYNC` plays and returns;
  - `SND_SYNC` waits for the end (with the virtual-time loop in the headless);
  - `SND_LOOP` repeats;
  - `SND_NODEFAULT` plays nothing if the file is missing;
  - `NULL` or a new sound stops the previous one (`sndPlaySound` has a single channel);
  - returns TRUE/FALSE.
- **MCI `waveaudio`:**
  - commands: `MCI_OPEN` (device id), `MCI_PLAY` (`MCI_FROM`/`MCI_TO` in ms,
    `MCI_NOTIFY`), `MCI_STOP`, `MCI_PAUSE`, `MCI_SEEK`, `MCI_STATUS` (mode,
    position, length in ms), `MCI_SET` (time format ms) and `MCI_CLOSE`;
  - channel separate from `sndPlaySound`, because the two play together
    (effect + speech);
  - **`MM_MCINOTIFY` is posted to the guest's queue by the guest thread**, when
    the mixer signals the end. **Never call the guest from the audio thread.**
  - Real error codes (`MCIERR_*`) when the file is missing.
- **Backend:**
  - macOS: AudioQueue or AudioUnit (`kAudioUnitSubType_DefaultOutput`)
    pulling from the mixer by callback, with a small buffer (~20–40 ms);
  - headless: the mixer advances by virtual time with no output (or writes a WAV with
    `--audio-out` for debugging);
  - mute: the game's own "Sound" item already controls this; do not create an extra
    option.

### 5. App icon (user request)

- **Style:** icon in the current macOS standard (Big Sur or later):
  - 1024×1024 canvas with the centered 824×824 "squircle" (radius of about
    185, continuous curve), background with a sober gradient, subtle inner border and the
    standard shadow from Apple's template;
  - **the game's original icon sits in the center**, enlarged by nearest
    neighbor with an integer factor (for example, the 32×32 at 16× = 512), to keep
    the pixels sharp.
- **Icon source:** the icon resource of `Gpm2.exe` itself (`.rsrc`, largest
  size and depth available) or `APPICON1.ICO` from the game folder.
  - The icon is generated **at local build time**, from the user's
    files, and **is not committed**.
  - There is already a dump in `extracted/dumps/exe/icons/` for checking.
- **Tools:**
  - `tools/make_icon.py`, stdlib only (writes PNG with `zlib`), generates the
    `.iconset` with all sizes (16…1024, including the @2x);
  - `iconutil -c icns`, which already ships with macOS;
  - the `Makefile` puts `GPM2.icns` in `Contents/Resources` and `CFBundleIconFile`
    in `Info.plist`;
  - if the game files are not available (CI), the app is assembled without an
    icon, without failing.
- **Acceptance:** the icon appears in the Finder and the Dock (the user checks); a 1024 PNG
  goes to `extracted/dumps/run-m3/icon.png`.

## Tests

- **Headless with a script:** reach at least one dialog of the normal game and
  fill it in through the script. New commands:
  - `dlg-wait <template>`;
  - `dlg-set <id> <text>`;
  - `dlg-click <id>`;
  - `dlg-select <id> <index>`;
  - `file-answer <host path>`: ready answer for the next Open/Save;
  - `audio-log`: lists the sounds played, with name, start and end.
- **Script `tests/scripts/save_load.txt`:**
  1. Quick Start → main screen;
  2. File → Save As (`file-answer` = `out/writable/teste.gps`);
  3. File → Open on the same file;
  4. show that it returned to the main screen;
  5. snaps.
  - The saved file is a valid `GPM: v2.00`. Check with `tools/save_v2.py`
    from the GPM2-Mac project (`python3 -I`) or with the header.
- **Loading a real save:** `original/savegames/*.GPS` (read-only) through
  Open, with a snap of the resulting screen.
- **Sound:** the `audio-log` of the Quick Start script shows the WAVs played.
  Check that some `MM_MCINOTIFY` was delivered, if there is MCI on the path.
- **C unit tests** for the controls (EDIT/LISTBOX/COMBOBOX) and the mixer
  (resampling, unsigned 8 bits → signed 16 bits), in the style of
  `menu_test`.

## Constraints

As always:
- nothing installed;
- never write to `original/`;
- do not commit frames, WAVs, saves, `gen/` or outputs (`git status` before
  each commit);
- portable C99 core;
- translator untouched;
- CI green;
- small commits in Portuguese, with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`;
- no `screencapture`.

Visual and playability acceptance is the user's.

## Acceptance

- Controls, dialogs, Open/Save and sound implemented as above.
- Green headless scripts: dialog filled in; save → load; loading a real
  save; `audio-log`.
- The rebuilt `GPM2.app` opens and shows the native dialogs, plays sounds and saves
  and loads through the macOS panels (the user checks).
- `docs/plans/06-win32-imports.md` updated. Log below written.

## Log

- 2026-11-02 — specification created (session 5).
- 2026-11-02 (session 5) — **M3 implemented** (old development repo `14a4971` core, `f25e08e` macOS backend, `bb5d08d` scripts). Visual,
  sound and playability acceptance: the user's.
  - **"Help > About" bug (black window of odd size) — cause and fix.** About is a top-level window of the
    `CMBClass` class of its own (style `0xC40000`, no WS_VISIBLE, 400x100) that the game measures and draws with GDI: `DrawTextA` with
    `DT_CALCRECT|DT_WORDBREAK` to learn the size, `SetWindowPos` to resize, `DrawTextA` to draw, and a child
    `BUTTON` "OK". All of this was a stub (`DrawTextA`/`TextOutA`/`ExtTextOutA` returned 0, rectangle not computed, button never
    drawn) and no frame was presented. Fix: real GDI text (`TextOutA`, `ExtTextOutA`, `DrawTextA` with
    CALCRECT/WORDBREAK/alignment/`&` prefix, `GetTextExtentPoint32A`, `GetTextMetricsA`) with the game's own font
    (`FONT15.EDY`, 16x16 cell, proportional advance; MS Sans Serif does not exist in the files), `EndPaint` presents the frame,
    native children (button, static, edit, list) of windows that are not dialogs are drawn by the core (Win95 style) and
    receive the mouse (child hit test). Script `tests/scripts/about.txt` (uses `syscmd 40029`, what the Help >
    About menu fires; the menu drawn by the game sends the same): `extracted/dumps/run-m3/about.png`, `about-closed.png` (OK closes).
    The window caption text is not seen in the headless compositor (only the native app shows the title).
  - **Controls (`win32/controls.c`):** EDIT (text, selection/caret, `ES_NUMBER`, limit, `WM_CHAR`, arrows/Home/End/Delete,
    `EN_CHANGE/SETFOCUS/KILLFOCUS`), LISTBOX (all the `LB_*` used, case-insensitive `LBS_SORT`, item data, multi-select),
    COMBOBOX (mapped `CB_*`, dropdown and dropdownlist), BUTTON (push, defpush, check, autocheck, radio with `WS_GROUP` group,
    groupbox, `BM_*`), STATIC; all send `WM_COMMAND` to the parent. Native procs (`NP_BASE+CK_*`) also serve the game's `myedit`
    `CallWindowProcA`. `GetClassLongA` fixed (`GCL_STYLE = -26`, `GCL_WNDPROC = -24`). `DestroyWindow` destroys the children.
  - **Dialogs (`win32/dialog.c`):** template from `.rsrc` (by id or name), controls created in pixels (DLU x6/4, y13/8), `WM_INITDIALOG`
    (TRUE = focus on the 1st tabstop), nested modal loop on the guest thread that keeps delivering timers/painting of the other windows
    (`user_modal_step`), owner disabled (events discarded), Tab/Shift-Tab/Enter/Esc as `IsDialogMessage`, `EndDialog`,
    `DefDlgProcA`, `CreateDialogParamA`, `GetDlgItem`, `SendDlgItemMessageA`, `CheckDlgButton`, `IsDlgButtonChecked`, `CheckRadioButton`.
    macOS backend: each dialog is an `NSPanel` (child of the main one, centered on it) with `NSButton`/`NSTextField`/`NSTableView`/
    `NSPopUpButton`/`NSComboBox`/`NSBox` at the Win32 positions (factor 1.0, small system font; buttons grow to fit the text);
    the core resends each control that changes (`host_dlg_ctl`) and the actions come back as `HOST_EV_DLG`. Esc = IDCANCEL, closing = `WM_CLOSE`.
    **Decision:** the panel does not use `runModalForWindow` (it is not modal in AppKit); modality belongs to the core (owner disabled), which
    prevents AppKit's nested loop from blocking the main queue.
  - **Open/Save (`win32/filedlg.c`):** reads `OPENFILENAMEA` (filter in pairs with trailing spaces, initial name `*.gps` treated as
    empty, `lpstrFile`, `nFileOffset/Extension`), `NSOpenPanel`/`NSSavePanel` with the filter's extension and the cwd folder in the
    writable layer, returns `C:\GPM2\...` (game folder or writable layer) or `Z:\...` (**synthetic drive = host root**).
    Writing to `original/`, to the game folder or to any folder named "original": alert and cancel (and, in the vfs, redirects
    to `_blocked/`). Cancel returns 0.
  - **Save/load:** `tests/scripts/save_load.txt`: Quick Start -> main screen; File > Save As (`file-answer out/writable/teste.gps`)
    -> "Save Successful!"; File > Open on the same file -> "Load successful!" and back to the main screen. The saved file
    (38,502 bytes) has the header `GPM: v2.` equal to that of the real saves. `tests/scripts/load_real.txt` (`GPM2_SAVES=<folder>`): loads
    `original/savegames/MCLAREN.GPS` (read-only) -> "Load successful!", main screen with McLaren Mercedes and $19,958,450.
    Frames in `extracted/dumps/run-m3/` (`after-save`, `after-open`, `back-to-main`, `loaded`, `loaded-main`).
  - **Dialogs by script:** the normal game only opens the DIALOGs in deep screens (none appears on the Quick Start -> main menu path);
    `tests/scripts/dialogs.txt` opens them with the test hook `dlg-open <name|#id>` (no guest DlgProc) and uses `dlg-wait`, `dlg-set 1000 123456`
    (EDITAMOUNT: the EDIT becomes "123456"), `dlg-select`, `dlg-click`, `dlg-dump` (description of the controls in `run-m3/*.txt`: EDITAMOUNT, CHEATING, #103).
    The game's real `DlgProc`s were not exercised by script (accepted limitation); the native AppKit mirror was not seen by me.
  - **Sound:** `win32/audio.c` (mixer 44100 Hz/16 bits/stereo; WAV 8-bit unsigned and 16-bit, mono/stereo, any rate, linear resampling
    at load; unknown chunks ignored), `winmm.c` (`sndPlaySoundA` with SYNC/ASYNC/LOOP/NODEFAULT/NULL = stop, single channel;
    MCI `waveaudio` with OPEN/PLAY(FROM/TO/NOTIFY/WAIT)/STOP/PAUSE/RESUME/SEEK/STATUS/SET/CLOSE, `MCIERR_*` errors, `mciGetErrorStringA`;
    **`MM_MCINOTIFY` always posted by the guest thread**, codes 1 SUCCESSFUL, 2 SUPERSEDED, 4 ABORTED). `audio_render` (audio thread)
    only touches the mixer (mutex) and marks "finished". macOS: `AudioUnit` DefaultOutput (`audio_mac.m`). Headless: the mixer advances by the
    guest's clock (virtual with `--fixed-time`), `--audio-out x.wav` writes what would be played. `audio-log` of the `audio.txt` script:
    `null.wav` 3000-3004, `rintrolo.wav` 8008-81008, `null.wav`, then the music through MCI chained by `MM_MCINOTIFY`:
    `ig1_m.wav` 81012-280100, `ig1_mat.wav` 280100-480442, `ig2_m.wav` ... (`run-m3/audio.log`). Note: the WaitMessage wait
    does NOT advance virtual time to the end of the sound (otherwise the infinite chaining prevents `wait-idle`).
  - **Icon:** `tools/make_icon.py` (stdlib only) reads `APPICON1.ICO` from the game (BMP 8 bpp 32x32), builds the 824x824 squircle (superellipse n=5) on a
    1024 canvas with a slate gradient, inner border and shadow, the game's icon at 16x (512, nearest neighbor), generates the `.iconset` (16..1024, @2x),
    and the Makefile runs `iconutil -c icns` -> `Contents/Resources/GPM2.icns` (`CFBundleIconFile = GPM2`). Without the `.ICO` (CI) the app comes out without an icon. It is not
    committed. 1024 PNG in `run-m3/icon.png`.
  - **Display changes requested by the user (only `host/macos/host_mac.m`):** (1) resizing the window is free, proportion locked
    (nearest neighbor when the scale in pixels is an integer, sharp bilinear otherwise); snapping stays in the View menu items; Option is no longer
    needed; the current scale is stored (`ViewScale`, `double`). (2) "1.5x" item in View (between 1x and 2x) and in Preferences; `--scale 1.5`.
    (3) Third full-screen mode "Stretch" (fills the screen ignoring the proportion, sharp bilinear), in the Full Screen Mode submenu and in
    Preferences, persisted like the others (`FullScreenMode` = 2). Not seen by me (no `screencapture`).
  - **Tests:** `make m3test` (EDIT, LISTBOX, COMBOBOX, BUTTON, WAV 8-bit -> signed 16-bit, resampling, mixer/loop) and `menutest` green;
    scripts also green with `GUEST_CHECK` (`run-chk`).
  - **Still stub / pending:** `DragAcceptFiles`, DDE/DPLAY, `WinHelpA`, hooks; GDI lines/polygons (`LineTo`, `Polygon`, `Pie`); child controls
    of the network/chat windows (`ChatClass`, editing `CMBClass`) only as model + simple drawing; `SND_MEMORY`; STATIC icons; multiline EDIT
    in the native panel is simple (`NSTextField` with wrapping). A dragged panel does not notify the guest (`WM_MOVE`).
  - **Open:** `open runtime/build/GPM2.app --args --game "<original/GPM2>" --exe "<original/GPM2 Updates v1.02b/Gpm2.exe>"`
    (optional `--scale 1.5`, `--fullscreen`, `--fill`). Rebuild: `make -C runtime app GEN=extracted/dumps/recomp-f1/fuzz-run/gen`.
- 2026-11-02 (session 5, post-acceptance adjustments 8/10) — (1) View > "Window Size" (submenu 1x, 1.5x, 2x... with the same shortcuts). (2) Icon: `APPICON1.ICO` has an
  8 bpp image with a broken palette (corrupted car); the source is now `GPM.ICO` (4 bpp, 32x32), squircle with a white background, nearest neighbor 16x
  (`run-m3/icon.png`). (3) **Sound that stopped:** the game's speech (`sndPlaySoundA(buf, 7)` = `SND_MEMORY|ASYNC|NODEFAULT`) was ignored (returned 1 without playing)
  while the game closes the MCI music before the speech and only resumes it when timer 0x47 (speech duration) ends: silence until it resumes (or until the
  sound dialog is reopened). Now `SND_MEMORY` reads the RIFF image from guest memory and plays it on the `sndPlaySound` channel. Not heard by me; the
  chaining via `MM_MCINOTIFY` and the order of messages were reviewed without finding a fault. (4) GDI: the game uses `MoveToEx/LineTo` (2 px white pen),
  `Polygon` (4 points, 3D bars of the graphs) and `Pie` (angular indicator); implemented in the 8 bpp rasterizer (Bresenham with square cap, even-odd
  fill, sector by angle). `Rectangle/Ellipse/Arc/Polyline` are not imported by the game. Not exercised by script (deep screens).
- 2026-11-02 (session 5, freezes and modals) — (1) **Freeze "no guest memory for bitmap 640x460" (~44000 frames):** `DeleteObject` of a bitmap only
  freed the GDI object and not the pixels in the guest heap (only `realloc_surface` freed them); each per-frame `CreateCompatibleBitmap` leaked 640x460
  (~300 KB) until the 64 MB were exhausted. In addition `CreateCompatibleDC`/`WinGCreateDC` leaked the default 1x1 bitmap. Fix: `obj_free` frees the pixels
  of bitmaps and the DC's default bitmap (`DC.defbm`). Log: `win32_mem_report` (heap used/blocks/largest free, GDI objects by type, KB of bitmaps)
  every ~1M calls, in the final summary and on dying; `win32_die` now logs the last import and the `ra` and shows a native alert
  (`host_message_box`, MB_ICONERROR) before exiting. Test: script with a race (Race > OK > OK > OK) up to the aerial view and 300 clicks on the bottom
  bar: heap 2.8 MB, 7 bitmaps at the end (flat). **Not reproduced:** the race simulation itself (practice/pit stop) does not advance in
  headless (the game waits for a user action that I did not find by script); muted-sound and pit stop paths not exercised, the most likely cause
  of the report is the same leak. `GetTickCount` with `--fixed-time` advances 1 ms per query (busy wait). (2) View > Window Size: items go
  up to 2x even if the usable screen cannot hold it. (3) **Game modals:** the warnings "Save Successful!", "Load successful!", About etc. are NOT
  `MessageBoxA` nor DIALOG: they are the game's `CMBClass` window (`FUN_00505030(text, title)`, wndproc `FUN_005053e0`, text in `DAT_0057b3d4`, child OK
  button, not modal, closes with Enter/Esc/Space/WM_COMMAND -> `DestroyWindow`; 3 callers). Now, on `ShowWindow` of `CMBClass`
  (`user32.c`) the core shows `host_message_box` (NSAlert; "About..." becomes the standard macOS About panel with the game's text) and sends
  `WM_COMMAND` to the wndproc, which destroys the window (logical cycle preserved, never becomes a panel). The 19 `MessageBoxA` were already NSAlert. The DIALOGs
  (EDITAMOUNT, RACE, 101..., 109/113/136) stay in the generic M3 panels (low priority). (4) **Where to check `LineTo`/`Polygon`/`Pie`:**
  `LineTo` at `0x457b20` and `Pie`+`LineTo` at `0x457ed0` (angular indicators), called by `0x46ac90` (Suspension screen: Front/Rear, car
  design), `0x46b130` (design/Design) and `0x45d700/0x45d7e0` (**Wind Tunnel**, "Straights/Corners"); `Polygon` (3D bars) at `0x47c3e0`, called by
  `0x47bbc0` (Races/Sponsorship Deals/Merchandising graphs, probably Finance > Profit and Loss), `0x4e2d40` (ACHIEVEMENT) and `0x51b890`
  (Results: "POSITION AT END OF EACH LAP", position-per-lap graph). I did not reach those screens by script (Finance > P&L did not open on click).

- 2026-11-02 (session 5b, Help and modals) — (1) About and all modals (the NSAlert of `MessageBoxA` and of the `CMBClass` window) now open as a sheet of the main
  window (`beginSheetModalForWindow` + `runModalForWindow`, centered on it); without a visible main window, `runModal`. The About panel has centered text and
  is recentered over the main window. (2) **Help:** `GPMHELP.HLP` (WinHelp 3.1, minor 21, uncompressed, no |Phrases, 15 topics, 45 contexts by hash) is
  converted by `tools/hlp2html.py` (stdlib; B+tree directory, |SYSTEM, |TOPIC, |CONTEXT, |FONT, text/link commands; TOPICOFFSET = block<<15 + accumulated
  text of the block; hash links become `topicN.html#hXXXXXXXX`). The game only calls `WinHelpA` with HELP_INDEX (3, `0x40a750`, Help menu) and HELP_QUIT (2, `0x40793x`);
  `WinHelpA` -> `host_help` (index -> `NSApp showHelp:`; HELP_CONTEXT -> anchor `ctx<N>`; QUIT = nothing). `make app` generates `GPM2.app/Contents/Resources/GPM2.help`
  (Help Book + index via `hiutil` if it exists) and the `CFBundleHelpBookFolder/Name` keys only when the .HLP exists (CI: app without Help). macOS Help menu > "GPM2 Help".
  There are no decoded embedded images (none found). The game's menu may have its own "Help" in addition to macOS's. Not seen by me on screen
  (only plist lint and a 4 s launch). (3) **Network:** study in `10-network-link-play.md` (IDirectPlay2A, 12 methods; not implemented, ~2000+ lines).

## Log 2026-11-02 (F7, final app)
- `make app` now generates a universal GPM2.app (arm64+x86_64, minos 11.0): sub-make per architecture + `lipo`; ad-hoc `codesign --deep` at the end. No warnings.
- Info.plist: version 1.0 (build 1), copyright, games category, HighResolution.
- Rosetta present: headless quickstart x86_64 vs arm64 with identical outputs (frames/snaps).
- New `GPM2 --check-install <folder>` (same detection as first use, without UI/defaults): the game folder and its parent folder find the 1.02B exe in "GPM2 Updates v1.02b"; a folder without the game => NOT FOUND (the app would ask).
