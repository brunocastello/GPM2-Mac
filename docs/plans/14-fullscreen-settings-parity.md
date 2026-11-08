# Settings parity, Zoom to Fit and full screen (Tiger and OS 9)

## Goal
Bring to `runtime/host/carbon/host_carbon.c` (Tiger/Leopard) and `runtime/host/os9/host_os9.c` what modern macOS has in `macos/host_mac.m`:
View > Zoom to Fit Screen, Enter/Exit Full Screen, Full Screen Mode (Pixel Perfect, Fill Screen, Stretch) and the Settings window
(Window scale, Full screen mode, Open in full screen, Game folder + Change). No resolution switching in any build.

## Decisions
- OS 9 (user request): full screen = frameless window covering the main screen's desktop, below the menu bar (the game's menus are in it), with the 3 modes and a black background.
- Tiger: frameless window covering the main screen, `SetSystemUIMode(kUIModeAllHidden, kUIOptionAutoShowMenuBar)`; the menu appears when the mouse goes to the top, as in the modern one.
- Changing the frame = recreating the main window (class/proc do not change during its life).
- Shortcuts: Cmd-0 Zoom; Tiger Cmd-Ctrl-F; OS 9 Cmd-F (Control alters the charCode in classic MenuKey).
- Period names: "Preferences..." (Tiger: standard item of the app menu; OS 9: File > Preferences, modal with OK/Cancel). Tiger is non-modal, applies immediately, like the modern one.

## OS 9: ALREADY DONE (do not touch)
Reference for the same design: `gFull`, `img_update`, `scx/scy`, `set_fullscreen`, `view_menu_checks`, `raise_dialogs`.
It also fixes: the Window Size submenu had ID 128, the same as the game's 1st menu (now VIEW_SIZE=123, VIEW_FMODE=124).

## Tiger: to implement in `host_carbon.c` (this file only)
Prefs (CFPreferences): `ViewScale` (already exists), `FullScreenMode` (int), `OpenFullScreen` (int).

1. **Globals right after `sc()`**: `gFull, gFullMode, gOpenFull`; `gImgX, gImgY, gImgW, gImgH` (int); `gSx, gSy` (float); `gFitScale`.
   `img_update()`: normal window: gSx=gSy=gScale, origin 0, gImgW=sc(GAME_W), gImgH=sc(GAME_H).
   Full: `CGDisplayBounds(CGMainDisplayID())` gives dw,dh; s=min(dw/GAME_W, dh/GAME_H) as float; mode 2: W=dw,H=dh; mode 0: s=floorf(s); modes 0/1: s<1 becomes 1, W/H = rounded GAME_*s.
   gSx=W/GAME_W, gSy=H/GAME_H, gImgX=(dw-W)/2, gImgY=(dh-H)/2. `scx/scy` = floorf(v*gSx+0.5f).
2. **`draw_w`**: W,H = bounds. If main and gFull: fill (0,0,W,H) black with CGContextSetRGBFillColor/FillRect, destination = the image rectangle; otherwise, the whole window.
   In CG y goes up: `CGRectMake(dx, H-dy-dh, dw, dh)`. Interpolation: None if `dw % w->pw == 0 && dh % w->ph == 0`, otherwise High.
3. **`layout_w`**: main and gFull: return. Children: `x = ox + gImgX + scx(...)`, `y = oy + gImgY + scy(...)`, size scx(cw)/scy(ch). `relayout_all` calls `img_update()` first.
   `win_handler` (BoundsChanged, resize): after `gScale = ...` call `img_update()`.
4. **`w_create`**: split out `static int w_make_ref(W *w, const HostWindow *hw, int main)`, with the WindowRef creation in a local variable (from `WindowAttributes at` to the end); only writes `w->ref` on success.
   Main and gFull: `kPlainWindowClass`, attrs only `kWindowStandardHandlerAttribute`, rect = screen bounds, `SetWindowContentColor` black, without restoring WinX/WinY.
5. **Persistence**: helper `pref_int_set(CFStringRef, int)` (with AppSynchronize) and `scale_save()` (ViewScale + sync) replacing the two CFNumber snippets of ViewScale. Today only `pref_set` synchronizes, so ViewScale/WinX/WinY may be lost on exit: register `atexit` with CFPreferencesAppSynchronize in `main` (check that `win32_exit` in win32/core.c uses exit and not _exit; report if not).
6. **View menu in `do_menu`**, after Window Size, in this order: "Zoom to Fit Screen" (CMD_FIT, key '0'), separator (`kMenuItemAttrSeparator`), "Enter Full Screen" (CMD_FULL, `SetMenuItemCommandKey` 'F' + `SetMenuItemModifiers(kMenuControlModifier)`), "Full Screen Mode" (submenu: CMD_FMODE+0..2).
   IDs: CMD_FULL 0x46530000u, CMD_FIT 0x46530001u, CMD_FMODE 0x46530010u. Store MenuRefs/indices in globals (View is recreated on each `do_menu`) and `gWszT10[]`.
   `view_refresh()`, called at the end of do_menu and on every change: check in Window Size if `!gFull` and gScale matches t10/10 (tolerance 0.01); check on the current mode; Enter/Exit title via `SetMenuItemTextWithCFString`.
7. **Before `mouse_handler`**:
   `raise_over_main()`: BringToFront on the visible children, on the `gD[i].ref` in use and on gPrefWin if visible.
   `set_fullscreen(int on)`: return if equal or without ref; stores old; gFull=on; img_update; w_make_ref(&gW[0], &gW[0].hw, 1), and if it fails reverts; `SetSystemUIMode(on ? kUIModeAllHidden : kUIModeNormal, on ? kUIOptionAutoShowMenuBar : 0)`; ShowWindow; SelectWindow; DisposeWindow(old); raise_over_main; relayout_all; on exit ConstrainWindowToScreen; view_refresh.
   `set_full_mode(int, persist)` and `scale_apply(float)` (stores, relayout_all; if !gFull, Constrain; view_refresh; prefs_refresh).
8. **`mouse_handler`**: main and gFull: on click, after SelectWindow call raise_over_main (otherwise a dialog stays behind the full screen). Coordinates: subtract gImgX/gImgY on the main, divide by gSx/gSy. Main and gFull outside the image: mouse down => `return noErr` with no event; move/drag without a button => same.
9. **`cmd_handler`**: CMD_FULL, CMD_FIT (scale_apply(gFitScale)), CMD_FMODE+n, `kHICommandPreferences` (show_prefs, with forward decl); CMD_SCALE now uses scale_apply.
   `start_timer`: `EnableMenuCommand(NULL, kHICommandPreferences)`.
10. **Preferences window** (after `resolve_files`): gPrefWin, `kDocumentWindowClass`, `kWindowCompositingAttribute|kWindowStandardHandlerAttribute|kWindowCloseBoxAttribute`, ~470x170, title "Preferences", RepositionWindow centered; kEventWindowClose => HideWindow.
    Same layout as the modern one (right-aligned labels via ControlFontStyleRec at x 10..140; controls at x=150): popup "Window scale:" (same t10 loop as the menu), popup "Full screen mode:", auto-toggle checkbox "Open in full screen", "Game folder:" with static text (`kControlStaticTextCFStringTag`, middle truncation with `kControlStaticTextTruncTag`) and button `cfs("Change\x85")`.
    Popups as in HC_COMBO_LIST: CreateNewMenu + `InsertMenu(m, kInsertHierarchicalMenu)` + CreatePopupButtonControl + SetControl32BitMaximum. Outside gMenuIds (cannot be deleted by do_menu).
    One kEventControlHit handler per control, with a tag in userData: scale (scale_apply), mode (set_full_mode), open full (pref_int_set), Change (nav_pick folder; `is_game_folder` otherwise `alert_need_102b`; pref_set GameFolder; host_message_box "Restart required" / "The new game folder will be used the next time GPM 2 starts.", 0x40).
    `prefs_refresh()`: synchronizes the controls; if gScale is not in the list, popup with value 0. No effect if the window does not exist.
11. **`main`**: read FullScreenMode (outside 0..2 becomes 0) and OpenFullScreen. `gFitScale` via `GetAvailableWindowPositioningBounds(GetMainDevice(), &r)`: min(w/GAME_W, (h-22)/GAME_H), floor at 0.01, minimum 1. The clamp of gScale becomes the larger of gMaxScale and gFitScale. Before w_create: `gFull = gOpenFull; img_update();`; after ShowWindow, if gFull, SetSystemUIMode.

## Constraints
C99/GCC4, SDK 10.4u: no API later than 10.4. Do not touch the rest of the file. Builds only in CI.

## Log
- 2026-11-05: implemented in both backends, with a final diff review (fixed: title preserved when recreating the window; OS 9 port when discarding the window; OS 9 window kept on the desktop when increasing the scale). Not yet compiled nor tested.
- 2026-11-05: commit `2dec969`, run 41958184102: `build-ppc`, `build-os9` and `be-test` green on the first build; artifacts in extracted/dumps/ppc and os9. User test pending.
- 2026-11-05: user test OK on both builds. Next item: Help in the system menu bar (HANDOFF, pending item 2).
